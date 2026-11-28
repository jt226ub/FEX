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
// The tool uses only the fexmac interface (fexmac.h): contexts, threads, guest state, runs
// that stop at `hlt`, and fexmac_handle_fault from its own fault handler.
//
// Each test is a classic litmus test: two guest threads run a few memory accesses on
// shared variables, many times over, and the tool counts the outcomes. Some outcomes are
// forbidden under TSO but allowed (and produced) by Apple silicon when the accesses are
// translated to plain loads and stores. With emulation on, a forbidden outcome must never
// appear. With emulation off, the same harness must see it, which shows that the test can
// tell the two apart. Every test must also show at least one outcome that is possible only
// when its two threads overlap; otherwise it fails as NOT RACING, in every mode, since a
// test whose threads never overlap passes for nothing. Store buffering (SB) is allowed
// under TSO and its relaxed outcome must appear as well (except with emulation on and a
// single group, where two threads may not show it at all).
//
// Further tests: store buffering with `mfence` or a locked instruction between store and
// load (forbidden in every mode), and counting tests in which every thread of every group
// runs one of the locked operations the core emulates when the operand crosses 16 bytes
// (xadd of 16, 32 and 64 bits, also across a 128-byte line and the 16K page; add, sub,
// xor, inc, dec, not, neg, or/and, cmpxchg, cmpxchg8b, xchg locks) on one shared operand,
// with plain stores to its neighbouring bytes, and the result must be exact.
//
// Before running, every guest thread of a run waits at a host-side start barrier, so that
// the threads that run the same code reach its first execution together. A test whose
// threads were never seen to overlap gets up to two more sets of runs before it fails as
// NOT RACING (all outcomes count). With fewer than four groups each run has
// proportionally more iterations.
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
//                       [--only name[,name...]] [--list] [--qos interactive|utility|background]
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

#include <fexmac.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <vector>

#include <pthread/qos.h>
#include <sys/ucontext.h>

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
  // movdqa (aligned; faults on a misaligned address).
  void MovdqaLoad(int X, int Base, int32_t Disp) {
    U8(0x66);
    Rex(false, X, Base);
    U8(0x0F);
    U8(0x6F);
    Mem(X, Base, Disp);
  }
  void MovdqaStore(int Base, int32_t Disp, int X) {
    U8(0x66);
    Rex(false, X, Base);
    U8(0x0F);
    U8(0x7F);
    Mem(X, Base, Disp);
  }
  // vmovdqu ymm (VEX.256.F3.0F 6F/7F), three-byte VEX so that any base register works.
  void Vex256F3(int X, int Base, uint8_t Opcode, int32_t Disp) {
    U8(0xC4);
    U8(((~X >> 3) & 1) << 7 | 1 << 6 | ((~Base >> 3) & 1) << 5 | 0x01);
    U8(0xF << 3 | 1 << 2 | 0x2);
    U8(Opcode);
    Mem(X, Base, Disp);
  }
  void VmovdquLoad(int Y, int Base, int32_t Disp) {
    Vex256F3(Y, Base, 0x6F, Disp);
  }
  void VmovdquStore(int Base, int32_t Disp, int Y) {
    Vex256F3(Y, Base, 0x7F, Disp);
  }
  void RepMovsb() {
    U8(0xF3);
    U8(0xA4);
  }
  void RepMovsq() {
    U8(0xF3);
    U8(0x48);
    U8(0xA5);
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
  // Any instruction with a [base + disp32] operand: legacy prefixes, REX (W and the bits
  // for RegField and Base), opcode bytes, ModRM.
  void Op(std::initializer_list<uint8_t> Prefixes, bool W, std::initializer_list<uint8_t> Opcode, int RegField, int Base, int32_t Disp) {
    for (auto P : Prefixes) {
      U8(P);
    }
    Rex(W, RegField, Base);
    for (auto O : Opcode) {
      U8(O);
    }
    Mem(RegField, Base, Disp);
  }
  void Store32(int Base, int32_t Disp, int Src) {
    Op({}, false, {0x89}, Src, Base, Disp);
  }
  void Store16(int Base, int32_t Disp, int Src) {
    Op({0x66}, false, {0x89}, Src, Base, Disp);
  }
  void Store8(int Base, int32_t Disp, int Src) {
    // A REX prefix (always present for these registers here) selects the low byte.
    const uint8_t Value = 0x40 | (((Src >> 3) & 1) << 2) | ((Base >> 3) & 1);
    U8(Value);
    U8(0x88);
    Mem(Src, Base, Disp);
  }
  void Shr(int R, uint8_t N) {
    Rex(true, 0, R);
    U8(0xC1);
    RR(5, R);
    U8(N);
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

// The shared area: constants first, then the counting tests' operands at misaligned
// addresses (0x1000 on, and across the page boundary at 0x4000), then their ticket.
constexpr size_t SHARED_SIZE = 0x10000; // four 16K pages
constexpr int ConstFull = 0;           // 16 bytes of FULL
constexpr int ConstCopy = 64;          // 64 bytes of COPY_BYTE
constexpr int ConstOne = 128;          // the qword 1
constexpr int ConstPattern = 256;      // TEAR tests: pattern k (1..4) is 32 bytes of 0x11 * k at 256 + 32 * (k - 1)
constexpr int ConstLong = 0x8000;      // 32K of COPY_BYTE, the source of the long rep movsq tests
constexpr uint64_t PATTERN = 0x1111'1111'1111'1111ULL;
constexpr int Increments = 4; // locked operations per thread per iteration
// Added by each locked increment. It changes both halves of a word that crosses 16 bytes
// at its middle, so that two emulations' compare-and-swaps can interleave: without the
// core's split-lock serialisation the totals come out short.
constexpr uint64_t SplitIncrement = 0x1'0000'0001ULL;

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
  // With emulation on (--expect tso): Hold for every test whose forbidden outcome x86
  // forbids; Either for the tests that only report something x86 allows (torn unaligned or
  // 32-byte accesses, stores inside one rep seen out of order).
  Expect On = Expect::Hold;
  std::function<int(const uint64_t* R0, const uint64_t* R1)> Classify;
  // Counting tests: checks the shared area after a run in which every thread did
  // `ThreadIterations` iterations in all; returns an empty string if it is right. A wrong
  // total counts as one forbidden outcome.
  std::function<std::string(const uint8_t* Shared, uint64_t ThreadIterations)> FinalCheck;
  // Runs this many times fewer iterations: every locked operation of the counting tests
  // takes a fault, a few microseconds each.
  uint32_t IterationDivisor = 1;
  // Counting tests: writes initial values into the shared area before each run, and sets
  // per-thread registers (thread index across all groups).
  std::function<void(uint8_t* Shared)> InitShared;
  std::function<void(int ThreadIndex, uint64_t* Regs)> InitRegs;
  // Misaligned tests: the instructions that take an alignment fault and are patched, per
  // run, with emulation on. With several groups the run must see more faults than this
  // (several threads faulting on one instruction at once), or the test fails.
  int PatchedPerRun = 0;
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
// Forbidden under TSO: any part of the flag is seen but not all of the data written before
// it. The flag is written after every data store, so once any byte of it is visible, all
// of the data is.
const std::vector<int> MPForbidden = {1 * 3 + 0, 1 * 3 + 1, 2 * 3 + 0, 2 * 3 + 1};
// Possible only while the writer is between its stores: no flag but some data, or part of
// the flag with all of the data. (No flag and no data means the reader went first, all of
// both that it went second.)
const std::vector<int> MPInterleaved = {0 * 3 + 1, 0 * 3 + 2, 1 * 3 + 2};

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

  // Tearing of 16- and 32-byte accesses (round 2, E3; the acceptance tests for splitting a
  // vector access into 8-byte acquire loads or release stores). The writer stores patterns
  // 1 to 4 in turn to one location (pattern k: every byte 0x11 * k); the reader reads it
  // four times. x86 (Intel SDM vol. 3A, "Guaranteed Atomic Operations": processors with
  // AVX carry out the 16-byte accesses of MOVDQA and friends atomically when 16-byte
  // aligned) forbids a 16-byte aligned movdqa load that returns parts of two different
  // stores. It does not promise that for an unaligned movdqu, nor for any 32-byte access:
  // those tests only report (On = Either). The reader's first loads touch the line, so
  // later loads may see the writer's stores in progress.
  {
    auto Pattern = [](int K) {
      return ConstPattern + 32 * (K - 1);
    };
    // Index of a qword among the patterns: 0 for zero, k for pattern k, -1 otherwise.
    auto Which = [](uint64_t V) {
      if (V == 0) {
        return 0;
      }
      for (int K = 1; K <= 4; ++K) {
        if (V == PATTERN * K) {
          return K;
        }
      }
      return -1;
    };
    const std::vector<std::string> TearLabels = {"one value seen", "values changed, none torn", "torn", "unexpected value"};
    // Groups of qwords that were read by one access: torn if they do not agree.
    auto TearClassify = [Which](const uint64_t* R, int Reads, int Qwords) {
      bool Changed = false, Torn = false;
      int First = Which(R[0]);
      for (int i = 0; i < Reads; ++i) {
        const int Lead = Which(R[i * Qwords]);
        for (int q = 0; q < Qwords; ++q) {
          const int W = Which(R[i * Qwords + q]);
          if (W < 0) {
            return 3;
          }
          Torn |= W != Lead;
        }
        Changed |= Lead != First;
      }
      return Torn ? 2 : (Changed ? 1 : 0);
    };
    auto Tear = [&](const char* Name, const char* Desc, int32_t Off, int Bytes, bool Aligned) {
      LitmusTest t {Name, Desc, D};
      t.Labels = TearLabels;
      t.Forbidden = {2, 3};
      t.Interleaved = {1};
      t.Reset = {D.X, D.X + 8, D.X + 16, D.X + 24, D.X + 32, D.X + 40};
      t.Pre = [](Asm& A, int Role) {
        WarmData(A, Role, D.X);
      };
      t.Body = [Off, Bytes, Aligned, Pattern](Asm& A, int Role) {
        if (Role == 0) {
          for (int K = 1; K <= 4; ++K) {
            if (Bytes == 32) {
              A.VmovdquLoad(0, R8, Pattern(K));
              A.VmovdquStore(RBX, Off, 0);
            } else {
              A.MovdquLoad(0, R8, Pattern(K));
              Aligned ? A.MovdqaStore(RBX, Off, 0) : A.MovdquStore(RBX, Off, 0);
            }
          }
        } else {
          for (int i = 0; i < (Bytes == 32 ? 2 : 4); ++i) {
            if (Bytes == 32) {
              A.VmovdquLoad(1, RBX, Off);
              A.VmovdquStore(R12, i * 32, 1);
            } else {
              Aligned ? A.MovdqaLoad(1, RBX, Off) : A.MovdquLoad(1, RBX, Off);
              A.MovdquStore(R12, i * 16, 1);
            }
          }
        }
      };
      t.Classify = [TearClassify, Bytes](const uint64_t*, const uint64_t* R1) {
        return TearClassify(R1, Bytes == 32 ? 2 : 4, Bytes / 8);
      };
      if (!Aligned || Bytes == 32) {
        t.On = t.Off = t.Scalar = Expect::Either;
      } else {
        // Measured on the M4 before and after: see the round 2 report.
        t.Off = t.Scalar = Expect::Either;
      }
      return t;
    };
    T.push_back(Tear("TEAR-sse", "16-byte aligned movdqa stores read by movdqa loads: no torn read", D.X, 16, true));
    T.push_back(Tear("TEAR-sse-unaligned", "movdqu at +8 (crosses 16 bytes): torn reads only reported", D.X + 8, 16, false));
    T.push_back(Tear("TEAR-avx", "32-byte vmovdqu stores and loads: torn reads only reported", D.X, 32, true));

    // The store side alone: aligned movdqa stores, read by two ordered qword loads, low then
    // high and high then low. An atomic 16-byte store cannot be seen half done by ordered
    // loads: once one half of store k is seen, a later load of the other half sees store k
    // or a later one. (Splitting the store into two release stores, low first, allows "low
    // newer than high" to be seen by a high-then-low reader.)
    {
      LitmusTest t {"TEAR-sse-st", "movdqa stores read by ordered qword loads: never one half older than the half read before it", D};
      t.Labels = TearLabels;
      t.Labels[2] = "half older than the half read first";
      t.Forbidden = {2, 3};
      t.Interleaved = {1};
      t.Reset = {D.X, D.X + 8};
      t.Pre = [](Asm& A, int Role) {
        WarmData(A, Role, D.X);
      };
      t.Body = [Pattern](Asm& A, int Role) {
        if (Role == 0) {
          for (int K = 1; K <= 4; ++K) {
            A.MovdquLoad(0, R8, Pattern(K));
            A.MovdqaStore(RBX, D.X, 0);
          }
        } else {
          // (low, high) twice, then (high, low) twice; results in read order.
          for (int i = 0; i < 4; ++i) {
            const bool HighFirst = i >= 2;
            A.Load(RAX, RBX, D.X + (HighFirst ? 8 : 0));
            A.Load(RDX, RBX, D.X + (HighFirst ? 0 : 8));
            A.Store(R12, i * 16, RAX);
            A.Store(R12, i * 16 + 8, RDX);
          }
        }
      };
      t.Classify = [Which](const uint64_t*, const uint64_t* R1) {
        bool Changed = false, Older = false;
        for (int i = 0; i < 8; ++i) {
          const int W = Which(R1[i]);
          if (W < 0) {
            return 3;
          }
          Changed |= W != Which(R1[0]);
        }
        for (int i = 0; i < 4; ++i) {
          Older |= Which(R1[i * 2 + 1]) < Which(R1[i * 2]);
        }
        return Older ? 2 : (Changed ? 1 : 0);
      };
      t.Off = t.Scalar = Expect::Either;
      T.push_back(std::move(t));
    }

    // The load side alone: the writer stores the low then the high qword of each pattern
    // (ordered scalar stores); the reader's aligned movdqa loads must never return a high
    // half newer than the low half (the high half of k is stored after the low half of
    // k). Splitting the load into two acquire loads, low first, allows it.
    {
      LitmusTest t {"TEAR-sse-ld", "qword stores low then high, read by movdqa: never the high half newer than the low", D};
      t.Labels = TearLabels;
      t.Labels[2] = "high half newer than low";
      t.Forbidden = {2, 3};
      t.Interleaved = {1};
      t.Reset = {D.X, D.X + 8};
      t.Pre = [](Asm& A, int Role) {
        WarmData(A, Role, D.X);
      };
      t.Body = [](Asm& A, int Role) {
        if (Role == 0) {
          for (int K = 1; K <= 4; ++K) {
            A.MovImm(RAX, PATTERN * K);
            A.Store(RBX, D.X, RAX);
            A.Store(RBX, D.X + 8, RAX);
          }
        } else {
          for (int i = 0; i < 4; ++i) {
            A.MovdqaLoad(1, RBX, D.X);
            A.MovdquStore(R12, i * 16, 1);
          }
        }
      };
      t.Classify = [Which](const uint64_t*, const uint64_t* R1) {
        bool Changed = false, Newer = false;
        for (int i = 0; i < 8; ++i) {
          const int W = Which(R1[i]);
          if (W < 0) {
            return 3;
          }
          Changed |= W != Which(R1[0]);
        }
        for (int i = 0; i < 4; ++i) {
          Newer |= Which(R1[i * 2 + 1]) > Which(R1[i * 2]);
        }
        return Newer ? 2 : (Changed ? 1 : 0);
      };
      t.Off = Expect::Either; // the writer's stores are plain with emulation off
      t.Scalar = Expect::Either;
      T.push_back(std::move(t));
    }
  }

  // Message passing with 32-byte AVX data and flag on both sides (vector-to-vector order
  // of 32-byte accesses, which the core makes of two 16-byte halves).
  {
    auto t = MP("MP-avx", "message passing, 32-byte vmovdqu data and flag, both sides");
    t.Reset = {D.X, D.X + 8, D.X + 16, D.X + 24, D.Y, D.Y + 8, D.Y + 16, D.Y + 24};
    t.Pre = [](Asm& A, int Role) {
      WarmData(A, Role, D.X);
    };
    t.Body = [](Asm& A, int Role) {
      if (Role == 0) {
        A.VmovdquLoad(0, R8, ConstPattern);
        A.VmovdquStore(RBX, D.X, 0);
        A.VmovdquStore(RBX, D.Y, 0);
      } else {
        A.VmovdquLoad(1, RBX, D.Y);
        A.VmovdquLoad(2, RBX, D.X);
        A.VmovdquStore(R12, 0, 1);
        A.VmovdquStore(R12, 32, 2);
      }
    };
    t.Scalar = Expect::Break;
    t.Classify = [](const uint64_t*, const uint64_t* R1) {
      return StateAll({R1[0], R1[1], R1[2], R1[3]}, PATTERN) * 3 + StateAll({R1[4], R1[5], R1[6], R1[7]}, PATTERN);
    };
    T.push_back(std::move(t));
  }

  // String operations longer than a page (Intel SDM vol. 3A, "Memory-Ordering Model for
  // String Operations on Write-Back (WB) Memory": stores within one string operation may be
  // seen out of order, but string operations are not reordered with other stores, and all
  // stores of an earlier string operation complete before any store of a later one). Data
  // of 20K (crossing 4K guest pages and the 16K host page at 16384) in 64K slots, sampled
  // at its first qword, either side of the 16K boundary and its last qword. Thread 0
  // clears the data of the slot two ahead with rep stosq before the barrier (ordered by
  // the barrier's mfence in every mode).
  {
    constexpr Layout L {65536, 4, 0, 32768};
    constexpr int32_t LongBytes = 20480;
    constexpr int32_t Scratch = 40960; // reader's copy, in its own slot area
    auto ClearAhead = [](Asm& A, int Role) {
      if (Role == 0) {
        // rax holds the slot two ahead (BuildProgram), before Reset is applied.
        A.Mov(RDI, RAX);
        A.MovImm(RAX, 0);
        A.MovImm(RCX, LongBytes / 8);
        A.RepStosq();
      }
      // The reader keeps stale copies of the lines it samples.
      for (int32_t Off : {0, 16376, 16384, LongBytes - 8}) {
        WarmData(A, Role, Off);
      }
    };
    // Loads the flag into rax with LoadFlag, once on even iterations and, on odd ones,
    // again and again (at most 65536 times) until it is non-zero: the odd iterations read
    // the data just after the flag appears, where reordering shows; the even ones overlap
    // the writer at any point. rdx counts.
    auto FlagThen = [](Asm& A, const std::function<void(Asm&)>& LoadFlag) {
      A.Mov(RDX, R15);
      A.AluImm(AND, RDX, 1);
      A.Shl(RDX, 16);
      A.Inc(RDX);
      const size_t Loop = A.Here();
      LoadFlag(A);
      A.Test(RAX, RAX);
      const size_t Got = A.JccFwd(CC_NZ);
      A.Dec32(RDX);
      A.JccBack(CC_NZ, Loop);
      A.Bind(Got);
    };
    auto Samples = [](Asm& A, int Base, int32_t Off) {
      // flag already in rax; data samples at 0, 16376, 16384, LongBytes - 8.
      A.Load(RDX, Base, Off + 0);
      A.Load(RCX, Base, Off + 16376);
      A.Load(RSI, Base, Off + 16384);
      A.Load(RDI, Base, Off + LongBytes - 8);
      A.Store(R12, 0, RAX);
      A.Store(R12, 8, RDX);
      A.Store(R12, 16, RCX);
      A.Store(R12, 24, RSI);
      A.Store(R12, 32, RDI);
    };
    auto Classify = [](uint64_t Value) {
      return [Value](const uint64_t*, const uint64_t* R1) {
        return State(R1[0], 1) * 3 + StateAll({R1[1], R1[2], R1[3], R1[4]}, Value);
      };
    };
    auto Long = [&](const char* Name, const char* Desc) {
      auto t = MP(Name, Desc, L);
      t.Reset = {L.Y};
      t.Pre = ClearAhead;
      t.IterationDivisor = 20;
      return t;
    };
    {
      auto t = Long("MP-stosq-long", "message passing, 20K of data by rep stosq, flag by rep stosq");
      t.Body = [Samples, FlagThen](Asm& A, int Role) {
        if (Role == 0) {
          A.MovImm(RAX, STOS_VALUE);
          A.Mov(RDI, RBX);
          A.MovImm(RCX, LongBytes / 8);
          A.RepStosq();
          RepStosqTo(A, L.Y, 1, 1);
        } else {
          FlagThen(A, [](Asm& A) { A.Load(RAX, RBX, L.Y); });
          Samples(A, RBX, 0);
        }
      };
      t.Scalar = Expect::Break;
      t.Classify = Classify(STOS_VALUE);
      T.push_back(std::move(t));
    }
    {
      auto t = Long("MP-movsq-long", "message passing, 20K of data by rep movsq, flag by rep movsq");
      t.Body = [Samples, FlagThen](Asm& A, int Role) {
        if (Role == 0) {
          A.Mov(RSI, R8);
          A.AluImm(ADD, RSI, ConstLong);
          A.Mov(RDI, RBX);
          A.MovImm(RCX, LongBytes / 8);
          A.RepMovsq();
          A.Mov(RSI, R8);
          A.AluImm(ADD, RSI, ConstOne);
          A.Mov(RDI, RBX);
          A.AluImm(ADD, RDI, L.Y);
          A.MovImm(RCX, 1);
          A.RepMovsq();
        } else {
          FlagThen(A, [](Asm& A) { A.Load(RAX, RBX, L.Y); });
          Samples(A, RBX, 0);
        }
      };
      t.Scalar = Expect::Break;
      t.Classify = Classify(COPY_BYTE * FULL);
      T.push_back(std::move(t));
    }
    {
      auto t = Long("MP-movsq-long-read", "message passing, scalar flag after 20K rep stosq; reader copies flag and data with rep movsq");
      t.Body = [Samples, FlagThen](Asm& A, int Role) {
        if (Role == 0) {
          A.MovImm(RAX, STOS_VALUE);
          A.Mov(RDI, RBX);
          A.MovImm(RCX, LongBytes / 8);
          A.RepStosq();
          A.StoreImm(RBX, L.Y, 1);
        } else {
          // Copy the flag, then the data, into the reader's scratch area of the slot.
          FlagThen(A, [](Asm& A) {
            A.Mov(RSI, RBX);
            A.AluImm(ADD, RSI, L.Y);
            A.Mov(RDI, RBX);
            A.AluImm(ADD, RDI, Scratch - 8);
            A.MovImm(RCX, 1);
            A.RepMovsq();
            A.Load(RAX, RBX, Scratch - 8);
          });
          A.Mov(RSI, RBX);
          A.Mov(RDI, RBX);
          A.AluImm(ADD, RDI, Scratch);
          A.MovImm(RCX, LongBytes / 8);
          A.RepMovsq();
          A.Load(RAX, RBX, Scratch - 8);
          Samples(A, RBX, Scratch);
        }
      };
      // Architecturally breakable with string ordering off (plain loads may pass each
      // other), but not seen on an M4 in either mode (MP-movsb-read2 does break): reported.
      t.Off = t.Scalar = Expect::Either;
      t.Classify = Classify(STOS_VALUE);
      T.push_back(std::move(t));
    }
    {
      // A store before a long string operation: no store of the string may be seen
      // before it (Intel's "String Operations Are not Reordered with Earlier Stores").
      // The reader reads the string's last qword, then the earlier store.
      auto t = Long("MP-before-stosq-long", "scalar store, then 20K rep stosq; the string's last qword seen means the store is seen");
      t.Body = [FlagThen](Asm& A, int Role) {
        if (Role == 0) {
          A.StoreImm(RBX, L.Y, 1);
          A.MovImm(RAX, STOS_VALUE);
          A.Mov(RDI, RBX);
          A.MovImm(RCX, LongBytes / 8);
          A.RepStosq();
        } else {
          FlagThen(A, [](Asm& A) { A.Load(RAX, RBX, LongBytes - 8); });
          A.Load(RDX, RBX, L.Y);
          A.Store(R12, 0, RAX);
          A.Store(R12, 8, RDX);
        }
      };
      // Outcome as MP with the string's last qword as the flag and the store as the data.
      t.Classify = [](const uint64_t*, const uint64_t* R1) {
        return State(R1[0], STOS_VALUE) * 3 + State(R1[1], 1);
      };
      // A plain store after a release store may pass it architecturally; not seen on an M4
      // with string ordering off or scalar only (MP-flag-stosq, the short form, is the same).
      t.Off = t.Scalar = Expect::Either;
      T.push_back(std::move(t));
    }
    {
      // Stores inside one string operation seen out of order: allowed by x86 for fast
      // strings (Intel SDM vol. 1, "Fast-String Operation"; vol. 3A, Example "Stores
      // Within a String Operation May be Reordered"), so reported only, in every mode.
      // The reader reads the last qword, then the first.
      LitmusTest t {"REP-order-within", "one 20K rep stosq: last qword seen before the first (allowed by x86, reported only)", L};
      t.Labels = {"last=0 first=0", "last=0 first=set", "last=set first=0", "last=set first=set"};
      t.Forbidden = {2};
      t.Interleaved = {1};
      t.Reset = {L.Y};
      t.Pre = ClearAhead;
      t.IterationDivisor = 20;
      t.Body = [FlagThen](Asm& A, int Role) {
        if (Role == 0) {
          A.MovImm(RAX, STOS_VALUE);
          A.Mov(RDI, RBX);
          A.MovImm(RCX, LongBytes / 8);
          A.RepStosq();
        } else {
          FlagThen(A, [](Asm& A) { A.Load(RAX, RBX, LongBytes - 8); });
          A.Load(RDX, RBX, 0);
          A.Store(R12, 0, RAX);
          A.Store(R12, 8, RDX);
        }
      };
      t.Classify = [](const uint64_t*, const uint64_t* R1) {
        return int(R1[0] == STOS_VALUE) * 2 + int(R1[1] == STOS_VALUE);
      };
      t.On = t.Off = t.Scalar = Expect::Either;
      T.push_back(std::move(t));
    }
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
    t.PatchedPerRun = 4; // the two stores and the two loads
    return t;
  };
  T.push_back(Unaligned("MP-split16", "message passing, data and flag cross a 16-byte boundary", D, D.X + 12, D.Y + 28));
  T.push_back(Unaligned("MP-splitline", "message passing, data and flag cross a 128-byte cache line", D, D.X + 124, D.Y + 124));
  {
    // Slots of 32K: the data straddles a 16K host page boundary, the flag a 4K guest one.
    const Layout P {32768, 16, 16376, 20472};
    T.push_back(Unaligned("MP-split16K", "message passing, data crosses a 16K host page, flag a 4K guest page", P, P.X + 4, P.Y + 4));
    T.push_back(Unaligned("MP-split4K", "message passing, data crosses a 4K guest page, flag a 16K host page", P, P.Y + 4, P.X + 4));
  }

  // Locked operations on misaligned addresses shared by every thread of every group: every
  // form the core emulates when its operand crosses 16 bytes (or a line, or the page). The
  // total must be exact. Operands change both halves of the word where the operation
  // allows it, so that two emulations can interleave, and after its operations every
  // thread writes the neighbouring bytes that share the operand's compare-and-swap units
  // with plain stores, as neighbouring fields of a real program would be written, so that
  // the emulation's second compare-and-swap fails for bytes that are not its own. Each
  // thread takes a ticket (a native, aligned lock xadd) before and after its operations;
  // a gap between its two tickets shows that another thread ran in between, which is the
  // evidence that they contended.
  struct Neighbour {
    int32_t Offset;
    int Size; // 1, 2 or 4
  };
  struct CountForm {
    const char* Name;
    const char* Description;
    int32_t Offset;
    int Size;                                  // operand bytes
    std::function<void(Asm&, int32_t Off)> Op; // one locked operation (run Increments times)
    uint64_t Initial;
    std::function<uint64_t(uint64_t Initial, uint64_t Operations)> Want;
    std::vector<Neighbour> Neighbours;
    int32_t Counter = 0; // lock forms: the plain counter the lock guards (checked instead)
    bool PlainRelease = false;
    std::function<void(int ThreadIndex, uint64_t* Regs)> InitRegs;
  };
  constexpr int32_t Ticket = 0x3000;
  auto Mask = [](int Size, uint64_t V) {
    return Size == 8 ? V : (V & ((1ULL << (Size * 8)) - 1));
  };
  auto QwordSplit = [](int32_t Off) {
    // A qword at 4 mod 8: the lower unit holds 4 foreign bytes below, the upper 4 above.
    return std::vector<Neighbour> {{Off - 4, 4}, {Off + 8, 4}};
  };
  constexpr uint64_t K = SplitIncrement;
  auto Base = [](int Slot) {
    return 0x1000 + Slot * 0x100;
  };
  std::vector<CountForm> Forms;
  Forms.push_back({"LOCK-xadd16-split16", "lock xadd on a word crossing 16 bytes", Base(0) + 15, 2,
                   [](Asm& A, int32_t Off) {
                     A.MovImm(RAX, 0x0101);
                     A.Op({0x66, 0xF0}, false, {0x0F, 0xC1}, RAX, R8, Off);
                   },
                   0, [](uint64_t I, uint64_t N) { return I + N * 0x0101; }, {}});
  Forms.push_back({"LOCK-xadd32-split16", "lock xadd on a dword crossing 16 bytes", Base(1) + 14, 4,
                   [](Asm& A, int32_t Off) {
                     A.MovImm(RAX, 0x0001'0001);
                     A.Op({0xF0}, false, {0x0F, 0xC1}, RAX, R8, Off);
                   },
                   0, [](uint64_t I, uint64_t N) { return I + N * 0x0001'0001; }, {{Base(1) + 12, 2}, {Base(1) + 18, 2}}});
  auto Xadd64 = [&](const char* Name, const char* Desc, int32_t Off) {
    Forms.push_back({Name, Desc, Off, 8,
                     [](Asm& A, int32_t O) {
                       A.MovImm(RAX, K);
                       A.LockXadd(R8, O, RAX);
                     },
                     0, [](uint64_t I, uint64_t N) { return I + N * K; }, QwordSplit(Off)});
  };
  Xadd64("LOCK-xadd-split16", "lock xadd on a qword crossing 16 bytes", Base(2) + 12);
  Xadd64("LOCK-xadd-splitline", "lock xadd on a qword crossing a 128-byte line", Base(3) + 124);
  Xadd64("LOCK-xadd-splitpage", "lock xadd on a qword crossing the 16K page", 0x4000 - 4);
  auto AluForm = [&](const char* Name, const char* Desc, int Slot, uint8_t Opcode, uint64_t Initial,
                     std::function<uint64_t(uint64_t, uint64_t)> Want) {
    Forms.push_back({Name, Desc, Base(Slot) + 12, 8,
                     [Opcode](Asm& A, int32_t O) {
                       A.MovImm(RAX, Opcode == 0x31 ? 0x5A5A'5A5A'A5A5'A5A5ULL : K);
                       A.Op({0xF0}, true, {Opcode}, RAX, R8, O);
                     },
                     Initial, Want, QwordSplit(Base(Slot) + 12)});
  };
  AluForm("LOCK-add-split16", "lock add on a qword crossing 16 bytes", 4, 0x01, 0, [](uint64_t I, uint64_t N) { return I + N * K; });
  AluForm("LOCK-sub-split16", "lock sub on a qword crossing 16 bytes", 5, 0x29, 0, [](uint64_t I, uint64_t N) { return I - N * K; });
  // An even number of operations in all, so the value ends where it started.
  AluForm("LOCK-xor-split16", "lock xor on a qword crossing 16 bytes", 6, 0x31, 0x1122'3344'5566'7788ULL,
          [](uint64_t I, uint64_t N) { return (N & 1) ? I ^ 0x5A5A'5A5A'A5A5'A5A5ULL : I; });
  auto UnaryForm = [&](const char* Name, const char* Desc, int Slot, int32_t Within, uint8_t Opcode, int Ext, uint64_t Initial,
                       std::function<uint64_t(uint64_t, uint64_t)> Want, std::vector<Neighbour> N) {
    Forms.push_back({Name, Desc, Base(Slot) + Within, 8,
                     [Opcode, Ext](Asm& A, int32_t O) {
                       A.Op({0xF0}, true, {Opcode}, Ext, R8, O);
                     },
                     Initial, Want, N});
  };
  // inc and dec change one byte below the boundary each time and the bytes above it on
  // every 256th: the operand starts 1 byte below the 16-byte boundary.
  UnaryForm("LOCK-inc-split16", "lock inc on a qword one byte below a 16-byte boundary", 7, 15, 0xFF, 0, 0,
            [](uint64_t I, uint64_t N) { return I + N; }, {{Base(7) + 8, 4}, {Base(7) + 23, 1}});
  UnaryForm("LOCK-dec-split16", "lock dec on a qword one byte below a 16-byte boundary", 8, 15, 0xFF, 1, 0,
            [](uint64_t I, uint64_t N) { return I - N; }, {{Base(8) + 8, 4}, {Base(8) + 23, 1}});
  UnaryForm("LOCK-not-split16", "lock not on a qword crossing 16 bytes", 9, 12, 0xF7, 2, 0x0123'4567'89AB'CDEFULL,
            [](uint64_t I, uint64_t N) { return (N & 1) ? ~I : I; }, QwordSplit(Base(9) + 12));
  UnaryForm("LOCK-neg-split16", "lock neg on a qword crossing 16 bytes", 10, 12, 0xF7, 3, 0x0123'4567'89AB'CDEFULL,
            [](uint64_t I, uint64_t N) { return (N & 1) ? 0 - I : I; }, QwordSplit(Base(10) + 12));
  {
    // Each thread sets its own bit in each half with lock or and clears them with lock and.
    CountForm F {"LOCK-or-and-split16", "lock or then lock and of each thread's bits, on a qword crossing 16 bytes", Base(11) + 12, 8,
                 [](Asm& A, int32_t O) {
                   A.Op({0xF0}, true, {0x09}, RSI, R8, O);
                   A.Op({0xF0}, true, {0x21}, RDI, R8, O);
                 },
                 0, [](uint64_t, uint64_t) { return uint64_t {0}; }, QwordSplit(Base(11) + 12)};
    F.InitRegs = [](int Index, uint64_t* Regs) {
      Regs[RSI] = (1ULL << Index) | (1ULL << (Index + 32));
      Regs[RDI] = ~Regs[RSI];
    };
    Forms.push_back(std::move(F));
  }
  Forms.push_back({"LOCK-cmpxchg-split16", "lock cmpxchg increment loop on a qword crossing 16 bytes", Base(12) + 12, 8,
                   [](Asm& A, int32_t O) {
                     const size_t Retry = A.Here();
                     A.Load(RAX, R8, O);
                     A.Mov(RDX, RAX);
                     A.MovImm(RCX, K);
                     A.Add(RDX, RCX);
                     A.LockCmpxchg(R8, O, RDX);
                     A.JccBack(CC_NZ, Retry);
                   },
                   0, [](uint64_t I, uint64_t N) { return I + N * K; }, QwordSplit(Base(12) + 12)});
  Forms.push_back({"LOCK-cmpxchg8b-split16", "lock cmpxchg8b increment loop on a qword crossing 16 bytes", Base(13) + 12, 8,
                   [](Asm& A, int32_t O) {
                     const size_t Retry = A.Here();
                     A.Load(RAX, R8, O);
                     A.Mov(RBX, RAX);
                     A.MovImm(RCX, K);
                     A.Add(RBX, RCX);
                     A.Mov(RDX, RAX);
                     A.Shr(RDX, 32);
                     A.Mov(RCX, RBX);
                     A.Shr(RCX, 32);
                     A.Op({0xF0}, false, {0x0F, 0xC7}, 1, R8, O); // edx:eax against [m], ecx:ebx stored
                     A.JccBack(CC_NZ, Retry);
                   },
                   0, [](uint64_t I, uint64_t N) { return I + N * K; }, QwordSplit(Base(13) + 12)});
  // Spin locks guarding a plain counter. With a plain release the release needs TSO (the
  // counter's store must not pass it), so with emulation off a short count is allowed.
  {
    CountForm F {"LOCK-xchg-split16", "spin lock by xchg on a qword crossing 16 bytes, plain release", Base(14) + 12, 8,
                 [](Asm& A, int32_t O) {
                   const size_t Retry = A.Here();
                   A.MovImm(RAX, 1);
                   A.Xchg(R8, O, RAX);
                   A.Test(RAX, RAX);
                   A.JccBack(CC_NZ, Retry);
                   A.Load(RDX, R8, O + 0x80);
                   A.Inc(RDX);
                   A.Store(R8, O + 0x80, RDX);
                   A.StoreImm(R8, O, 0);
                 },
                 0, nullptr, QwordSplit(Base(14) + 12), Base(14) + 12 + 0x80, true};
    Forms.push_back(std::move(F));
  }
  {
    CountForm F {"LOCK-xchg2-split16", "spin lock by xchg of a value in both halves, xchg release", Base(15) + 12, 8,
                 [](Asm& A, int32_t O) {
                   const size_t Retry = A.Here();
                   A.MovImm(RAX, K);
                   A.Xchg(R8, O, RAX);
                   A.Test(RAX, RAX);
                   A.JccBack(CC_NZ, Retry);
                   A.Load(RDX, R8, O + 0x80);
                   A.Inc(RDX);
                   A.Store(R8, O + 0x80, RDX);
                   A.MovImm(RAX, 0);
                   A.Xchg(R8, O, RAX);
                 },
                 0, nullptr, QwordSplit(Base(15) + 12), Base(15) + 12 + 0x80, false};
    Forms.push_back(std::move(F));
  }
  {
    CountForm F {"LOCK-cmpxchg8b-lock", "spin lock by cmpxchg8b on a qword crossing 16 bytes, plain release", Base(16) + 12, 8,
                 [](Asm& A, int32_t O) {
                   const size_t Retry = A.Here();
                   A.MovImm(RAX, 0);
                   A.MovImm(RDX, 0);
                   A.MovImm(RBX, 1);
                   A.MovImm(RCX, 0);
                   A.Op({0xF0}, false, {0x0F, 0xC7}, 1, R8, O);
                   A.JccBack(CC_NZ, Retry);
                   A.Load(RDX, R8, O + 0x80);
                   A.Inc(RDX);
                   A.Store(R8, O + 0x80, RDX);
                   A.StoreImm(R8, O, 0);
                 },
                 0, nullptr, QwordSplit(Base(16) + 12), Base(16) + 12 + 0x80, true};
    Forms.push_back(std::move(F));
  }

  for (auto& F : Forms) {
    LitmusTest t {F.Name, F.Description, D};
    t.Labels = {"no other thread between", "another thread between"};
    t.Interleaved = {1};
    t.Off = F.PlainRelease ? Expect::Either : Expect::Hold;
    t.Scalar = Expect::Hold;
    t.IterationDivisor = 400;
    t.Body = [F](Asm& A, int) {
      A.MovImm(RAX, 1);
      A.LockXadd(R8, Ticket, RAX);
      A.Store(R12, 0, RAX);
      for (int i = 0; i < Increments; ++i) {
        F.Op(A, F.Offset);
      }
      A.MovImm(RAX, 1);
      A.LockXadd(R8, Ticket, RAX);
      A.Store(R12, 8, RAX);
      for (const auto& N : F.Neighbours) {
        if (N.Size == 4) {
          A.Store32(R8, N.Offset, R15);
        } else if (N.Size == 2) {
          A.Store16(R8, N.Offset, R15);
        } else {
          A.Store8(R8, N.Offset, R15);
        }
      }
    };
    t.Classify = [](const uint64_t* R0, const uint64_t* R1) {
      return int(R0[1] - R0[0] != 1 || R1[1] - R1[0] != 1);
    };
    t.InitShared = [F](uint8_t* Shared) {
      memcpy(Shared + F.Offset, &F.Initial, F.Size);
    };
    t.InitRegs = F.InitRegs;
    t.FinalCheck = [F, Mask](const uint8_t* Shared, uint64_t ThreadIterations) {
      const uint64_t Operations = ThreadIterations * Increments;
      uint64_t V = 0, Want;
      if (F.Counter) {
        memcpy(&V, Shared + F.Counter, 8);
        Want = Operations;
      } else {
        memcpy(&V, Shared + F.Offset, F.Size);
        Want = Mask(F.Size, F.Want(F.Initial, Operations));
      }
      if (V == Want) {
        return std::string();
      }
      char Text[96];
      snprintf(Text, sizeof(Text), "0x%llx, expected 0x%llx", static_cast<unsigned long long>(V), static_cast<unsigned long long>(Want));
      return std::string(Text);
    };
    T.push_back(std::move(t));
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

// Counts the alignment faults the library handles, to show that the misaligned tests reach
// the fault-time back-patching (and how often several threads fault on the same code).
std::atomic<uint64_t> AlignmentFaults {};

// Which guest threads took an alignment fault at each faulting instruction address (host
// PC in translated code), to tell whether several threads faulted on the same instruction
// (the concurrent case of the back-patching) rather than one thread on several, or on its
// own copy again. Filled from the fault handler: open addressing with atomics only.
constexpr size_t FaultSites = 8192;
std::atomic<uint64_t> FaultSitePC[FaultSites];
std::atomic<uint32_t> FaultSiteThreads[FaultSites];
pthread_key_t ThreadIndexKey; // index + 1 of the guest thread running on this host thread

void RecordFaultSite(uint64_t PC) {
  const auto Index = reinterpret_cast<uintptr_t>(pthread_getspecific(ThreadIndexKey));
  if (!Index || Index > 32) {
    return;
  }
  for (size_t Probe = 0; Probe < FaultSites; ++Probe) {
    auto& Slot = FaultSitePC[(PC / 4 + Probe) % FaultSites];
    uint64_t Seen = Slot.load(std::memory_order_relaxed);
    if (Seen == 0 && Slot.compare_exchange_strong(Seen, PC)) {
      Seen = PC;
    }
    if (Seen == PC) {
      FaultSiteThreads[(PC / 4 + Probe) % FaultSites].fetch_or(1U << (Index - 1), std::memory_order_relaxed);
      return;
    }
  }
}

// The largest number of distinct threads that faulted on one instruction since the last
// reset, and how many instructions faulted.
std::pair<int, size_t> FaultSiteSummary() {
  int Most = 0;
  size_t Sites = 0;
  for (size_t i = 0; i < FaultSites; ++i) {
    if (FaultSitePC[i].load()) {
      ++Sites;
      Most = std::max(Most, __builtin_popcount(FaultSiteThreads[i].load()));
    }
  }
  return {Most, Sites};
}

void ResetFaultSites() {
  for (size_t i = 0; i < FaultSites; ++i) {
    FaultSitePC[i] = 0;
    FaultSiteThreads[i] = 0;
  }
}

// Prints where a stalled thread is. Not async-signal-safe; it only runs when the tool is
// about to give up anyway.
void WhereHandler(int, siginfo_t*, void* RawContext) {
  char Line[160];
  auto& SS = static_cast<ucontext_t*>(RawContext)->uc_mcontext->__ss;
  snprintf(Line, sizeof(Line), "--- thread %p: pc 0x%llx lr 0x%llx\n", (void*)pthread_self(), (unsigned long long)SS.__pc, (unsigned long long)SS.__lr);
  write(STDOUT_FILENO, Line, strlen(Line));
  void* Frames[48];
  const int N = backtrace(Frames, 48);
  backtrace_symbols_fd(Frames, N, STDOUT_FILENO);
}

// The tool owns SIGBUS and SIGSEGV and hands every fault to the library first (the second
// way in fexmac.h), so that it can count the alignment faults the library handled; the
// library tells faults apart by the syndrome's fault status code, and so does this count
// (0x21 is an alignment fault). A fault the library does not claim is a real crash: the
// default action is restored and the fault happens again.
void FaultHandler(int Signal, siginfo_t* Info, void* RawContext) {
  const uint32_t ESR = static_cast<ucontext_t*>(RawContext)->uc_mcontext->__es.__esr;
  const uint64_t PC = static_cast<ucontext_t*>(RawContext)->uc_mcontext->__ss.__pc;
  if (fexmac_handle_fault(Signal, Info, RawContext)) {
    if ((ESR & 0x3F) == 0x21) {
      AlignmentFaults.fetch_add(1, std::memory_order_relaxed);
      RecordFaultSite(PC);
    }
    return;
  }
  signal(Signal, SIG_DFL);
}

struct GuestThread {
  int Index {}; // across all groups of the run
  uint64_t RIP {};
  uint64_t Regs[16] {};
  bool Done {};
  uint64_t FinalIteration {};
  // 0 not started, 1 thread created, 2 running guest code, 3 returned, 4 cleaned up.
  std::atomic<int> Stage {};
  pthread_t Handle {};
};

// A start barrier for every guest thread of a run, after its state is set and before it
// runs. The guest barrier lines up only the two threads of one group, and those run
// different code, so without this the threads that run the same code (the same role in
// different groups) reach their first, faulting execution of it at different times, and
// the core's back-patching is rarely entered by several threads at once.
std::atomic<unsigned> StartArrived {};
std::atomic<unsigned> StartTotal {};

// What the stall watchdog reports.
struct {
  std::mutex M;
  std::string Test;
  unsigned Run {};
  uint64_t Serial {};
  std::vector<const uint64_t*> Counters;
  std::vector<const GuestThread*> Threads;
} Progress;

// The quality of service of every guest thread (--qos). macOS has no way to pin a thread to
// a core: user-interactive (the default) makes the scheduler prefer the performance cores
// and not park the thread; background keeps it on the efficiency cores, provided no
// higher-priority thread blocks waiting for it (a thread waiting in pthread_join lends its
// priority to the thread it waits for, which then ran on the performance cores on the
// M4; the main thread therefore waits by polling, see WaitForGuests).
qos_class_t GuestQos = QOS_CLASS_USER_INTERACTIVE;
// The CPU numbers (pthread_cpu_number_np) guest threads were seen on, bit per CPU, sampled
// when a thread starts and when it returns from the guest.
std::atomic<uint64_t> CpusSeen {};

void NoteCpu() {
  size_t Cpu = 0;
  if (pthread_cpu_number_np(&Cpu) == 0 && Cpu < 64) {
    CpusSeen.fetch_or(1ULL << Cpu, std::memory_order_relaxed);
  }
}

std::string CpuList(uint64_t Mask) {
  std::string S;
  for (int c = 0; c < 64; ++c) {
    if (Mask & (1ULL << c)) {
      S += (S.empty() ? "" : ",") + std::to_string(c);
    }
  }
  return S.empty() ? "none" : S;
}

void RunGuest(fexmac_context* Context, GuestThread* Self) {
  pthread_set_qos_class_self_np(GuestQos, 0);
  NoteCpu();
  Self->Handle = pthread_self();

  pthread_setspecific(ThreadIndexKey, reinterpret_cast<void*>(static_cast<uintptr_t>(Self->Index + 1)));
  fexmac_thread* Thread {};
  if (const auto Result = fexmac_thread_create(Context, &Thread); Result != FEXMAC_OK) {
    // The others would wait for this thread at the start barrier for ever.
    fprintf(stderr, "FAIL: could not create guest thread %d: %s\n", Self->Index, fexmac_result_string(Result));
    fflush(stderr);
    _exit(1);
  }
  Self->Stage = 1;
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  fexmac_thread_get_state(Thread, &State);
  State.rip = Self->RIP;
  for (int i = 0; i < 16; ++i) {
    State.gpr[i] = Self->Regs[i];
  }
  fexmac_thread_set_state(Thread, &State);

  StartArrived.fetch_add(1);
  while (StartArrived.load(std::memory_order_acquire) < StartTotal.load(std::memory_order_relaxed)) {
  }

  Self->Stage = 2;
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  const fexmac_result Result = fexmac_thread_run(Thread, &Stop);
  NoteCpu();
  Self->Stage = 3;

  // Each guest loop ends at its hlt.
  if (Result == FEXMAC_OK && Stop.reason == FEXMAC_STOP_HALT && fexmac_thread_get_state(Thread, &State) == FEXMAC_OK) {
    Self->FinalIteration = State.gpr[R15];
    Self->Done = true;
  } else {
    fprintf(stderr, "guest thread stopped unexpectedly: %s, stop reason %d at 0x%llx\n", fexmac_result_string(Result), Stop.reason,
            static_cast<unsigned long long>(Stop.instruction_address));
  }
  fexmac_thread_destroy(Thread);
  Self->Stage = 4;
}

uint32_t RandomBelow(uint32_t N) {
  return arc4random_uniform(N);
}

size_t HostPageSize() {
  return static_cast<size_t>(getpagesize());
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
  qos_class_t Qos = QOS_CLASS_USER_INTERACTIVE;
  std::vector<std::string> Only;
};

[[noreturn]]
void Usage() {
  fprintf(stderr, "usage: FEXCoreOrderingTest [--tso on|off|scalar] [--expect tso|weak|scalar|report] [--iterations N] [--runs R]\n"
                  "                           [--groups G] [--soak] [--only name[,name...]] [--list]\n"
                  "                           [--qos interactive|utility|background]\n");
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
    } else if (A == "--qos") {
      const auto V = Next();
      if (V == "interactive") {
        O.Qos = QOS_CLASS_USER_INTERACTIVE;
      } else if (V == "utility") {
        O.Qos = QOS_CLASS_UTILITY;
      } else if (V == "background") {
        O.Qos = QOS_CLASS_BACKGROUND;
      } else {
        Usage();
      }
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

// Clock frequency seen by a thread of the guests' QoS class: 64 dependent adds per loop
// turn, one cycle each on Apple's performance and efficiency cores. Run on its own thread,
// waited for by polling (see GuestQos). Returns GHz and the CPU number it ended on.
std::pair<double, int> CalibrateAtGuestQos() {
  std::atomic<int> Done {};
  double GHz = 0;
  int Cpu = -1;
  std::thread T([&] {
    pthread_set_qos_class_self_np(GuestQos, 0);
#if defined(__aarch64__)
    auto Run = [](uint64_t N) {
      const auto T0 = std::chrono::steady_clock::now();
      uint64_t X = 0;
      for (uint64_t i = 0; i < N; ++i) {
        __asm__ volatile(".rept 64\nadd %0, %0, #1\n.endr\n" : "+r"(X));
      }
      return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - T0).count();
    };
    Run(2'000'000); // ramp up
    for (int i = 0; i < 20; ++i) {
      GHz = std::max(GHz, 64.0 * 100'000 / Run(100'000));
    }
#endif
    size_t C = 0;
    if (pthread_cpu_number_np(&C) == 0) {
      Cpu = static_cast<int>(C);
    }
    Done = 1;
  });
  while (!Done.load()) {
    usleep(1000);
  }
  T.join();
  return {GHz, Cpu};
}

unsigned HostCPUs() {
  const auto N = std::thread::hardware_concurrency();
  return N ? N : 2;
}

} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  pthread_key_create(&ThreadIndexKey, nullptr);

  const Options Opt = Parse(argc, argv);
  auto Tests = MakeTests();
  if (Opt.List) {
    for (auto& T : Tests) {
      printf("%-20s %s\n", T.Name, T.Description);
    }
    return 0;
  }

  const unsigned Groups = Opt.Groups ? Opt.Groups : std::max(1U, std::min(4U, HostCPUs() / 2));
  // With fewer than four groups (a machine with few processors, such as the hosted
  // virtual Mac), each run gives fewer tries, and rare outcomes, including the ones that
  // show the threads overlap, may not appear: run proportionally more iterations instead.
  const uint64_t BaseIterations = Opt.Soak ? 250'000 : 100'000;
  const uint64_t AllIterations = Opt.Iterations ? Opt.Iterations : BaseIterations * ((4 + Groups - 1) / Groups);
  const unsigned Runs = Opt.Runs ? Opt.Runs : (Opt.Soak ? 100 : 8);
  // Every test has two threads; the result arrays and the stall report assume it.
  constexpr int NumThreads = 2;

  if ((fexmac_version() >> 16) != FEXMAC_VERSION_MAJOR) {
    fprintf(stderr, "FAIL: fexmac version %x, built against %x\n", fexmac_version(), FEXMAC_VERSION);
    return 1;
  }
  GuestQos = Opt.Qos;
  const char* QosName = Opt.Qos == QOS_CLASS_BACKGROUND ? "background" : (Opt.Qos == QOS_CLASS_UTILITY ? "utility" : "user-interactive");
  {
    const auto [GHz, Cpu] = CalibrateAtGuestQos();
    printf("guest threads: QoS %s; a thread of that class measured %.2f GHz (best of 20) on CPU %d\n"
           "  (on the M4 Mac mini: CPUs 0-5 are efficiency cores, at most about 2.9 GHz; 6-9 performance cores, about 4.4 GHz)\n",
           QosName, GHz, Cpu);
  }
  const bool ScalarTSO = Opt.Mode != Options::TSO::Off;
  const bool OtherTSO = Opt.Mode == Options::TSO::On;
  const fexmac_option TSOOptions[] = {
    {"TSOENABLED", ScalarTSO ? "1" : "0"},
    {"VECTORTSOENABLED", OtherTSO ? "1" : "0"},
    {"MEMCPYSETTSOENABLED", OtherTSO ? "1" : "0"},
  };
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = TSOOptions;
  Config.option_count = sizeof(TSOOptions) / sizeof(TSOOptions[0]);
  fexmac_context* Context {};
  if (const auto Result = fexmac_context_create(&Config, &Context); Result != FEXMAC_OK) {
    fprintf(stderr, "FAIL: could not create the context: %s\n", fexmac_result_string(Result));
    return 1;
  }
  {
    // SA_NODEFER, as fexmac.h asks of a host that owns the fault signals: on macOS a fault
    // while its signal is blocked hangs the thread.
    struct sigaction Action {};
    Action.sa_sigaction = FaultHandler;
    Action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&Action.sa_mask);
    sigaction(SIGBUS, &Action, nullptr);
    sigaction(SIGSEGV, &Action, nullptr);
  }

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
  uint64_t AllCpus = 0;

  for (auto& T : Tests) {
    if (!Opt.Only.empty() && std::find(Opt.Only.begin(), Opt.Only.end(), T.Name) == Opt.Only.end()) {
      continue;
    }
    const uint64_t Iterations = std::max<uint64_t>(16, AllIterations / T.IterationDivisor);
    const auto TestStart = std::chrono::steady_clock::now();
    const uint64_t FaultsBefore = AlignmentFaults.load();
    const uint64_t CpusBefore = CpusSeen.exchange(0);
    ResetFaultSites();
    std::vector<uint64_t> Counts(T.Labels.size(), 0);
    bool Broken = false;
    uint64_t FinalFailures = 0;
    std::string FinalMessage;

    // A test whose threads were never seen to overlap gets up to two more sets of runs
    // (reported): on a machine with few processors two threads sometimes share a core for
    // a whole run. Every outcome of every set counts, so this cannot hide a forbidden one.
    unsigned RunLimit = Runs;
    // SB's relaxed outcome is also required, except with emulation on (or scalar) and a
    // single group: there it needs a load-acquire to pass a store-release, which two
    // threads sometimes never show over thousands of runs on a loaded machine (they still
    // must overlap, as every test must).
    const bool RelaxedRequired = T.Relaxed >= 0 && (Opt.Mode == Options::TSO::Off || Groups >= 2);
    unsigned ExtraSets = 0;
    auto Racing = [&] {
      uint64_t Overlap = 0;
      for (int I : T.Interleaved) {
        Overlap += Counts[I];
      }
      return Overlap != 0 && (!RelaxedRequired || Counts[T.Relaxed] != 0);
    };
    for (unsigned Run = 0; Run < RunLimit && !Broken; ++Run) {
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
      for (int k = 1; k <= 4; ++k) {
        memset(Shared + ConstPattern + 32 * (k - 1), 0x11 * k, 32);
      }
      memset(Shared + ConstLong, COPY_BYTE, SHARED_SIZE - ConstLong);
      const uint64_t One = 1;
      memcpy(Shared + ConstOne, &One, 8);
      if (T.InitShared) {
        T.InitShared(Shared);
      }

      std::vector<GroupMemory> Mem(Groups);
      std::vector<GuestThread> Threads(Groups * NumThreads);
      const size_t Page = HostPageSize();
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
          GT.Index = static_cast<int>(G * NumThreads + R);
          GT.RIP = reinterpret_cast<uint64_t>(Code + R * RoleCodeSize);
          GT.Regs[R14] = Iterations;
          GT.Regs[R13] = reinterpret_cast<uint64_t>(M.Results[R]);
          GT.Regs[R11] = reinterpret_cast<uint64_t>(M.Slots);
          GT.Regs[R10] = reinterpret_cast<uint64_t>(M.Counter);
          GT.Regs[R9] = reinterpret_cast<uint64_t>(M.Delays[R]);
          GT.Regs[R8] = reinterpret_cast<uint64_t>(Shared);
          if (T.InitRegs) {
            T.InitRegs(G * NumThreads + R, GT.Regs);
          }
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
      StartArrived = 0;
      StartTotal = static_cast<unsigned>(Threads.size());
      std::vector<std::thread> Host;
      for (auto& GT : Threads) {
        Host.emplace_back(RunGuest, Context, &GT);
      }
      // Poll instead of blocking in join (see GuestQos), then join the finished threads.
      for (auto& GT : Threads) {
        while (GT.Stage.load() < 4) {
          usleep(500);
        }
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
      if (Run + 1 == RunLimit && !Broken && ExtraSets < 2 && !Racing()) {
        ++ExtraSets;
        RunLimit += Runs;
      }
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
    const uint64_t Faults = AlignmentFaults.load() - FaultsBefore;
    // With emulation on and several groups, the threads of one role run the same code and
    // meet its first execution together (start barrier above), so a misaligned test must
    // have seen some instruction fault on more than one thread at once.
    const bool NeedConcurrentFaults = T.PatchedPerRun && Opt.Mode == Options::TSO::On && Groups >= 2;
    const auto [MostThreadsAtOneSite, FaultingSites] = FaultSiteSummary();
    const Expect Required = Opt.Want == Options::Expect::TSO    ? T.On :
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
    } else if (RelaxedRequired && Counts[T.Relaxed] == 0) {
      Verdict = "NOT RACING (relaxed outcome never seen)";
    } else if (NeedConcurrentFaults && MostThreadsAtOneSite < 2) {
      Verdict = "NOT CONCURRENT (no instruction faulted on two or more distinct threads: " + std::to_string(Faults) + " faults at " +
                std::to_string(FaultingSites) + " instructions)";
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
    if (ExtraSets) {
      Verdict += "; threads not seen to overlap at first, " + std::to_string(ExtraSets) + " more set(s) of " + std::to_string(Runs) + " runs";
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
    if (Faults) {
      printf("    misaligned-access faults handled: %llu over %u runs, at %zu instructions, up to %d distinct threads at one\n",
             static_cast<unsigned long long>(Faults), RunLimit, FaultingSites, MostThreadsAtOneSite);
    }
    const uint64_t TestCpus = CpusSeen.load();
    CpusSeen.fetch_or(CpusBefore);
    AllCpus |= TestCpus;
    printf("    %s, %.1fs, guest threads on CPUs %s\n", Verdict.c_str(), Seconds, CpuList(TestCpus).c_str());
  }

  const double Total = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
  printf("guest threads (QoS %s) ran on CPUs %s\n", QosName, CpuList(AllCpus).c_str());
  printf("%s: emulation %s, %d test(s) failed, %.1fs\n", Failures ? "FAIL" : "PASS", ModeName, Failures, Total);
  fflush(stdout);
  _exit(Failures ? 1 : 0);
}
