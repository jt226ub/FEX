// SPDX-License-Identifier: MIT
//
// Checks the on-disk code cache with several threads and with guest code that moves,
// through the fexmac interface (and, for state the configuration does not carry, its
// test-only hooks).
//
// The guest program is a chain of small blocks, each adding a constant to rax, taking
// its own address with a RIP-relative `lea` and adding that to rdx, then jumping to the
// next block. The whole chain runs in a loop. Every thread runs it at once with its own
// loop count, so on a warm cache all threads ask for the same cached blocks at the same
// moment. The final rax and rdx depend on every constant and on where the code is, so a
// block loaded with a wrong relocation, or a stale block, gives a wrong answer.
//
//   FEXCoreCacheTest [--threads N] [--blocks N] [--base ADDRESS] [--variant add|sub]
//                    [--expect cold|warm|any] [--trampoline] [--hardware-tso]
//                    [--force-tso] [--fork] [--race] [--tear-index]
//
// The cache is switched on through the environment, as for the instruction-test runner:
// FEX_DISKCACHE=1 and FEX_DISKCACHEPATH=<dir> become fexmac's code_cache_* fields; other
// FEX_* variables naming fexmac options are passed as options. --base maps the guest code
// at a fixed address (default: wherever the system puts it, which differs from run to
// run). --variant sub makes the first block subtract instead of add, which changes the
// guest bytes at the same address. --expect warm fails unless nothing was compiled and
// something was loaded; --expect cold fails if anything was loaded from the cache.
//
// Exit code: 0 if every thread got the right answer and the expectation held.

#include "fexmac.h"
#include "fexmac_testing.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <execinfo.h>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
enum { RAX = 0, RCX = 1, RDX = 2 };

void LogHandler(fexmac_log_level Level, const char* Message, void*) {
  static const char* Names[] = {"?", "A", "E", "D", "I"};
  fprintf(stderr, "[%s] %s\n", Level >= 1 && Level <= 4 ? Names[Level] : "?", Message);
}

uint64_t CodeBase {};
uint64_t CodeSize {};
uint64_t FirstOpcodeOffset {};
// --race: change the first block's opcode the first time the core reports guest code as
// executable, which it does after compiling a block and before storing it in the cache.
std::atomic<bool> ModifyOnFirstMark {};

void MarkHook(uint64_t, uint64_t, void*) {
  if (ModifyOnFirstMark.exchange(false)) {
    auto* Opcode = reinterpret_cast<volatile uint8_t*>(CodeBase + FirstOpcodeOffset);
    *Opcode = *Opcode == 0x05 ? 0x2D : 0x05;
  }
}

// Diagnostics for a stuck thread: the watchdog sends this signal and the thread prints
// where it is. backtrace() only reads the stack, so it is safe enough for a test that is
// about to fail anyway.
void BacktraceHandler(int) {
  void* Frames[48];
  const int Count = backtrace(Frames, 48);
  char Line[64];
  const int Len = snprintf(Line, sizeof(Line), "--- thread %p\n", static_cast<void*>(pthread_self()));
  (void)!write(STDOUT_FILENO, Line, Len);
  backtrace_symbols_fd(Frames, Count, STDOUT_FILENO);
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
Program BuildProgram(uint64_t Blocks, bool SubVariant, bool Distinct = false) {
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
    if (Distinct && i != 0) {
      // --distinct: a different number of leading nops makes every block's bytes, and so
      // its cache entry, different.
      P.Bytes.insert(P.Bytes.end(), i % 61, 0x90);
    }
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
  bool Halted {};
  std::atomic<bool> Done {};
  pthread_t Host {};
};

struct StartBarrier {
  std::mutex Mutex;
  std::condition_variable CV;
  unsigned Ready {};
  bool Go {};
};

void Run(fexmac_context* Context, unsigned ThreadCount, StartBarrier* Barrier, uint64_t Entry, Worker* Self) {
  fexmac_thread* Thread {};
  if (fexmac_thread_create(Context, &Thread) != FEXMAC_OK) {
    fprintf(stderr, "FAIL: could not create a guest thread\n");
    _exit(1);
  }
  Self->Host = pthread_self();
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  fexmac_thread_get_state(Thread, &State);
  State.rip = Entry;
  State.gpr[RAX] = 0;
  State.gpr[RDX] = 0;
  State.gpr[RCX] = Self->Count;
  fexmac_thread_set_state(Thread, &State);

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

  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  const auto Result = fexmac_thread_run(Thread, &Stop);
  Self->Halted = Result == FEXMAC_OK && Stop.reason == FEXMAC_STOP_HALT;
  fexmac_thread_get_state(Thread, &State);
  Self->RAX = State.gpr[RAX];
  Self->RDX = State.gpr[RDX];
  Self->RCX = State.gpr[RCX];
  fexmac_thread_destroy(Thread);
  Self->Done = true;
}

// Runs the program at Base on ThreadCount threads and returns how many got a wrong answer.
int RunAndCheck(fexmac_context* Context, unsigned ThreadCount, const Program& P, uint64_t Base, bool Trampoline) {
  StartBarrier Barrier;
  std::vector<Worker> Workers(ThreadCount);
  std::vector<std::thread> Threads;
  for (unsigned i = 0; i < ThreadCount; ++i) {
    Workers[i].Count = 1000 + i * 37;
    Threads.emplace_back(Run, Context, ThreadCount, &Barrier, Base, &Workers[i]);
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
        printf("thread %u: started %d done %d\n", i, Workers[i].Started, Workers[i].Done.load());
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
    const bool Passed = Workers[i].Halted && Workers[i].RAX == ExpectedRAX && Workers[i].RDX == ExpectedRDX && Workers[i].RCX == ExpectedRCX;
    if (!Passed) {
      printf("thread %u: halted %d rax 0x%llx (expected 0x%llx), rdx 0x%llx (expected 0x%llx), rcx %llu: WRONG\n", i, Workers[i].Halted,
             static_cast<unsigned long long>(Workers[i].RAX), static_cast<unsigned long long>(ExpectedRAX),
             static_cast<unsigned long long>(Workers[i].RDX), static_cast<unsigned long long>(ExpectedRDX),
             static_cast<unsigned long long>(Workers[i].RCX));
    }
    Failures += Passed ? 0 : 1;
  }
  return Failures;
}

fexmac_translation_counters GetCounters(fexmac_context* Context) {
  fexmac_translation_counters C;
  fexmac_translation_counters_init(&C);
  fexmac_context_get_counters(Context, &C);
  return C;
}

void PrintCounters(const char* Who, const fexmac_translation_counters& C) {
  printf("%sCode cache: compiled %llu loaded %llu rejected %llu queued %llu\n", Who, static_cast<unsigned long long>(C.blocks_compiled),
         static_cast<unsigned long long>(C.blocks_loaded), static_cast<unsigned long long>(C.cache_entries_rejected),
         static_cast<unsigned long long>(C.cache_entries_queued));
  printf("%sCode cache state: %d dropped %llu replaced %u reset %u\n", Who, static_cast<int>(C.cache_state),
         static_cast<unsigned long long>(C.cache_entries_dropped), C.untrusted_files_replaced, C.damaged_databases_reset);
}

// As if another process died while appending to the index, after this one opened it: a
// record header promising more bytes than follow.
void TearIndexes(const char* Path) {
  if (!Path) {
    return;
  }
  DIR* Base = opendir(Path);
  if (!Base) {
    return;
  }
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
} // namespace

int main(int argc, char** argv) {
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
  bool ForkRaw = false;
  bool Distinct = false;
  const char* StallLock = nullptr;
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
    } else if (Arg == "--distinct") {
      Distinct = true;
    } else if (Arg == "--fork-raw") {
      Fork = true;
      ForkRaw = true;
    } else if (Arg == "--stall-lock" && Next) {
      StallLock = Next;
      ++i;
    } else {
      fprintf(stderr,
              "usage: %s [--threads N] [--blocks N] [--base ADDRESS] [--variant add|sub] [--expect cold|warm|any]\n"
              "          [--trampoline] [--hardware-tso] [--force-tso] [--fork] [--fork-raw] [--race] [--tear-index]\n"
              "          [--stall-lock hold|stop] [--distinct]\n",
              argv[0]);
      return 2;
    }
  }
  if (ThreadCount == 0 || Blocks == 0) {
    return 2;
  }

  const auto P = BuildProgram(Blocks, SubVariant, Distinct);
  CodeSize = (P.Bytes.size() + 0x3FFF) & ~uint64_t {0x3FFF};
  void* Map = ::mmap(reinterpret_cast<void*>(FixedBase), CodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Map == MAP_FAILED || (FixedBase && reinterpret_cast<uint64_t>(Map) != FixedBase)) {
    fprintf(stderr, "FAIL: could not map guest code%s\n", FixedBase ? " at the requested address" : "");
    return 1;
  }
  CodeBase = reinterpret_cast<uint64_t>(Map);
  FirstOpcodeOffset = P.FirstOpcodeOffset;
  memcpy(Map, P.Bytes.data(), P.Bytes.size());

  // Options and the cache from the environment.
  std::vector<std::string> Names, Values;
  for (char** Var = environ; Var && *Var; ++Var) {
    const std::string_view Entry {*Var};
    const auto Equals = Entry.find('=');
    if (!Entry.starts_with("FEX_") || Equals == std::string_view::npos) {
      continue;
    }
    std::string Name {Entry.substr(4, Equals - 4)};
    if (fexmac_option_known(Name.c_str())) {
      Names.push_back(Name);
      Values.emplace_back(Entry.substr(Equals + 1));
    }
  }
  std::vector<fexmac_option> Options;
  for (size_t i = 0; i < Names.size(); ++i) {
    Options.push_back({Names[i].c_str(), Values[i].c_str()});
  }
  const char* CacheEnv = getenv("FEX_DISKCACHE");
  const char* CachePath = getenv("FEX_DISKCACHEPATH");

  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = Options.data();
  Config.option_count = Options.size();
  Config.log = LogHandler;
  if (CacheEnv && std::string_view {CacheEnv} == "1") {
    Config.code_cache_enabled = 1;
    Config.code_cache_directory = CachePath && *CachePath ? CachePath : nullptr;
  }
  fexmac_context* Context {};
  if (const auto Result = fexmac_context_create(&Config, &Context); Result != FEXMAC_OK) {
    fprintf(stderr, "FAIL: could not create the context: %s\n", fexmac_result_string(Result));
    return 1;
  }
  if (fexmac_install_fault_handlers() != FEXMAC_OK) {
    fprintf(stderr, "FAIL: could not install the fault handlers\n");
    return 1;
  }
  // Only the guest code mapping is code: the cache hashes and patches nothing beyond it.
  fexmac_memory_map(Context, CodeBase, CodeSize, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE | FEXMAC_PROT_EXEC);

  // Code generation state that is not in the configuration: each must keep cached code
  // made without it from being used (and the reverse).
  if (HardwareTSO) {
    // Pretend the host orders memory like x86: the core then emits no ordering emulation
    // even with FEX_TSOENABLED=1. Results are the same single-threaded.
    fexmac_testing_set_hardware_tso(Context, 1);
  }
  if (ForceTSO) {
    fexmac_testing_force_tso(Context, CodeBase, CodeBase + P.Bytes.size(), CodeBase);
  }
  if (Trampoline) {
    // A custom IR handler at the first block: it jumps to the stub instead of running the
    // block's own bytes, so a cached copy of the block must never be used.
    fexmac_testing_add_thunk_trampoline(Context, CodeBase, CodeBase + P.StubOffset);
  }
  if (TearIndex) {
    TearIndexes(CachePath);
  }

  // --stall-lock: another process takes the database's locks after this one opened it and
  // keeps them (hold: sleeping; stop: stopped with SIGSTOP while holding them). The run's
  // stores must then be dropped after one wait, so the flush below returns promptly.
  pid_t LockHolder = -1;
  if (StallLock) {
    int Ready[2];
    if (pipe(Ready) != 0) {
      return 1;
    }
    LockHolder = fork();
    if (LockHolder == 0) {
      std::vector<int> Held;
      if (DIR* Base = opendir(CachePath ? CachePath : ".")) {
        while (auto* Bucket = readdir(Base)) {
          if (Bucket->d_name[0] == '.' || Bucket->d_type != DT_DIR) {
            continue;
          }
          const std::string BucketPath = std::string(CachePath) + "/" + Bucket->d_name;
          if (DIR* Files = opendir(BucketPath.c_str())) {
            while (auto* File = readdir(Files)) {
              const std::string_view Name = File->d_name;
              if (Name.size() > 4 && Name.substr(Name.size() - 4) == ".foz") {
                const int FD = open((BucketPath + "/" + File->d_name).c_str(), O_RDONLY);
                if (FD >= 0 && flock(FD, LOCK_EX) == 0) {
                  Held.push_back(FD);
                }
              }
            }
            closedir(Files);
          }
        }
        closedir(Base);
      }
      const char Count = static_cast<char>(Held.size());
      (void)!write(Ready[1], &Count, 1);
      if (std::string_view {StallLock} == "stop") {
        raise(SIGSTOP);
      }
      sleep(120);
      _exit(0);
    }
    char Count = 0;
    (void)!read(Ready[0], &Count, 1);
    printf("lock holder %d holds %d files\n", static_cast<int>(LockHolder), Count);
  }

  if (Race) {
    // The first block's bytes change after it was compiled and before it is stored. The
    // results of this run are meaningless; what matters is that the translation is not
    // filed under the new bytes, which later runs of each variant check.
    ModifyOnFirstMark = true;
    fexmac_testing_set_mark_executable_hook(MarkHook, nullptr);
    (void)RunAndCheck(Context, ThreadCount, P, CodeBase, Trampoline);
    fexmac_context_flush_cache(Context);
    PrintCounters("race: ", GetCounters(Context));
    printf("PASS: race run finished\n");
    fflush(stdout);
    _exit(0);
  }

  int Failures = RunAndCheck(Context, ThreadCount, P, CodeBase, Trampoline);

  if (Fork) {
    // fork with the cache in use. The child gets only this thread: it runs a changed copy
    // of the program at another address on two new threads, flushes (which must refuse at
    // once: the child does not write), and exits.
    const auto ChildProgram = BuildProgram(Blocks, !SubVariant);
    void* ChildMap = ::mmap(nullptr, CodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ChildMap == MAP_FAILED) {
      fprintf(stderr, "FAIL: could not map the child's guest code\n");
      return 1;
    }
    memcpy(ChildMap, ChildProgram.Bytes.data(), ChildProgram.Bytes.size());
    fexmac_memory_map(Context, reinterpret_cast<uint64_t>(ChildMap), CodeSize, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE | FEXMAC_PROT_EXEC);
    // --fork-raw: fork without fexmac_before_fork / fexmac_after_fork. No other host
    // thread is inside the library here, so its own locks are free; the cache must notice
    // the fork by itself and turn read-only in the child instead of waiting for the
    // parent's writer thread.
    if (!ForkRaw && fexmac_before_fork(Context) != FEXMAC_OK) {
      fprintf(stderr, "FAIL: fexmac_before_fork\n");
      return 1;
    }
    const pid_t Child = fork();
    if (Child == 0) {
      if (!ForkRaw) {
        fexmac_after_fork(Context, 1);
      }
      CodeBase = reinterpret_cast<uint64_t>(ChildMap);
      const int ChildFailures = RunAndCheck(Context, 2, ChildProgram, CodeBase, false);
      const auto Flush = fexmac_context_flush_cache(Context);
      const auto C = GetCounters(Context);
      PrintCounters("child: ", C);
      const bool StateOk = !Config.code_cache_enabled || C.cache_state == FEXMAC_CACHE_READ_ONLY;
      printf("child: %d wrong, flush %s, cache state %s\n", ChildFailures, Flush == FEXMAC_OK ? "wrote" : "skipped",
             StateOk ? "as expected" : "WRONG");
      fflush(stdout);
      _exit(ChildFailures == 0 && StateOk ? 0 : 1);
    }
    if (!ForkRaw) {
      fexmac_after_fork(Context, 0);
    }
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
    Failures += RunAndCheck(Context, 2, P, CodeBase, Trampoline);
  }

  const auto FlushStart = std::chrono::steady_clock::now();
  fexmac_context_flush_cache(Context);
  const double FlushSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - FlushStart).count();
  const auto Counters = GetCounters(Context);
  if (LockHolder > 0) {
    kill(LockHolder, SIGKILL);
    waitpid(LockHolder, nullptr, 0);
  }
  printf("flush took %.2f s\n", FlushSeconds);

  printf("guest code at 0x%llx, %llu blocks, %u threads\n", static_cast<unsigned long long>(CodeBase),
         static_cast<unsigned long long>(Blocks), ThreadCount);
  PrintCounters("", Counters);

  bool ExpectOk = true;
  if (Expect == "warm") {
    ExpectOk = Counters.blocks_compiled == 0 && Counters.blocks_loaded > 0 && Counters.cache_entries_rejected == 0;
  } else if (Expect == "cold") {
    ExpectOk = Counters.blocks_loaded == 0;
  }
  if (!ExpectOk) {
    printf("expectation '%.*s' not met\n", static_cast<int>(Expect.size()), Expect.data());
  }
  const bool Ok = Failures == 0 && ExpectOk;
  printf("%s: %d wrong\n", Ok ? "PASS" : "FAIL", Failures);
  fflush(stdout);
  _exit(Ok ? 0 : 1);
}
