// SPDX-License-Identifier: MIT
//
// Multi-thread x86 memory-ordering litmus tests for the translator core.
//
// x86 guarantees total store order (TSO): a thread's stores become visible to other
// threads in program order, and its loads are not reordered with each other or with
// later stores. ARM64 is weaker, and macOS gives ordinary processes no way to switch on
// the hardware TSO mode of Apple silicon, so the core emulates TSO in software
// (FEX_TSOENABLED, FEX_VECTORTSOENABLED, FEX_MEMCPYSETTSOENABLED). The instruction tests
// run with it on but are single-threaded; this tool checks ordering between threads.
//
// Each test is a classic litmus test: two guest threads run a few memory accesses on
// shared variables, many times over, and the tool counts the outcomes. Some outcomes are
// forbidden under TSO but allowed (and produced) by Apple silicon when the accesses are
// translated to plain loads and stores. With emulation on, a forbidden outcome must never
// appear. With emulation off, the same harness must see it, which shows that the test can
// tell the two apart. Every test must also show at least one outcome that is possible only
// when its two threads overlap; otherwise it fails as NOT RACING, in every mode, since a
// test whose threads never overlap passes for nothing. Store buffering (SB) is allowed
// under TSO and its relaxed outcome must appear as well.
//
// Further tests: store buffering with `mfence` or a locked instruction between store and
// load (forbidden in every mode), and counting tests in which every thread of every group
// runs locked operations on one misaligned word (crossing 16 bytes, a 128-byte line or a
// 16K page) and the total must be exact.
//
// Harness, all in hand-assembled x86-64 guest code:
//  - Each iteration uses a fresh slot of a ring of slots, so no per-iteration reset
//    barrier is needed: thread 0 clears the slot two iterations ahead.
//  - The threads of a group meet at a barrier before every iteration (a `lock xadd` on a
//    shared counter, a spin on the counter, then `mfence`, which is a full barrier in
//    every mode), then each waits a small random number of loop turns from its own table,
//    so that the two sides overlap at varying offsets.
//  - Variables sit in separate cache lines (Apple silicon reports 128-byte lines), and the
//    reader touches the data line before the barrier so that its copy is stale, which is
//    the case where the hardware reorders.
//  - Thread pinning does not exist on macOS; threads ask for the user-interactive quality
//    of service and the tool relies on many iterations in several groups at once.
//
// Several groups run each test at once on the same translated code, and each run copies
// the code to a fresh address, so every run translates it again. In the unaligned tests
// with emulation on, every thread of every group then takes the alignment fault on the
// same instructions within moments of each other (the barrier lines them up), which is
// the concurrent case of the fault-time back-patching.
//
//   FEXCoreOrderingTest [--tso on|off|scalar] [--expect tso|weak|scalar|report]
//                       [--iterations N] [--runs R] [--groups G] [--soak]
//                       [--only name[,name...]] [--list]
//
// --tso on (default) switches on all three emulation settings, off switches all off, and
// scalar switches on only the general one (vector and string-instruction accesses stay
// plain). What each test must show depends on --expect, which by default follows --tso:
//  - tso (with --tso on): no forbidden outcome in any test.
//  - weak (with --tso off): the forbidden outcome at least once in each test that an M4
//    has been measured to break, none in the tests that hold in every mode (fences, locked
//    instructions), and only a report for the rest.
//  - scalar (with --tso scalar): the vector-to-vector and string-to-string tests must
//    break, the tests in which a scalar access orders its neighbour must hold.
//  - report: counts only.
// In every mode a test fails if its threads never overlapped, if a guest thread did not
// finish, or if a counting test's total is wrong. --only takes exact test names (an
// unknown one is an error). --soak runs far more iterations (minutes instead of seconds).

#include "Common/HostFeatures.h"
#include "CoreThreadSetup.h"
#include "DummyHandlers.h"
#include "MacOSHostFaults.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/fextl/memory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <vector>

#ifdef __APPLE__
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

namespace {

// ---------------------------------------------------------------------------------------
// A minimal x86-64 encoder: just the instructions the tests need. Memory operands are
// always [base + disp32].
enum Reg : int { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum Alu : int { ADD = 0, AND = 4, SUB = 5, CMP = 7 };
enum Cond : uint8_t { CC_B = 0x2, CC_Z = 0x4, CC_NZ = 0x5 };

struct Asm {
  std::vector<uint8_t> B;

  void U8(uint8_t V) {
    B.push_back(V);
  }
  void U32(uint32_t V) {
    for (int i = 0; i < 4; ++i) {
      U8(V >> (i * 8));
    }
  }
  void U64(uint64_t V) {
    for (int i = 0; i < 8; ++i) {
      U8(V >> (i * 8));
    }
  }
  void Rex(bool W, int R, int Base) {
    const uint8_t Value = 0x40 | (W << 3) | (((R >> 3) & 1) << 2) | ((Base >> 3) & 1);
    if (Value != 0x40) {
      U8(Value);
    }
  }
  void Mem(int R, int Base, int32_t Disp) {
    U8(0x80 | ((R & 7) << 3) | (Base & 7));
    if ((Base & 7) == 4) {
      U8(0x24); // SIB for rsp/r12 as base
    }
    U32(Disp);
  }
  void RR(int R, int RM) {
    U8(0xC0 | ((R & 7) << 3) | (RM & 7));
  }

  void MovImm(int R, uint64_t Imm) {
    Rex(true, 0, R);
    U8(0xB8 + (R & 7));
    U64(Imm);
  }
  void Load(int Dst, int Base, int32_t Disp) {
    Rex(true, Dst, Base);
    U8(0x8B);
    Mem(Dst, Base, Disp);
  }
  void Store(int Base, int32_t Disp, int Src) {
    Rex(true, Src, Base);
    U8(0x89);
    Mem(Src, Base, Disp);
  }
  void StoreImm(int Base, int32_t Disp, int32_t Imm) {
    Rex(true, 0, Base);
    U8(0xC7);
    Mem(0, Base, Disp);
    U32(Imm);
  }
  void Mov(int Dst, int Src) {
    Rex(true, Src, Dst);
    U8(0x89);
    RR(Src, Dst);
  }
  void AluImm(Alu Op, int R, int32_t Imm) {
    Rex(true, 0, R);
    U8(0x81);
    RR(Op, R);
    U32(Imm);
  }
  void Add(int Dst, int Src) {
    Rex(true, Src, Dst);
    U8(0x01);
    RR(Src, Dst);
  }
  // Flags of A - B.
  void Cmp(int A, int B) {
    Rex(true, B, A);
    U8(0x39);
    RR(B, A);
  }
  void Test(int A, int B) {
    Rex(true, B, A);
    U8(0x85);
    RR(B, A);
  }
  void Shl(int R, uint8_t N) {
    Rex(true, 0, R);
    U8(0xC1);
    RR(4, R);
    U8(N);
  }
  void Inc(int R) {
    Rex(true, 0, R);
    U8(0xFF);
    RR(0, R);
  }
  void Dec32(int R) {
    Rex(false, 0, R);
    U8(0xFF);
    RR(1, R);
  }
  void MovzxByte(int Dst, int Base, int32_t Disp) {
    Rex(false, Dst, Base);
    U8(0x0F);
    U8(0xB6);
    Mem(Dst, Base, Disp);
  }
  void LockXadd(int Base, int32_t Disp, int Src) {
    U8(0xF0);
    Rex(true, Src, Base);
    U8(0x0F);
    U8(0xC1);
    Mem(Src, Base, Disp);
  }
  void Mfence() {
    U8(0x0F);
    U8(0xAE);
    U8(0xF0);
  }
  void MovdquLoad(int X, int Base, int32_t Disp) {
    U8(0xF3);
    Rex(false, X, Base);
    U8(0x0F);
    U8(0x6F);
    Mem(X, Base, Disp);
  }
  void MovdquStore(int Base, int32_t Disp, int X) {
    U8(0xF3);
    Rex(false, X, Base);
    U8(0x0F);
    U8(0x7F);
    Mem(X, Base, Disp);
  }
  void RepMovsb() {
    U8(0xF3);
    U8(0xA4);
  }
  void RepStosq() {
    U8(0xF3);
    U8(0x48);
    U8(0xAB);
  }
  void Hlt() {
    U8(0xF4);
  }

  void LockCmpxchg(int Base, int32_t Disp, int Src) {
    U8(0xF0);
    Rex(true, Src, Base);
    U8(0x0F);
    U8(0xB1);
    Mem(Src, Base, Disp);
  }
  // xchg with a memory operand is always locked.
  void Xchg(int Base, int32_t Disp, int R) {
    Rex(true, R, Base);
    U8(0x87);
    Mem(R, Base, Disp);
  }
  void JmpBack(size_t Target) {
    U8(0xE9);
    U32(static_cast<int32_t>(Target - (Here() + 4)));
  }

  size_t Here() const {
    return B.size();
  }
  void JccBack(Cond C, size_t Target) {
    U8(0x0F);
    U8(0x80 | C);
    U32(static_cast<int32_t>(Target - (Here() + 4)));
  }
  size_t JccFwd(Cond C) {
    U8(0x0F);
    U8(0x80 | C);
    const size_t At = Here();
    U32(0);
    return At;
  }
  void Bind(size_t At) {
    const int32_t Rel = static_cast<int32_t>(Here() - (At + 4));
    memcpy(&B[At], &Rel, 4);
  }
};

// ---------------------------------------------------------------------------------------
// Register use in the guest loop, set by the host before the thread starts:
//   r15 iteration, r14 iteration count, r13 this thread's result array, r11 slot ring,
//   r10 the group's barrier counter, r9 this thread's delay table, r8 the run's shared
//   area (constants, and the counters every thread of every group updates), rbp barrier
//   target (counts up). Per iteration: rbx slot, r12 this iteration's result entry. rax,
//   rcx, rdx, rsi, rdi and xmm0 to xmm2 are scratch. rsp is never used as a base: the
//   core treats stack accesses as thread-local and does not order them.
constexpr uint64_t FULL = 0x0101'0101'0101'0101ULL;
constexpr uint64_t COPY_BYTE = 0x22;
constexpr uint64_t STOS_VALUE = 0x3333'3333'3333'3333ULL;
constexpr int RESULT_SHIFT = 6; // 64 bytes (8 qwords) of results per thread per iteration
constexpr uint32_t DELAY_ENTRIES = 4096;

// The shared area: constants first, then counters at misaligned addresses.
constexpr size_t SHARED_SIZE = 0x8000; // two 16K pages
constexpr int ConstFull = 0;           // 16 bytes of FULL
constexpr int ConstCopy = 64;          // 64 bytes of COPY_BYTE
constexpr int ConstOne = 128;          // the qword 1
constexpr int CtrSplit16 = 0x1000 + 12;        // crosses a 16-byte boundary
constexpr int CtrSplitLine = 0x1000 + 0x100 + 124; // crosses a 128-byte line
constexpr int CtrSplitPage = 0x4000 - 4;           // crosses the 16K page boundary
constexpr int CasSplit16 = 0x1000 + 0x400 + 12;
constexpr int XchgLock = 0x1000 + 0x800 + 12;
constexpr int XchgCounter = 0x1000 + 0xC00;
constexpr int Increments = 4; // locked operations per thread per iteration

struct Layout {
  uint32_t Stride = 512;
  uint32_t Slots = 256;
  int32_t X = 0;
  int32_t Y = 256;
};
constexpr Layout D {};

// What a mode requires of a test's forbidden outcome.
enum class Expect { Hold, Break, Either };

struct LitmusTest {
  const char* Name;
  const char* Description;
  Layout L;
  std::vector<int32_t> Reset; // qword offsets thread 0 clears in the slot ahead
  std::function<void(Asm&, int Role)> Pre;  // before the barrier (may be empty)
  std::function<void(Asm&, int Role)> Body; // the litmus accesses
  std::function<void(Asm&)> Final;          // thread 0, after a second barrier (may be empty)
  std::vector<std::string> Labels;
  std::vector<int> Forbidden;   // outcome indices TSO forbids
  std::vector<int> Interleaved; // outcomes only possible when the two threads overlap
  int Relaxed = -1;             // SB only: the relaxed outcome that TSO allows
  // With emulation on every test must hold. With it off (--expect weak) and with only the
  // general setting on (--expect scalar), these say what is required: Break means the
  // forbidden outcome must be seen at least once, Hold that it must never be, Either that
  // it is only reported. Break is set only where an M4 has been measured to show it.
  Expect Off = Expect::Break;
  Expect Scalar = Expect::Hold;
  std::function<int(const uint64_t* R0, const uint64_t* R1)> Classify;
  // Counting tests: checks the shared area after a run in which every thread did
  // `ThreadIterations` iterations in all; returns an empty string if it is right. A wrong
  // total counts as one forbidden outcome.
  std::function<std::string(const uint8_t* Shared, uint64_t ThreadIterations)> FinalCheck;
  // Runs this many times fewer iterations: every locked operation of the counting tests
  // takes a fault, a few microseconds each.
  uint32_t IterationDivisor = 1;
};

// Message passing outcome: flag and data each none, partial or all of what was written.
int State(uint64_t V, uint64_t Expected) {
  return V == 0 ? 0 : (V == Expected ? 2 : 1);
}
int StateAll(std::initializer_list<uint64_t> Values, uint64_t Expected) {
  bool AllZero = true, AllFull = true;
  for (auto V : Values) {
    AllZero &= V == 0;
    AllFull &= V == Expected;
  }
  return AllZero ? 0 : (AllFull ? 2 : 1);
}
std::vector<std::string> MPLabels() {
  std::vector<std::string> L;
  const char* S[] = {"none", "part", "all"};
  for (int f = 0; f < 3; ++f) {
    for (int d = 0; d < 3; ++d) {
      L.push_back(std::string("flag=") + S[f] + " data=" + S[d]);
    }
  }
  return L;
}
// Forbidden under TSO: the whole flag is seen but not all of the data written before it.
const std::vector<int> MPForbidden = {2 * 3 + 0, 2 * 3 + 1};
// Possible only while the writer is between its stores: no flag but some data, or part of
// the flag. (No flag and no data means the reader went first, all of both that it went
// second.)
const std::vector<int> MPInterleaved = {0 * 3 + 1, 0 * 3 + 2, 1 * 3 + 0, 1 * 3 + 1, 1 * 3 + 2};

LitmusTest MP(const char* Name, const char* Desc, Layout L = D) {
  LitmusTest t {Name, Desc, L};
  t.Labels = MPLabels();
  t.Forbidden = MPForbidden;
  t.Interleaved = MPInterleaved;
  return t;
}

void WarmData(Asm& A, int Role, int32_t Offset) {
  if (Role == 1) {
    A.Load(RAX, RBX, Offset);
  }
}

const std::vector<int32_t> Reset64 = {D.X, D.X + 8, D.X + 16, D.X + 24, D.X + 32, D.X + 40, D.X + 48, D.X + 56, D.Y, D.Y + 8};

// Reads the flag and the first and last qwords of 64 bytes of data with plain moves.
void ReadFlagAndCopied(Asm& A) {
  A.Load(RAX, RBX, D.Y);
  A.Load(RDX, RBX, D.X);
  A.Load(RCX, RBX, D.X + 56);
  A.Store(R12, 0, RAX);
  A.Store(R12, 8, RDX);
  A.Store(R12, 16, RCX);
}

void RepMovsbFrom(Asm& A, int SrcBase, int32_t SrcOff, int DstBase, int32_t DstOff, int32_t Count) {
  A.Mov(RSI, SrcBase);
  A.AluImm(ADD, RSI, SrcOff);
  A.Mov(RDI, DstBase);
  A.AluImm(ADD, RDI, DstOff);
  A.MovImm(RCX, Count);
  A.RepMovsb();
}

void RepStosqTo(Asm& A, int32_t DstOff, uint64_t Value, int32_t Count) {
  A.MovImm(RAX, Value);
  A.Mov(RDI, RBX);
  A.AluImm(ADD, RDI, DstOff);
  A.MovImm(RCX, Count);
  A.RepStosq();
}

const std::vector<std::string> TwoBitLabels = {"r0=0 r1=0", "r0=0 r1=1", "r0=1 r1=0", "r0=1 r1=1"};
int TwoBit(const uint64_t* R0, const uint64_t* R1) {
  return int(R0[0] & 1) * 2 + int(R1[0] & 1);
}

std::vector<LitmusTest> MakeTests() {
  std::vector<LitmusTest> T;

  // Store buffering, plain and with a fence or a locked instruction between the store and
  // the load (Dekker's pattern). Plain SB's relaxed outcome is allowed by x86 and must be
  // seen, which shows the threads race at all; with the fence or locked instruction x86
  // forbids it, and that must hold in every mode, since `mfence` and locked instructions
  // are translated to full barriers and acquire-release atomics regardless of the setting.
  {
    LitmusTest t {"SB", "store buffering (relaxed outcome allowed by TSO, must be seen)", D};
    t.Reset = {D.X, D.Y};
    t.Body = [](Asm& A, int Role) {
      A.StoreImm(RBX, Role == 0 ? D.X : D.Y, 1);
      A.Load(RAX, RBX, Role == 0 ? D.Y : D.X);
      A.Store(R12, 0, RAX);
    };
    t.Labels = TwoBitLabels;
    t.Relaxed = 0;
    t.Interleaved = {3};
    t.Off = t.Scalar = Expect::Hold; // nothing is forbidden
    t.Classify = TwoBit;
    T.push_back(std::move(t));
  }
  auto Dekker = [&T](const char* Name, const char* Desc, bool Locked) {
    LitmusTest t {Name, Desc, D};
    t.Reset = {D.X, D.Y};
    t.Body = [Locked](Asm& A, int Role) {
      A.StoreImm(RBX, Role == 0 ? D.X : D.Y, 1);
      if (Locked) {
        // A locked add of zero on a word of this thread's own, in another line.
        A.MovImm(RCX, 0);
        A.LockXadd(RBX, Role == 0 ? D.X + 128 : D.Y + 128, RCX);
      } else {
        A.Mfence();
      }
      A.Load(RAX, RBX, Role == 0 ? D.Y : D.X);
      A.Store(R12, 0, RAX);
    };
    t.Labels = TwoBitLabels;
    t.Forbidden = {0};
    t.Interleaved = {3};
    t.Off = t.Scalar = Expect::Hold;
    t.Classify = TwoBit;
    T.push_back(std::move(t));
  };
  Dekker("SB+mfence", "store buffering with mfence between store and load", false);
  Dekker("SB+lock", "store buffering with a locked instruction between store and load", true);

  {
    auto t = MP("MP", "message passing, scalar qwords");
    t.Reset = {D.X, D.Y};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.StoreImm(RBX, D.X, 1);
        A.StoreImm(RBX, D.Y, 1);
      } else {
        A.Load(RAX, RBX, D.Y);
        A.Load(RDX, RBX, D.X);
        A.Store(R12, 0, RAX);
        A.Store(R12, 8, RDX);
      }
    };
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + State(R1[1], 1);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP+spin", "message passing, reader spins on the flag (bounded) then reads the data");
    t.Reset = {D.X, D.Y};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.StoreImm(RBX, D.X, 1);
        A.StoreImm(RBX, D.Y, 1);
      } else {
        A.MovImm(RCX, 256);
        const size_t Loop = A.Here();
        A.Load(RAX, RBX, D.Y);
        A.Test(RAX, RAX);
        const size_t Got = A.JccFwd(CC_NZ);
        A.Dec32(RCX);
        A.JccBack(CC_NZ, Loop);
        A.Bind(Got);
        A.Load(RDX, RBX, D.X);
        A.Store(R12, 0, RAX);
        A.Store(R12, 8, RDX);
      }
    };
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + State(R1[1], 1);
    };
    T.push_back(std::move(t));
  }

  {
    LitmusTest t {"LB", "load buffering", D};
    t.Reset = {D.X, D.Y};
    t.Body = [](Asm& A, int Role) {
      A.Load(RAX, RBX, Role == 0 ? D.X : D.Y);
      A.StoreImm(RBX, Role == 0 ? D.Y : D.X, 1);
      A.Store(R12, 0, RAX);
    };
    t.Labels = TwoBitLabels;
    t.Forbidden = {3};
    t.Interleaved = {0};
    // Never seen on an M4 (none in 100 million tries with --soak), so not required.
    t.Off = Expect::Either;
    t.Classify = TwoBit;
    T.push_back(std::move(t));
  }

  {
    LitmusTest t {"S", "T0: x=2; y=1. T1: r=y; x=1. Forbidden: r=1 and x ends 2", D};
    t.Reset = {D.X, D.Y};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.StoreImm(RBX, D.X, 2);
        A.StoreImm(RBX, D.Y, 1);
      } else {
        A.Load(RAX, RBX, D.Y);
        A.StoreImm(RBX, D.X, 1);
        A.Store(R12, 0, RAX);
      }
    };
    t.Final = [](Asm& A) {
      A.Load(RAX, RBX, D.X);
      A.Store(R12, 0, RAX);
    };
    // T0 first gives r=1 x=1, T1 first r=0 x=2; r=0 x=1 needs them to overlap.
    t.Labels = {"r=0 x=1", "r=0 x=2", "r=1 x=1", "r=1 x=2"};
    t.Forbidden = {3};
    t.Interleaved = {0};
    t.Classify = [](const uint64_t* R0, const uint64_t* R1) {
      return int(R1[0] & 1) * 2 + int(R0[0] == 2);
    };
    T.push_back(std::move(t));
  }

  {
    LitmusTest t {"2+2W", "T0: x=1; y=2. T1: y=1; x=2. Forbidden: x and y both end 1", D};
    t.Reset = {D.X, D.Y};
    t.Body = [](Asm& A, int Role) {
      A.StoreImm(RBX, Role == 0 ? D.X : D.Y, 1);
      A.StoreImm(RBX, Role == 0 ? D.Y : D.X, 2);
    };
    t.Final = [](Asm& A) {
      A.Load(RAX, RBX, D.X);
      A.Store(R12, 0, RAX);
      A.Load(RAX, RBX, D.Y);
      A.Store(R12, 8, RAX);
    };
    // One thread entirely first gives x=1 y=2 or x=2 y=1; x=2 y=2 needs overlap.
    t.Labels = {"x=1 y=1", "x=1 y=2", "x=2 y=1", "x=2 y=2"};
    t.Forbidden = {0};
    t.Interleaved = {3};
    t.Classify = [](const uint64_t* R0, const uint64_t*) {
      return int(R0[0] == 2) * 2 + int(R0[1] == 2);
    };
    T.push_back(std::move(t));
  }

  // Vector accesses. With only the general setting on, a vector access next to a scalar
  // one is still ordered by the scalar access's release or acquire (MP-sse); only
  // vector-to-vector order depends on FEX_VECTORTSOENABLED, on the store side (MP-sse-st)
  // and on the load side (MP-sse-ld).
  {
    auto t = MP("MP-sse", "message passing, 16-byte SSE data (movdqu), scalar flag");
    t.Reset = {D.X, D.X + 8, D.Y};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.MovdquLoad(0, R8, ConstFull);
        A.MovdquStore(RBX, D.X, 0);
        A.StoreImm(RBX, D.Y, 1);
      } else {
        A.Load(RAX, RBX, D.Y);
        A.MovdquLoad(1, RBX, D.X);
        A.Store(R12, 0, RAX);
        A.MovdquStore(R12, 16, 1);
      }
    };
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + StateAll({R1[2], R1[3]}, FULL);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-sse2", "message passing, SSE data and SSE flag, both sides");
    t.Reset = {D.X, D.X + 8, D.Y, D.Y + 8};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.MovdquLoad(0, R8, ConstFull);
        A.MovdquStore(RBX, D.X, 0);
        A.MovdquStore(RBX, D.Y, 0);
      } else {
        A.MovdquLoad(1, RBX, D.Y);
        A.MovdquLoad(2, RBX, D.X);
        A.MovdquStore(R12, 0, 1);
        A.MovdquStore(R12, 16, 2);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return StateAll({R1[0], R1[1]}, FULL) * 3 + StateAll({R1[2], R1[3]}, FULL);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-sse-st", "message passing, SSE stores for data and flag, scalar loads");
    t.Reset = {D.X, D.X + 8, D.Y, D.Y + 8};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.MovdquLoad(0, R8, ConstFull);
        A.MovdquStore(RBX, D.X, 0);
        A.MovdquStore(RBX, D.Y, 0);
      } else {
        A.Load(RAX, RBX, D.Y);
        A.Load(RDX, RBX, D.X + 8);
        A.Store(R12, 0, RAX);
        A.Store(R12, 8, RDX);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], FULL) * 3 + State(R1[1], FULL);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-sse-ld", "message passing, scalar stores, SSE loads for flag and data");
    t.Reset = {D.X, D.X + 8, D.Y, D.Y + 8};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.MovImm(RAX, FULL);
        A.Store(RBX, D.X, RAX);
        A.Store(RBX, D.X + 8, RAX);
        A.Store(RBX, D.Y, RAX);
        A.Store(RBX, D.Y + 8, RAX);
      } else {
        A.MovdquLoad(1, RBX, D.Y);
        A.MovdquLoad(2, RBX, D.X);
        A.MovdquStore(R12, 0, 1);
        A.MovdquStore(R12, 16, 2);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return StateAll({R1[0], R1[1]}, FULL) * 3 + StateAll({R1[2], R1[3]}, FULL);
    };
    T.push_back(std::move(t));
  }

  // String instructions. As with vectors, a string access next to a scalar one is ordered
  // by the scalar release or acquire with only the general setting on (MP-movsb, MP-stosq,
  // MP-movsb-read). String-to-string order depends on FEX_MEMCPYSETTSOENABLED: stores in
  // MP-stos-stos and MP-movs-movs, loads in MP-movsb-read2. Architecturally a plain store
  // after a release store, and a plain load before an acquire load, may also be reordered;
  // an M4 shows the second (MP-flag-movsb) but has not shown the first (MP-flag-stosq).
  {
    auto t = MP("MP-movsb", "message passing, data written by rep movsb (64 bytes), scalar flag");
    t.Reset = Reset64;
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        RepMovsbFrom(A, R8, ConstCopy, RBX, D.X, 64);
        A.StoreImm(RBX, D.Y, 1);
      } else {
        ReadFlagAndCopied(A);
      }
    };
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + StateAll({R1[1], R1[2]}, COPY_BYTE * FULL);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-stosq", "message passing, data written by rep stosq (8 qwords), scalar flag");
    t.Reset = Reset64;
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        RepStosqTo(A, D.X, STOS_VALUE, 8);
        A.StoreImm(RBX, D.Y, 1);
      } else {
        ReadFlagAndCopied(A);
      }
    };
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + StateAll({R1[1], R1[2]}, STOS_VALUE);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-stos-stos", "message passing, data and flag both written by rep stosq");
    t.Reset = Reset64;
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        RepStosqTo(A, D.X, STOS_VALUE, 8);
        RepStosqTo(A, D.Y, 1, 1);
      } else {
        ReadFlagAndCopied(A);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + StateAll({R1[1], R1[2]}, STOS_VALUE);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-movs-movs", "message passing, data and flag both written by rep movsb");
    t.Reset = Reset64;
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        RepMovsbFrom(A, R8, ConstCopy, RBX, D.X, 64);
        RepMovsbFrom(A, R8, ConstOne, RBX, D.Y, 8);
      } else {
        ReadFlagAndCopied(A);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + StateAll({R1[1], R1[2]}, COPY_BYTE * FULL);
    };
    T.push_back(std::move(t));
  }

  // Writer for the tests where the reader uses string loads: eight qwords, then the flag.
  auto ScalarWriter64 = [](Asm& A) {
    A.MovImm(RAX, FULL);
    for (int i = 0; i < 8; ++i) {
      A.Store(RBX, D.X + i * 8, RAX);
    }
    A.StoreImm(RBX, D.Y, 1);
  };

  {
    auto t = MP("MP-movsb-read", "message passing, scalar flag load, data copied by rep movsb");
    t.Reset = Reset64;
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [ScalarWriter64](Asm& A, int Role) {
      if (Role == 0) {
        ScalarWriter64(A);
      } else {
        A.Load(RAX, RBX, D.Y);
        A.Store(R12, 0, RAX);
        RepMovsbFrom(A, RBX, D.X, R12, 32, 32);
      }
    };
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + StateAll({R1[4], R1[5], R1[6], R1[7]}, FULL);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-movsb-read2", "message passing, reader copies flag and data with rep movsb");
    t.Reset = Reset64;
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [ScalarWriter64](Asm& A, int Role) {
      if (Role == 0) {
        ScalarWriter64(A);
      } else {
        RepMovsbFrom(A, RBX, D.Y, R12, 0, 8);
        RepMovsbFrom(A, RBX, D.X, R12, 32, 32);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + StateAll({R1[4], R1[5], R1[6], R1[7]}, FULL);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-flag-stosq", "message passing, scalar data, flag written by rep stosq");
    t.Reset = {D.X, D.Y};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.StoreImm(RBX, D.X, 1);
        RepStosqTo(A, D.Y, 1, 1);
      } else {
        A.Load(RAX, RBX, D.Y);
        A.Load(RDX, RBX, D.X);
        A.Store(R12, 0, RAX);
        A.Store(R12, 8, RDX);
      }
    };
    t.Scalar = Expect::Either; // allowed to break architecturally, not seen on an M4
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + State(R1[1], 1);
    };
    T.push_back(std::move(t));
  }

  {
    auto t = MP("MP-flag-movsb", "message passing, scalar data, reader copies the flag with rep movsb");
    t.Reset = {D.X, D.Y};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.StoreImm(RBX, D.X, 1);
        A.StoreImm(RBX, D.Y, 1);
      } else {
        RepMovsbFrom(A, RBX, D.Y, R12, 0, 8);
        A.Load(RDX, RBX, D.X);
        A.Store(R12, 8, RDX);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], 1) * 3 + State(R1[1], 1);
    };
    T.push_back(std::move(t));
  }

  // Misaligned message passing. Both data and flag are full qwords written and read at
  // misaligned addresses. With emulation on, these accesses take an alignment fault.
  auto Unaligned = [](const char* Name, const char* Desc, Layout L, int32_t DataOff, int32_t FlagOff) {
    auto t = MP(Name, Desc, L);
    t.Reset = {DataOff & ~7, (DataOff & ~7) + 8, FlagOff & ~7, (FlagOff & ~7) + 8};
    // Warm both halves of the data, which may be in different lines or pages.
    t.Pre = [DataOff](Asm& A, int Role) {
      WarmData(A, Role, DataOff & ~7);
      WarmData(A, Role, (DataOff & ~7) + 8);
    };
    t.Body = [DataOff, FlagOff](Asm& A, int Role) {
      if (Role == 0) {
        A.MovImm(RAX, FULL);
        A.Store(RBX, DataOff, RAX);
        A.Store(RBX, FlagOff, RAX);
      } else {
        A.Load(RAX, RBX, FlagOff);
        A.Load(RDX, RBX, DataOff);
        A.Store(R12, 0, RAX);
        A.Store(R12, 8, RDX);
      }
    };
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return State(R1[0], FULL) * 3 + State(R1[1], FULL);
    };
    return t;
  };
  T.push_back(Unaligned("MP-split16", "message passing, data and flag cross a 16-byte boundary", D, D.X + 12, D.Y + 28));
  T.push_back(Unaligned("MP-splitline", "message passing, data and flag cross a 128-byte cache line", D, D.X + 124, D.Y + 124));
  {
    // Slots of 32K: the data straddles a 16K host page boundary, the flag a 4K guest one.
    const Layout P {32768, 16, 16376, 20472};
    T.push_back(Unaligned("MP-split16K", "message passing, data crosses a 16K host page, flag a 4K guest page", P, P.X + 4, P.Y + 4));
    // Rare on an M4: 24 forbidden outcomes in 100 million tries (--soak), none in the
    // default run, so not required.
    T.back().Off = Expect::Either;
    T.push_back(Unaligned("MP-split4K", "message passing, data crosses a 4K guest page, flag a 16K host page", P, P.Y + 4, P.X + 4));
  }

  // Locked operations on misaligned addresses shared by every thread of every group. ARM64
  // atomics fault on these, and the fault handler emulates them; the total must be exact.
  // Each thread records whether another thread got in between its operations, which is
  // the evidence that they contended.
  auto Counting = [&T](const char* Name, const char* Desc) -> LitmusTest& {
    LitmusTest t {Name, Desc, D};
    t.Labels = {"no other thread between", "another thread between"};
    t.Interleaved = {1};
    t.Off = t.Scalar = Expect::Hold;
    t.IterationDivisor = 100;
    T.push_back(std::move(t));
    return T.back();
  };
  auto XaddTest = [&](const char* Name, const char* Desc, int32_t Ctr) {
    auto& t = Counting(Name, Desc);
    t.Body = [Ctr](Asm& A, int) {
      for (int i = 0; i < Increments; ++i) {
        A.MovImm(RAX, 1);
        A.LockXadd(R8, Ctr, RAX);
        if (i == 0) {
          A.Store(R12, 0, RAX);
        }
      }
      A.Store(R12, 8, RAX);
    };
    t.Classify = [](const uint64_t* R0, const uint64_t* R1) {
      return int(R0[1] - R0[0] != Increments - 1 || R1[1] - R1[0] != Increments - 1);
    };
    t.FinalCheck = [Ctr](const uint8_t* Shared, uint64_t ThreadIterations) {
      uint64_t V;
      memcpy(&V, Shared + Ctr, 8);
      const uint64_t Want = ThreadIterations * Increments;
      return V == Want ? std::string() : "counter " + std::to_string(V) + ", expected " + std::to_string(Want);
    };
  };
  XaddTest("LOCK-xadd-split16", "lock xadd on a qword crossing a 16-byte boundary, all threads", CtrSplit16);
  XaddTest("LOCK-xadd-splitline", "lock xadd on a qword crossing a 128-byte line, all threads", CtrSplitLine);
  XaddTest("LOCK-xadd-splitpage", "lock xadd on a qword crossing a 16K page, all threads", CtrSplitPage);
  {
    auto& t = Counting("LOCK-cmpxchg-split16", "lock cmpxchg increment loop on a qword crossing 16 bytes");
    t.Body = [](Asm& A, int) {
      A.MovImm(RSI, 0); // retries
      for (int i = 0; i < Increments; ++i) {
        const size_t Retry = A.Here();
        A.Load(RAX, R8, CasSplit16);
        A.Mov(RDX, RAX);
        A.Inc(RDX);
        A.LockCmpxchg(R8, CasSplit16, RDX);
        const size_t Done = A.JccFwd(CC_Z);
        A.Inc(RSI);
        A.JmpBack(Retry);
        A.Bind(Done);
      }
      A.Store(R12, 0, RSI);
    };
    t.Classify = [](const uint64_t* R0, const uint64_t* R1) {
      return int(R0[0] != 0 || R1[0] != 0);
    };
    t.FinalCheck = [](const uint8_t* Shared, uint64_t ThreadIterations) {
      uint64_t V;
      memcpy(&V, Shared + CasSplit16, 8);
      const uint64_t Want = ThreadIterations * Increments;
      return V == Want ? std::string() : "counter " + std::to_string(V) + ", expected " + std::to_string(Want);
    };
  }
  {
    // A spin lock taken with xchg on a misaligned word and released with a plain store,
    // as x86 code does, guarding a plain increment. Mutual exclusion needs the xchg to be
    // atomic; the release needs TSO (the increment's store must not pass it), so with
    // emulation off a short count is allowed.
    auto& t = Counting("LOCK-xchg-split16", "spin lock by xchg on a qword crossing 16 bytes, plain release");
    t.Body = [](Asm& A, int) {
      A.MovImm(RSI, 0); // spins
      const size_t Retry = A.Here();
      A.MovImm(RAX, 1);
      A.Xchg(R8, XchgLock, RAX);
      A.Test(RAX, RAX);
      const size_t Got = A.JccFwd(CC_Z);
      A.Inc(RSI);
      A.JmpBack(Retry);
      A.Bind(Got);
      A.Load(RDX, R8, XchgCounter);
      A.Inc(RDX);
      A.Store(R8, XchgCounter, RDX);
      A.StoreImm(R8, XchgLock, 0);
      A.Store(R12, 0, RSI);
    };
    t.Off = Expect::Either;
    t.Classify = [](const uint64_t* R0, const uint64_t* R1) {
      return int(R0[0] != 0 || R1[0] != 0);
    };
    t.FinalCheck = [](const uint8_t* Shared, uint64_t ThreadIterations) {
      uint64_t V;
      memcpy(&V, Shared + XchgCounter, 8);
      return V == ThreadIterations ? std::string() : "counter " + std::to_string(V) + ", expected " + std::to_string(ThreadIterations);
    };
  }

  return T;
}

// ---------------------------------------------------------------------------------------

int Log2(uint32_t V) {
  int N = 0;
  while ((1U << N) < V) {
    ++N;
  }
  return N;
}

// Builds the whole guest loop for one thread of a test.
std::vector<uint8_t> BuildProgram(const LitmusTest& T, int Role, int NumThreads) {
  Asm A;
  const int StrideShift = Log2(T.L.Stride);
  auto Barrier = [&] {
    A.AluImm(ADD, RBP, NumThreads);
    A.MovImm(RAX, 1);
    A.LockXadd(R10, 0, RAX);
    const size_t Spin = A.Here();
    A.Load(RAX, R10, 0);
    A.Cmp(RAX, RBP);
    A.JccBack(CC_B, Spin);
    // A full barrier in every mode, so that nothing from the body moves above the
    // barrier even with emulation off.
    A.Mfence();
  };

  const size_t Top = A.Here();
  A.Mov(RBX, R15);
  A.AluImm(AND, RBX, T.L.Slots - 1);
  A.Shl(RBX, StrideShift);
  A.Add(RBX, R11);
  A.Mov(R12, R15);
  A.Shl(R12, RESULT_SHIFT);
  A.Add(R12, R13);

  if (Role == 0) {
    // Clear the slot two iterations ahead: the slot the other thread may still be using
    // is the previous one, and the next one was cleared before the last barrier.
    A.Mov(RAX, R15);
    A.AluImm(ADD, RAX, 2);
    A.AluImm(AND, RAX, T.L.Slots - 1);
    A.Shl(RAX, StrideShift);
    A.Add(RAX, R11);
    for (auto Off : T.Reset) {
      A.StoreImm(RAX, Off, 0);
    }
  }
  if (T.Pre) {
    T.Pre(A, Role);
  }

  Barrier();

  // Random delay from this thread's table.
  A.Mov(RAX, R15);
  A.AluImm(AND, RAX, DELAY_ENTRIES - 1);
  A.Add(RAX, R9);
  A.MovzxByte(RCX, RAX, 0);
  A.Test(RCX, RCX);
  const size_t Skip = A.JccFwd(CC_Z);
  const size_t DelayLoop = A.Here();
  A.Dec32(RCX);
  A.JccBack(CC_NZ, DelayLoop);
  A.Bind(Skip);

  T.Body(A, Role);

  if (T.Final) {
    Barrier();
    if (Role == 0) {
      T.Final(A);
    }
  }

  A.Inc(R15);
  A.Cmp(R15, R14);
  A.JccBack(CC_B, Top);
  A.Hlt();
  return A.B;
}

// ---------------------------------------------------------------------------------------

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[%s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
void AssertHandler(const char* Message) {
  fprintf(stderr, "[ASSERT] %s\n", Message);
}

class NullSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override {}
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
};

thread_local jmp_buf LongJump {};

// Counts the alignment faults the core handles, to show that the misaligned tests reach the
// fault-time back-patching (and how often several threads fault on the same code).
std::atomic<uint64_t> AlignmentFaults {};

// Prints where a stalled thread is. Not async-signal-safe; it only runs when the tool is
// about to give up anyway.
void WhereHandler(int, siginfo_t*, void* RawContext) {
  char Line[160];
#if defined(__APPLE__) && defined(__aarch64__)
  auto& SS = static_cast<ucontext_t*>(RawContext)->uc_mcontext->__ss;
  snprintf(Line, sizeof(Line), "--- thread %p: pc 0x%llx lr 0x%llx\n", (void*)pthread_self(), (unsigned long long)SS.__pc, (unsigned long long)SS.__lr);
#else
  (void)RawContext;
  snprintf(Line, sizeof(Line), "--- thread %p\n", (void*)pthread_self());
#endif
  write(STDOUT_FILENO, Line, strlen(Line));
  void* Frames[48];
  const int N = backtrace(Frames, 48);
  backtrace_symbols_fd(Frames, N, STDOUT_FILENO);
}

#if defined(__APPLE__) && defined(__aarch64__)
void CountingFaultHandler(int Signal, siginfo_t* Info, void* Context) {
  if (Signal == SIGBUS && Info->si_code == BUS_ADRALN) {
    AlignmentFaults.fetch_add(1, std::memory_order_relaxed);
  }
  FEX::MacOSHostFaults::FaultHandler(Signal, Info, Context);
}
#endif

[[noreturn]]
void SynchronousExceptionHandler(FEXCore::Context::Context*, FEXCore::Core::CpuStateFrame*) {
  longjmp(LongJump, 1);
}

struct GuestThread {
  uint64_t RIP {};
  uint64_t Regs[16] {};
  bool Done {};
  uint64_t FinalIteration {};
  // 0 not started, 1 thread created, 2 running guest code, 3 returned, 4 cleaned up.
  std::atomic<int> Stage {};
  pthread_t Handle {};
};

std::mutex CreateMutex;

// What the stall watchdog reports.
struct {
  std::mutex M;
  std::string Test;
  unsigned Run {};
  uint64_t Serial {};
  std::vector<const uint64_t*> Counters;
  std::vector<const GuestThread*> Threads;
} Progress;

void RunGuest(FEXCore::Context::Context* CTX, GuestThread* Self) {
#ifdef __APPLE__
  // macOS has no way to pin a thread to a core. Ask for the highest quality of service so
  // the scheduler prefers performance cores and does not park the thread.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  Self->Handle = pthread_self();
  FEXCore::Allocator::InitializeThread();

  FEXCore::Core::InternalThreadState* Thread {};
  {
    std::lock_guard Lock {CreateMutex};
    Thread = CTX->CreateThread();
  }
  Self->Stage = 1;
  if (!Thread || !FEX::CoreThreadSetup::AllocateCallRetStack(Thread)) {
    return;
  }
  auto Frame = Thread->CurrentFrame;
  auto* GDT = FEX::CoreThreadSetup::FullDescriptorTable();
  Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = GDT;
  Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = GDT;
  Frame->State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto CodeSegment = FEXCore::Core::CPUState::GetSegmentFromIndex(Frame->State, Frame->State.cs_idx);
  {
    std::lock_guard Lock {CreateMutex};
    FEXCore::Core::CPUState::SetGDTBase(CodeSegment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(CodeSegment, 0xF'FFFFU);
    CodeSegment->L = 1;
    CodeSegment->D = 0;
  }
  Frame->State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CodeSegment);
  Frame->Pointers.SynchronousExceptionHandler = reinterpret_cast<uintptr_t>(&SynchronousExceptionHandler);
  Frame->State.rip = Self->RIP;
  for (int i = 0; i < 16; ++i) {
    Frame->State.gregs[i] = Self->Regs[i];
  }

#if defined(__APPLE__) && defined(__aarch64__)
  FEX::MacOSHostFaults::CurrentThread = Thread;
#endif
  Self->Stage = 2;
  if (!setjmp(LongJump)) {
    CTX->ExecuteThread(Thread);
  }
  Self->Stage = 3;
#if defined(__APPLE__) && defined(__aarch64__)
  FEX::MacOSHostFaults::CurrentThread = nullptr;
#endif

  Self->FinalIteration = Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_R15];
  Self->Done = true;

  void* CallRet = Thread->CallRetStackBase;
  CTX->DestroyThread(Thread);
  if (CallRet) {
    constexpr size_t Guard = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
    ::munmap(static_cast<uint8_t*>(CallRet) - Guard, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * Guard);
  }
  Self->Stage = 4;
}

uint32_t RandomBelow(uint32_t N) {
#ifdef __APPLE__
  return arc4random_uniform(N);
#else
  return static_cast<uint32_t>(random()) % N;
#endif
}

void* MapZeroed(size_t Size) {
  void* P = ::mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return P == MAP_FAILED ? nullptr : P;
}

size_t AlignUp(size_t V, size_t A) {
  return (V + A - 1) & ~(A - 1);
}

struct Options {
  enum class TSO { On, Off, Scalar } Mode = TSO::On;
  enum class Expect { TSO, Weak, Scalar, Report } Want = Expect::TSO;
  bool ExpectSet = false;
  uint64_t Iterations = 0;
  unsigned Runs = 0;
  unsigned Groups = 0;
  bool Soak = false;
  bool List = false;
  std::vector<std::string> Only;
};

[[noreturn]]
void Usage() {
  fprintf(stderr, "usage: FEXCoreOrderingTest [--tso on|off|scalar] [--expect tso|weak|scalar|report] [--iterations N] [--runs R]\n"
                  "                           [--groups G] [--soak] [--only name[,name...]] [--list]\n");
  _exit(2);
}

Options Parse(int argc, char** argv) {
  Options O;
  for (int i = 1; i < argc; ++i) {
    const std::string A = argv[i];
    auto Next = [&]() -> std::string {
      if (i + 1 >= argc) {
        Usage();
      }
      return argv[++i];
    };
    if (A == "--tso") {
      const auto V = Next();
      if (V == "on") {
        O.Mode = Options::TSO::On;
      } else if (V == "off") {
        O.Mode = Options::TSO::Off;
      } else if (V == "scalar") {
        O.Mode = Options::TSO::Scalar;
      } else {
        Usage();
      }
    } else if (A == "--expect") {
      const auto V = Next();
      if (V == "tso") {
        O.Want = Options::Expect::TSO;
      } else if (V == "weak") {
        O.Want = Options::Expect::Weak;
      } else if (V == "scalar") {
        O.Want = Options::Expect::Scalar;
      } else if (V == "report") {
        O.Want = Options::Expect::Report;
      } else {
        Usage();
      }
      O.ExpectSet = true;
    } else if (A == "--iterations") {
      O.Iterations = strtoull(Next().c_str(), nullptr, 0);
    } else if (A == "--runs") {
      O.Runs = static_cast<unsigned>(strtoul(Next().c_str(), nullptr, 0));
    } else if (A == "--groups") {
      O.Groups = static_cast<unsigned>(strtoul(Next().c_str(), nullptr, 0));
    } else if (A == "--soak") {
      O.Soak = true;
    } else if (A == "--list") {
      O.List = true;
    } else if (A == "--only") {
      std::string V = Next();
      size_t Pos = 0;
      while (Pos <= V.size()) {
        const size_t Comma = V.find(',', Pos);
        O.Only.push_back(V.substr(Pos, Comma == std::string::npos ? std::string::npos : Comma - Pos));
        if (Comma == std::string::npos) {
          break;
        }
        Pos = Comma + 1;
      }
    } else {
      Usage();
    }
  }
  if (!O.ExpectSet) {
    O.Want = O.Mode == Options::TSO::On ? Options::Expect::TSO : (O.Mode == Options::TSO::Off ? Options::Expect::Weak : Options::Expect::Scalar);
  }
  return O;
}

unsigned HostCPUs() {
  const auto N = std::thread::hardware_concurrency();
  return N ? N : 2;
}

} // namespace

int main(int argc, char** argv) {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
  setvbuf(stdout, nullptr, _IOLBF, 0);

  const Options Opt = Parse(argc, argv);
  auto Tests = MakeTests();
  if (Opt.List) {
    for (auto& T : Tests) {
      printf("%-20s %s\n", T.Name, T.Description);
    }
    return 0;
  }

  const unsigned Groups = Opt.Groups ? Opt.Groups : std::max(1U, std::min(4U, HostCPUs() / 2));
  const uint64_t AllIterations = Opt.Iterations ? Opt.Iterations : (Opt.Soak ? 250'000 : 100'000);
  const unsigned Runs = Opt.Runs ? Opt.Runs : (Opt.Soak ? 100 : 8);
  // Every test has two threads; the result arrays and the stall report assume it.
  constexpr int NumThreads = 2;

  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
  const bool ScalarTSO = Opt.Mode != Options::TSO::Off;
  const bool OtherTSO = Opt.Mode == Options::TSO::On;
  FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, ScalarTSO ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_VECTORTSOENABLED, OtherTSO ? "1" : "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MEMCPYSETTSOENABLED, OtherTSO ? "1" : "0");

  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  {
    auto HostFeatures = FEX::FetchHostFeatures(true);
    CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  }
  auto SignalDelegation = FEX::DummyHandlers::CreateSignalDelegator();
  auto SyscallHandler = fextl::make_unique<NullSyscallHandler>();
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());
  if (!CTX->InitCore()) {
    fprintf(stderr, "FAIL: InitCore returned false\n");
    return 1;
  }
#if defined(__APPLE__) && defined(__aarch64__)
  FEX::MacOSHostFaults::Install();
  {
    struct sigaction Action {};
    Action.sa_sigaction = CountingFaultHandler;
    Action.sa_flags = SA_SIGINFO;
    sigemptyset(&Action.sa_mask);
    sigaction(SIGBUS, &Action, nullptr);
  }
#endif

  const char* ModeName = Opt.Mode == Options::TSO::On ? "on" : (Opt.Mode == Options::TSO::Off ? "off" : "scalar only");
  printf("memory-ordering emulation: %s; expecting %s; %u groups x %u runs x %llu iterations per test\n", ModeName,
         Opt.Want == Options::Expect::TSO  ? "TSO (no forbidden outcome)" :
         Opt.Want == Options::Expect::Weak ? "weak behaviour (forbidden outcomes seen)" :
         Opt.Want == Options::Expect::Scalar ? "the vector and string tests to break, the rest to hold" :
                                             "nothing (report only)", Groups, Runs,
         static_cast<unsigned long long>(AllIterations));

  {
    struct sigaction Action {};
    Action.sa_sigaction = WhereHandler;
    Action.sa_flags = SA_SIGINFO;
    sigemptyset(&Action.sa_mask);
    sigaction(SIGUSR2, &Action, nullptr);
  }
  // A guest thread that stops (a hang in the core, or a fault in a fault handler, which
  // hangs on macOS) leaves its partner spinning at the barrier. Report where everything
  // stood once nothing has moved for a while.
  std::thread([StallLimit = Opt.Soak ? 120U : 30U] {
    uint64_t Last = ~0ULL;
    unsigned Stalled = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::lock_guard Lock {Progress.M};
      uint64_t Signature = Progress.Serial * 1'000'003ULL;
      for (auto* C : Progress.Counters) {
        Signature += __atomic_load_n(C, __ATOMIC_RELAXED);
      }
      for (auto* T : Progress.Threads) {
        Signature += T->Stage.load() * 7;
      }
      Stalled = Signature == Last ? Stalled + 1 : 0;
      Last = Signature;
      if (Stalled < StallLimit) {
        continue;
      }
      printf("FAIL: no progress for %u seconds in %s, run %u\n", StallLimit, Progress.Test.c_str(), Progress.Run);
      for (size_t G = 0; G < Progress.Counters.size(); ++G) {
        printf("  group %zu: barrier count %llu, threads at stage", G,
               static_cast<unsigned long long>(__atomic_load_n(Progress.Counters[G], __ATOMIC_RELAXED)));
        for (size_t R = 0; R < 2 && G * 2 + R < Progress.Threads.size(); ++R) {
          printf(" %d", Progress.Threads[G * 2 + R]->Stage.load());
        }
        printf("\n");
      }
      printf("  (stage 1 created, 2 running guest code, 3 returned, 4 cleaned up)\n");
      fflush(stdout);
      // Ask each thread that is still running guest code where it is.
      for (auto* T : Progress.Threads) {
        if (T->Stage.load() == 2) {
          pthread_kill(T->Handle, SIGUSR2);
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
      }
      _exit(3);
    }
  }).detach();

  for (const auto& Name : Opt.Only) {
    if (std::none_of(Tests.begin(), Tests.end(), [&Name](const LitmusTest& T) { return Name == T.Name; })) {
      fprintf(stderr, "FAIL: no test named '%s' (see --list)\n", Name.c_str());
      return 2;
    }
  }

  const auto Start = std::chrono::steady_clock::now();
  int Failures = 0;

  for (auto& T : Tests) {
    if (!Opt.Only.empty() && std::find(Opt.Only.begin(), Opt.Only.end(), T.Name) == Opt.Only.end()) {
      continue;
    }
    const uint64_t Iterations = std::max<uint64_t>(16, AllIterations / T.IterationDivisor);
    const auto TestStart = std::chrono::steady_clock::now();
    const uint64_t FaultsBefore = AlignmentFaults.load();
    std::vector<uint64_t> Counts(T.Labels.size(), 0);
    bool Broken = false;
    uint64_t FinalFailures = 0;
    std::string FinalMessage;

    for (unsigned Run = 0; Run < Runs && !Broken; ++Run) {
      // A fresh copy of the code for every run, so it is translated again and the
      // threads meet its first execution together. Code regions are never unmapped: the
      // core keeps translations by guest address.
      constexpr size_t RoleCodeSize = 0x4000;
      auto* Code = static_cast<uint8_t*>(MapZeroed(RoleCodeSize * NumThreads));
      if (!Code) {
        fprintf(stderr, "FAIL: could not map guest code\n");
        return 1;
      }
      for (int Role = 0; Role < NumThreads; ++Role) {
        auto Program = BuildProgram(T, Role, NumThreads);
        if (Program.size() > RoleCodeSize) {
          fprintf(stderr, "FAIL: program too large\n");
          return 1;
        }
        memcpy(Code + Role * RoleCodeSize, Program.data(), Program.size());
      }

      // Small random delays; the range differs between runs, from none to a few hundred
      // nanoseconds, because which offset exposes reordering varies by test and core.
      static constexpr uint32_t MaxDelays[] = {0, 3, 12, 40};
      const uint32_t MaxDelay = MaxDelays[Run % 4];

      struct GroupMemory {
        uint8_t* Base;
        size_t Size;
        uint8_t* Slots;
        uint64_t* Results[NumThreads];
        uint64_t* Counter;
        uint8_t* Delays[NumThreads];
      };
      // The run's shared area: constants the guest code reads, and the counters of the
      // counting tests, which every thread of every group updates.
      auto* Shared = static_cast<uint8_t*>(MapZeroed(SHARED_SIZE));
      if (!Shared) {
        fprintf(stderr, "FAIL: could not map guest data\n");
        return 1;
      }
      for (int i = 0; i < 2; ++i) {
        memcpy(Shared + ConstFull + i * 8, &FULL, 8);
      }
      memset(Shared + ConstCopy, COPY_BYTE, 64);
      const uint64_t One = 1;
      memcpy(Shared + ConstOne, &One, 8);

      std::vector<GroupMemory> Mem(Groups);
      std::vector<GuestThread> Threads(Groups * NumThreads);
      const size_t Page = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
      const size_t SlotBytes = AlignUp(size_t(T.L.Stride) * T.L.Slots, Page);
      const size_t ResultBytes = AlignUp(Iterations << RESULT_SHIFT, Page);
      for (unsigned G = 0; G < Groups; ++G) {
        auto& M = Mem[G];
        M.Size = SlotBytes + NumThreads * ResultBytes + Page + NumThreads * Page;
        M.Base = static_cast<uint8_t*>(MapZeroed(M.Size));
        if (!M.Base) {
          fprintf(stderr, "FAIL: could not map guest data\n");
          return 1;
        }
        M.Slots = M.Base;
        uint8_t* P = M.Base + SlotBytes;
        for (int R = 0; R < NumThreads; ++R) {
          M.Results[R] = reinterpret_cast<uint64_t*>(P);
          P += ResultBytes;
        }
        M.Counter = reinterpret_cast<uint64_t*>(P);
        P += Page;
        for (int R = 0; R < NumThreads; ++R) {
          M.Delays[R] = P;
          for (uint32_t i = 0; i < DELAY_ENTRIES; ++i) {
            // About a quarter start at once.
            P[i] = (MaxDelay == 0 || RandomBelow(4) == 0) ? 0 : static_cast<uint8_t>(RandomBelow(MaxDelay + 1));
          }
          P += Page;
        }
        for (int R = 0; R < NumThreads; ++R) {
          auto& GT = Threads[G * NumThreads + R];
          GT.RIP = reinterpret_cast<uint64_t>(Code + R * RoleCodeSize);
          GT.Regs[FEXCore::X86State::REG_R14] = Iterations;
          GT.Regs[FEXCore::X86State::REG_R13] = reinterpret_cast<uint64_t>(M.Results[R]);
          GT.Regs[FEXCore::X86State::REG_R11] = reinterpret_cast<uint64_t>(M.Slots);
          GT.Regs[FEXCore::X86State::REG_R10] = reinterpret_cast<uint64_t>(M.Counter);
          GT.Regs[FEXCore::X86State::REG_R9] = reinterpret_cast<uint64_t>(M.Delays[R]);
          GT.Regs[FEXCore::X86State::REG_R8] = reinterpret_cast<uint64_t>(Shared);
        }
      }

      {
        std::lock_guard Lock {Progress.M};
        Progress.Test = T.Name;
        Progress.Run = Run;
        ++Progress.Serial;
        Progress.Counters.clear();
        Progress.Threads.clear();
        for (auto& M : Mem) {
          Progress.Counters.push_back(M.Counter);
        }
        for (auto& GT : Threads) {
          Progress.Threads.push_back(&GT);
        }
      }
      std::vector<std::thread> Host;
      for (auto& GT : Threads) {
        Host.emplace_back(RunGuest, CTX.get(), &GT);
      }
      for (auto& H : Host) {
        H.join();
      }

      for (unsigned G = 0; G < Groups; ++G) {
        for (int R = 0; R < NumThreads; ++R) {
          const auto& GT = Threads[G * NumThreads + R];
          if (!GT.Done || GT.FinalIteration != Iterations) {
            printf("  %s: group %u thread %d did not finish (iteration %llu of %llu)\n", T.Name, G, R,
                   static_cast<unsigned long long>(GT.FinalIteration), static_cast<unsigned long long>(Iterations));
            Broken = true;
          }
        }
        if (Broken) {
          continue;
        }
        for (uint64_t i = 0; i < Iterations; ++i) {
          const uint64_t* R0 = Mem[G].Results[0] + (i << (RESULT_SHIFT - 3));
          const uint64_t* R1 = Mem[G].Results[1] + (i << (RESULT_SHIFT - 3));
          const int Index = T.Classify(R0, R1);
          if (Index < 0 || size_t(Index) >= Counts.size()) {
            Broken = true;
            break;
          }
          ++Counts[Index];
        }
      }
      {
        std::lock_guard Lock {Progress.M};
        ++Progress.Serial;
        Progress.Counters.clear();
        Progress.Threads.clear();
      }
      if (!Broken && T.FinalCheck) {
        auto Message = T.FinalCheck(Shared, uint64_t(Groups) * NumThreads * Iterations);
        if (!Message.empty()) {
          ++FinalFailures;
          FinalMessage = Message;
        }
      }
      for (auto& M : Mem) {
        ::munmap(M.Base, M.Size);
      }
      ::munmap(Shared, SHARED_SIZE);
    }

    uint64_t Forbidden = FinalFailures;
    for (int F : T.Forbidden) {
      Forbidden += Counts[F];
    }
    uint64_t Overlapping = 0;
    for (int I : T.Interleaved) {
      Overlapping += Counts[I];
    }
    const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - TestStart).count();
    const Expect Required = Opt.Want == Options::Expect::TSO    ? Expect::Hold :
                            Opt.Want == Options::Expect::Weak   ? T.Off :
                            Opt.Want == Options::Expect::Scalar ? T.Scalar :
                                                                  Expect::Either;
    std::string Verdict;
    bool Failed = true;
    if (Broken) {
      Verdict = "BROKEN (a guest thread did not finish or an outcome was out of range)";
    } else if (Overlapping == 0) {
      // Without overlap a test cannot show reordering, so passing would mean nothing.
      Verdict = "NOT RACING (no outcome that needs the threads to overlap)";
    } else if (T.Relaxed >= 0 && Counts[T.Relaxed] == 0) {
      Verdict = "NOT RACING (relaxed outcome never seen)";
    } else if (Required == Expect::Hold) {
      Failed = Forbidden != 0;
      Verdict = Forbidden ? "FAIL (TSO-forbidden outcome seen)" : "ok (no forbidden outcome)";
    } else if (Required == Expect::Break) {
      Failed = Forbidden == 0;
      Verdict = Forbidden ? "ok (forbidden outcome seen, as required in this mode)" : "FAIL (forbidden outcome not seen)";
    } else {
      Failed = false;
      Verdict = Forbidden ? "forbidden outcome seen (not required)" : "forbidden outcome not seen (not required)";
    }
    if (!FinalMessage.empty()) {
      Verdict += "; wrong total in " + std::to_string(FinalFailures) + " run(s), last: " + FinalMessage;
    }
    Failures += Failed ? 1 : 0;

    printf("%-20s %s\n", T.Name, T.Description);
    for (size_t i = 0; i < Counts.size(); ++i) {
      const bool IsForbidden = std::find(T.Forbidden.begin(), T.Forbidden.end(), int(i)) != T.Forbidden.end();
      const bool IsInterleaved = std::find(T.Interleaved.begin(), T.Interleaved.end(), int(i)) != T.Interleaved.end();
      if (Counts[i] || IsForbidden) {
        printf("    %-24s %12llu%s\n", T.Labels[i].c_str(), static_cast<unsigned long long>(Counts[i]),
               IsForbidden            ? "  <- forbidden by TSO" :
               int(i) == T.Relaxed    ? "  <- relaxed, allowed by TSO" :
               IsInterleaved          ? "  (threads overlapped)" :
                                        "");
      }
    }
    if (const uint64_t Faults = AlignmentFaults.load() - FaultsBefore) {
      printf("    misaligned-access faults handled: %llu over %u runs\n", static_cast<unsigned long long>(Faults), Runs);
    }
    printf("    %s, %.1fs\n", Verdict.c_str(), Seconds);
  }

  const double Total = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
  printf("%s: emulation %s, %d test(s) failed, %.1fs\n", Failures ? "FAIL" : "PASS", ModeName, Failures, Total);
  fflush(stdout);
  _exit(Failures ? 1 : 0);
}
