// SPDX-License-Identifier: MIT
//
// Checks the on-disk code cache with several threads and with guest code that moves.
//
// The guest program is a chain of small blocks, each adding a constant to rax, taking
// its own address with a RIP-relative `lea` and adding that to rdx, then jumping to the
// next block. The whole chain runs in a loop. Every thread runs it at once with its own
// loop count, so on a warm cache all threads ask for the same cached blocks at the same
// moment. The final rax and rdx depend on every constant and on where the code is, so a
// block loaded with a wrong relocation, or a stale block, gives a wrong answer.
//
//   FEXCoreCacheTest [--threads N] [--blocks N] [--base ADDRESS] [--variant add|sub]
//                    [--expect cold|warm|any]
//
// The cache is switched on through the environment: FEX_DISKCACHE=1 and
// FEX_DISKCACHEPATH=<dir>. --base maps the guest code at a fixed address (default:
// wherever the system puts it, which differs from run to run). --variant sub makes the
// first block subtract instead of add, which changes the guest bytes at the same
// address. --expect warm fails unless nothing was compiled and something was loaded;
// --expect cold fails if anything was loaded from the cache.
//
// Exit code: 0 if every thread got the right answer and the expectation held.

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include "CoreThreadSetup.h"
#include "DummyHandlers.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/set.h>
#include <FEXCore/Utils/IntervalList.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <optional>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>

namespace {
void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[%s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
void AssertHandler(const char* Message) {
  fprintf(stderr, "[ASSERT] %s\n", Message);
}

uint64_t CodeBase {};
uint64_t CodeSize {};
// --race: change the first block's opcode the first time the core reports a page of
// guest code as executable. The core does that after compiling a block and before
// storing it in the disk cache, so this is a write between decoding and storing.
std::atomic<bool> ModifyOnFirstMark {};
uint64_t FirstOpcodeOffset {};

class CacheTestSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override {}
  void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t, uint64_t) override {
    if (ModifyOnFirstMark.exchange(false)) {
      auto* Opcode = reinterpret_cast<volatile uint8_t*>(CodeBase + FirstOpcodeOffset);
      *Opcode = *Opcode == 0x05 ? 0x2D : 0x05;
    }
  }
  // Needed for --race (self-modifying code checks): drop stale translations, as the
  // instruction-test runner does.
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override {
    auto Lock = FEXCore::GuardSignalDeferringSectionWithFallback(Thread->CTX->GetCodeInvalidationMutex(), Thread);
    Thread->CTX->InvalidateCodeBuffersCodeRange(Start, Length);
    Thread->CTX->InvalidateThreadCachedCodeRange(Thread, Start, Length);
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  // Only the guest code mapping is executable: the cache must not hash or patch beyond it.
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    if (Address >= CodeBase && Address < CodeBase + CodeSize) {
      return {CodeBase, CodeSize, true};
    }
    return {0, 0, false};
  }
};

thread_local jmp_buf LongJump {};

// Diagnostics for a stuck thread: the watchdog sends this signal and the thread prints
// where it is. backtrace() only reads the stack, so it is safe enough for a test that is
// about to fail anyway.
void BacktraceHandler(int) {
  void* Frames[48];
  const int Count = backtrace(Frames, 48);
  char Line[64];
  const int Len = snprintf(Line, sizeof(Line), "--- thread %p\n", static_cast<void*>(pthread_self()));
  write(STDOUT_FILENO, Line, Len);
  backtrace_symbols_fd(Frames, Count, STDOUT_FILENO);
}

[[noreturn]]
void SynchronousExceptionHandler(FEXCore::Context::Context*, FEXCore::Core::CpuStateFrame*) {
  longjmp(LongJump, 1);
}

struct Program {
  std::vector<uint8_t> Bytes;
  uint64_t SumPerLoop {};   // added to rax by one pass over the chain
  uint64_t AddrPerLoop {};  // added to rdx by one pass over the chain, relative to the code base
  uint64_t BlockCount {};
  uint64_t StubOffset {}; // `mov rax, TrampolineResult; hlt`, reached only through --trampoline
  uint64_t FirstOpcodeOffset {}; // the first block's add/sub opcode byte
};

constexpr uint64_t TrampolineResult = 0x5A5A'1234'5678'A5A5ULL;

// Builds the chain. Block i:
//   add rax, imm32        (48 05 imm32), or sub rax, imm32 (48 2D imm32) for block 0 in the sub variant
//   lea rbx, [rip + 0]    (48 8D 1D 00 00 00 00): rbx = address of the next instruction
//   add rdx, rbx          (48 01 DA)
//   jmp next              (E9 rel32)
// After the chain: dec rcx; jnz start; hlt.
Program BuildProgram(uint64_t Blocks, bool SubVariant) {
  Program P;
  P.BlockCount = Blocks;
  auto Emit = [&](std::initializer_list<uint8_t> B) {
    P.Bytes.insert(P.Bytes.end(), B);
  };
  auto Emit32 = [&](uint32_t V) {
    for (int i = 0; i < 4; ++i) {
      P.Bytes.push_back(static_cast<uint8_t>(V >> (i * 8)));
    }
  };
  for (uint64_t i = 0; i < Blocks; ++i) {
    // Values of at least 0x10000 so the frontend treats them as patchable data.
    const uint32_t Imm = static_cast<uint32_t>(((i + 1) * 2654435761ULL) & 0x3FFFFFFF) | 0x10000;
    const bool Sub = SubVariant && i == 0;
    if (i == 0) {
      // The first block starts with 70 nops, so its add/sub lies beyond the 64 bytes the
      // anonymous-code key is computed from: changing it keeps the key and changes only
      // the full guest hash (used by --race).
      P.Bytes.insert(P.Bytes.end(), 70, 0x90);
      P.FirstOpcodeOffset = P.Bytes.size() + 1;
    }
    Emit({0x48, static_cast<uint8_t>(Sub ? 0x2D : 0x05)});
    Emit32(Imm);
    P.SumPerLoop += Sub ? -static_cast<uint64_t>(Imm) : static_cast<uint64_t>(Imm);
    Emit({0x48, 0x8D, 0x1D, 0x00, 0x00, 0x00, 0x00});
    P.AddrPerLoop += P.Bytes.size();
    Emit({0x48, 0x01, 0xDA});
    Emit({0xE9, 0x00, 0x00, 0x00, 0x00}); // jump to the next instruction
  }
  // dec rcx; jnz start (rel32); hlt
  Emit({0x48, 0xFF, 0xC9});
  Emit({0x0F, 0x85});
  const int32_t Back = -static_cast<int32_t>(P.Bytes.size() + 4);
  Emit32(static_cast<uint32_t>(Back));
  Emit({0xF4});
  P.StubOffset = P.Bytes.size();
  Emit({0x48, 0xB8});
  Emit32(static_cast<uint32_t>(TrampolineResult));
  Emit32(static_cast<uint32_t>(TrampolineResult >> 32));
  Emit({0xF4});
  return P;
}

struct Worker {
  uint64_t Count {};
  uint64_t RAX {};
  uint64_t RDX {};
  uint64_t RCX {};
  bool Started {};
  std::atomic<bool> Done {};
  std::atomic<FEXCore::Core::InternalThreadState*> Thread {};
  pthread_t Host {};
};

std::mutex CreateMutex;

struct StartBarrier {
  std::mutex Mutex;
  std::condition_variable CV;
  unsigned Ready {};
  bool Go {};
};

void Run(FEXCore::Context::Context* CTX, unsigned ThreadCount, StartBarrier* Barrier, uint64_t Entry, Worker* Self) {
  FEXCore::Allocator::InitializeThread();

  FEXCore::Core::InternalThreadState* Thread {};
  {
    std::lock_guard Lock {CreateMutex};
    Thread = CTX->CreateThread();
  }
  if (!Thread || !FEX::CoreThreadSetup::AllocateCallRetStack(Thread)) {
    fprintf(stderr, "FAIL: could not set up a guest thread\n");
    _exit(1);
  }
  Self->Thread = Thread;
  Self->Host = pthread_self();

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
  Frame->State.rip = Entry;
  Frame->State.gregs[FEXCore::X86State::REG_RAX] = 0;
  Frame->State.gregs[FEXCore::X86State::REG_RDX] = 0;
  Frame->State.gregs[FEXCore::X86State::REG_RCX] = Self->Count;

  // Start every thread at the same moment, so they ask for the same blocks together.
  {
    std::unique_lock Lock {Barrier->Mutex};
    if (++Barrier->Ready == ThreadCount) {
      Barrier->Go = true;
      Barrier->CV.notify_all();
    } else {
      Barrier->CV.wait(Lock, [&] { return Barrier->Go; });
    }
  }
  Self->Started = true;

  if (!setjmp(LongJump)) {
    CTX->ExecuteThread(Thread);
  }

  Self->RAX = Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
  Self->RDX = Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RDX];
  Self->RCX = Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RCX];
  Self->Done = true;
}

// Runs the program at Base on ThreadCount threads and returns how many got a wrong answer.
int RunAndCheck(FEXCore::Context::Context* CTX, unsigned ThreadCount, const Program& P, uint64_t Base, bool Trampoline) {
  StartBarrier Barrier;
  std::vector<Worker> Workers(ThreadCount);
  std::vector<std::thread> Threads;
  for (unsigned i = 0; i < ThreadCount; ++i) {
    Workers[i].Count = 1000 + i * 37;
    Threads.emplace_back(Run, CTX, ThreadCount, &Barrier, Base, &Workers[i]);
  }
  // A watchdog instead of an unbounded join, so a stuck thread is reported, not hung on.
  const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (true) {
    bool AllDone = true;
    for (auto& W : Workers) {
      AllDone &= W.Done.load();
    }
    if (AllDone) {
      break;
    }
    if (std::chrono::steady_clock::now() > Deadline) {
      for (unsigned i = 0; i < ThreadCount; ++i) {
        auto* T = Workers[i].Thread.load();
        printf("thread %u: started %d done %d rip 0x%llx rcx %llu\n", i, Workers[i].Started, Workers[i].Done.load(),
               T ? static_cast<unsigned long long>(T->CurrentFrame->State.rip) : 0ULL,
               T ? static_cast<unsigned long long>(T->CurrentFrame->State.gregs[FEXCore::X86State::REG_RCX]) : 0ULL);
      }
      fflush(stdout);
      signal(SIGUSR1, BacktraceHandler);
      for (auto& W : Workers) {
        if (!W.Done.load() && W.Host) {
          pthread_kill(W.Host, SIGUSR1);
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
      }
      printf("FAIL: threads did not finish within 20 seconds\n");
      fflush(stdout);
      _exit(3);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  for (auto& Thread : Threads) {
    Thread.join();
  }

  int Failures = 0;
  for (unsigned i = 0; i < ThreadCount; ++i) {
    const uint64_t K = Workers[i].Count;
    // Through the trampoline the chain never runs: rax is the stub's constant and the
    // counters are untouched.
    const uint64_t ExpectedRAX = Trampoline ? TrampolineResult : K * P.SumPerLoop;
    const uint64_t ExpectedRDX = Trampoline ? 0 : K * (P.AddrPerLoop + P.BlockCount * Base);
    const uint64_t ExpectedRCX = Trampoline ? K : 0;
    const bool Passed = Workers[i].Started && Workers[i].RAX == ExpectedRAX && Workers[i].RDX == ExpectedRDX && Workers[i].RCX == ExpectedRCX;
    if (!Passed) {
      printf("thread %u: rax 0x%llx (expected 0x%llx), rdx 0x%llx (expected 0x%llx), rcx %llu: WRONG\n", i,
             static_cast<unsigned long long>(Workers[i].RAX), static_cast<unsigned long long>(ExpectedRAX),
             static_cast<unsigned long long>(Workers[i].RDX), static_cast<unsigned long long>(ExpectedRDX),
             static_cast<unsigned long long>(Workers[i].RCX));
    }
    Failures += Passed ? 0 : 1;
  }
  return Failures;
}

void PrintCounters(const char* Who, const FEXCore::Context::TranslationCounters& C) {
  printf("%sCode cache: compiled %llu loaded %llu rejected %llu queued %llu\n", Who, static_cast<unsigned long long>(C.BlocksCompiled),
         static_cast<unsigned long long>(C.BlocksLoadedFromCache), static_cast<unsigned long long>(C.CacheEntriesRejected),
         static_cast<unsigned long long>(C.CacheEntriesQueued));
}
} // namespace

int main(int argc, char** argv, char** const envp) {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  unsigned ThreadCount = 8;
  uint64_t Blocks = 200;
  uint64_t FixedBase = 0;
  bool SubVariant = false;
  bool Trampoline = false;
  bool HardwareTSO = false;
  bool ForceTSO = false;
  bool Fork = false;
  bool Race = false;
  bool TearIndex = false;
  std::string_view Expect = "any";
  for (int i = 1; i < argc; ++i) {
    std::string_view Arg = argv[i];
    const char* Next = i + 1 < argc ? argv[i + 1] : nullptr;
    if (Arg == "--threads" && Next) {
      ThreadCount = static_cast<unsigned>(atoi(Next));
      ++i;
    } else if (Arg == "--blocks" && Next) {
      Blocks = strtoull(Next, nullptr, 0);
      ++i;
    } else if (Arg == "--base" && Next) {
      FixedBase = strtoull(Next, nullptr, 0);
      ++i;
    } else if (Arg == "--variant" && Next) {
      SubVariant = std::string_view(Next) == "sub";
      ++i;
    } else if (Arg == "--expect" && Next) {
      Expect = Next;
      ++i;
    } else if (Arg == "--trampoline") {
      Trampoline = true;
    } else if (Arg == "--hardware-tso") {
      HardwareTSO = true;
    } else if (Arg == "--force-tso") {
      ForceTSO = true;
    } else if (Arg == "--fork") {
      Fork = true;
    } else if (Arg == "--race") {
      Race = true;
    } else if (Arg == "--tear-index") {
      TearIndex = true;
    } else {
      fprintf(stderr,
              "usage: %s [--threads N] [--blocks N] [--base ADDRESS] [--variant add|sub] [--expect cold|warm|any]\n"
              "          [--trampoline] [--hardware-tso] [--force-tso] [--fork] [--race] [--tear-index]\n",
              argv[0]);
      return 2;
    }
  }
  if (ThreadCount == 0 || Blocks == 0) {
    return 2;
  }

  FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
  FEXCore::Config::Initialize();
  FEXCore::Config::AddLayer(FEX::Config::CreateEnvironmentLayer(envp));
  FEXCore::Config::Load();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");

  const auto P = BuildProgram(Blocks, SubVariant);
  CodeSize = (P.Bytes.size() + 0x3FFF) & ~uint64_t {0x3FFF};
  void* Map = ::mmap(reinterpret_cast<void*>(FixedBase), CodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Map == MAP_FAILED || (FixedBase && reinterpret_cast<uint64_t>(Map) != FixedBase)) {
    fprintf(stderr, "FAIL: could not map guest code%s\n", FixedBase ? " at the requested address" : "");
    return 1;
  }
  CodeBase = reinterpret_cast<uint64_t>(Map);
  FirstOpcodeOffset = P.FirstOpcodeOffset;
  memcpy(Map, P.Bytes.data(), P.Bytes.size());

  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  {
    auto HostFeatures = FEX::FetchHostFeatures(true);
    CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  }
  auto SignalDelegation = FEX::DummyHandlers::CreateSignalDelegator();
  auto SyscallHandler = fextl::make_unique<CacheTestSyscallHandler>();
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());
  if (!CTX->InitCore()) {
    fprintf(stderr, "FAIL: InitCore returned false\n");
    return 1;
  }

  // Code generation state that is not in the configuration: each must keep cached code
  // made without it from being used (and the reverse).
  if (HardwareTSO) {
    // Pretend the host orders memory like x86: the core then emits no ordering emulation
    // even with FEX_TSOENABLED=1. Results are the same single-threaded.
    CTX->SetHardwareTSOSupport(true);
  }
  if (ForceTSO) {
    std::unique_lock Lock {CTX->GetCodeInvalidationMutex()};
    FEXCore::IntervalList<uint64_t> Ranges;
    Ranges.Insert({CodeBase, CodeBase + P.Bytes.size()});
    fextl::set<uint64_t> Instructions {CodeBase};
    CTX->AddForceTSOInformation(Ranges, std::move(Instructions));
  }
  if (Trampoline) {
    // A custom IR handler at the first block: it jumps to the stub instead of running the
    // block's own bytes, so a cached copy of the block must never be used.
    CTX->AddThunkTrampolineIRHandler(CodeBase, CodeBase + P.StubOffset);
  }

  if (TearIndex) {
    // As if another process died while appending to the index, after this one opened it:
    // a record header promising more bytes than follow. This process's own appends must
    // not end up behind it.
    if (const char* Path = getenv("FEX_DISKCACHEPATH")) {
      if (DIR* Base = opendir(Path)) {
        while (auto* Bucket = readdir(Base)) {
          if (Bucket->d_name[0] == '.' || Bucket->d_type != DT_DIR) {
            continue;
          }
          const std::string BucketPath = std::string(Path) + "/" + Bucket->d_name;
          if (DIR* Files = opendir(BucketPath.c_str())) {
            while (auto* File = readdir(Files)) {
              const std::string_view Name = File->d_name;
              if (Name.size() > 8 && Name.substr(Name.size() - 8) == "_idx.foz") {
                if (FILE* F = fopen((BucketPath + "/" + File->d_name).c_str(), "ab")) {
                  uint8_t Torn[40 + 16 + 7] {};
                  memset(Torn, 'T', 39);
                  const uint32_t Header[4] = {4000, 1, 0, 4000};
                  memcpy(Torn + 40, Header, sizeof(Header));
                  memcpy(Torn + 56, "partial", 7);
                  fwrite(Torn, 1, sizeof(Torn), F);
                  fclose(F);
                }
              }
            }
            closedir(Files);
          }
        }
        closedir(Base);
      }
    }
  }

  if (Race) {
    // The first block's bytes change after it was compiled and before it is stored (see
    // ModifyOnFirstMark). The results of this run are meaningless (the guest runs with
    // full self-modifying-code checks, FEX_SMCCHECKS=full, so it recovers); what matters
    // is that the translation is not filed under the new bytes, which later runs of each
    // variant check.
    ModifyOnFirstMark = true;
    (void)RunAndCheck(CTX.get(), ThreadCount, P, CodeBase, Trampoline);
    CTX->FlushDiskCache();
    PrintCounters("race: ", CTX->GetTranslationCounters());
    printf("PASS: race run finished\n");
    fflush(stdout);
    _exit(0);
  }

  int Failures = RunAndCheck(CTX.get(), ThreadCount, P, CodeBase, Trampoline);

  if (Fork) {
    // fork with the cache in use. The child gets only this thread: it runs a changed
    // copy of the program at another address on two new threads, flushes (which must
    // return at once: the child does not write), and exits.
    const auto ChildProgram = BuildProgram(Blocks, !SubVariant);
    void* ChildMap = ::mmap(nullptr, CodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ChildMap == MAP_FAILED) {
      fprintf(stderr, "FAIL: could not map the child's guest code\n");
      return 1;
    }
    memcpy(ChildMap, ChildProgram.Bytes.data(), ChildProgram.Bytes.size());
    CTX->LockBeforeFork(nullptr);
    const pid_t Child = fork();
    if (Child == 0) {
      CTX->UnlockAfterFork(nullptr, true);
      CodeBase = reinterpret_cast<uint64_t>(ChildMap);
      const int ChildFailures = RunAndCheck(CTX.get(), 2, ChildProgram, CodeBase, false);
      const bool Flushed = CTX->FlushDiskCache();
      PrintCounters("child: ", CTX->GetTranslationCounters());
      printf("child: %d wrong, flush %s\n", ChildFailures, Flushed ? "wrote" : "skipped");
      fflush(stdout);
      _exit(ChildFailures == 0 ? 0 : 1);
    }
    CTX->UnlockAfterFork(nullptr, false);
    if (Child < 0) {
      fprintf(stderr, "FAIL: fork failed\n");
      return 1;
    }
    int Status = 0;
    bool Exited = false;
    for (int i = 0; i < 300 && !Exited; ++i) {
      Exited = waitpid(Child, &Status, WNOHANG) == Child;
      if (!Exited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    if (!Exited) {
      kill(Child, SIGKILL);
      printf("child: did not finish within 30 seconds\n");
      ++Failures;
    } else if (!WIFEXITED(Status) || WEXITSTATUS(Status) != 0) {
      printf("child: failed (status 0x%x)\n", Status);
      ++Failures;
    }
    // The parent keeps using the cache after the fork.
    Failures += RunAndCheck(CTX.get(), 2, P, CodeBase, Trampoline);
  }

  CTX->FlushDiskCache();
  const auto Counters = CTX->GetTranslationCounters();

  printf("guest code at 0x%llx, %llu blocks, %u threads\n", static_cast<unsigned long long>(CodeBase),
         static_cast<unsigned long long>(Blocks), ThreadCount);
  PrintCounters("", Counters);

  bool ExpectOk = true;
  if (Expect == "warm") {
    ExpectOk = Counters.BlocksCompiled == 0 && Counters.BlocksLoadedFromCache > 0 && Counters.CacheEntriesRejected == 0;
  } else if (Expect == "cold") {
    ExpectOk = Counters.BlocksLoadedFromCache == 0;
  }
  if (!ExpectOk) {
    printf("expectation '%.*s' not met\n", static_cast<int>(Expect.size()), Expect.data());
  }
  const bool Ok = Failures == 0 && ExpectOk;
  printf("%s: %d wrong\n", Ok ? "PASS" : "FAIL", Failures);
  fflush(stdout);
  _exit(Ok ? 0 : 1);
}
