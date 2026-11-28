// SPDX-License-Identifier: MIT
//
// The ordering switch of fexmac's TSODEFERRED option, tested from outside: a context that
// translates without memory-ordering emulation while it has one guest thread must switch
// ordering on, completely, before a second guest thread runs.
//
//   FEXCoreOrderingSwitchTest litmus [--trials N] [--reads R] [--mode deferred|on|off] [--pattern loop|call]
//                                    [--break no-wait|no-discard|keep-callret]
//                                    [--expect pass|fail|report] [--qos interactive|utility|background]
//   FEXCoreOrderingSwitchTest cost [--blocks N] [--repeat K]
//
// litmus: message passing across the switch. Each trial makes a fresh context. Guest
// thread 1 (the writer) runs a loop that stays inside one translation unit
// (MULTIBLOCK=1, closed by `jz`):
//     loop: inc rax; mov [X], rax; mov [Y], rax; mov rcx, [DONE]; test rcx, rcx; jz loop
// so with ordering deferred it is translated with plain stores, and would never leave
// that translation by itself. Once the writer is seen running, the host creates guest
// thread 2 (which makes the switch) and runs it on another host thread. Thread 2 (the
// reader) reads Y, then X, R times, and counts the reads where X < Y, then sets DONE.
// x86 forbids X < Y: X is written before Y with the same increasing value and read after
// it. With --pattern call the writer reads DONE in a called function, so the switch can
// catch it at that function's entry with a return address into the old loop on its
// call/return stack. A reader starts only once the writer has left its unordered translation, so with
// `--mode deferred` (and no --break) the count must be 0 in every trial.
//
// That the test can fail: `--mode off` (no ordering at all) shows how often the pattern
// breaks when nothing orders it; the FEXCoreOrderingSwitchTest_hooks build (linked against
// fexmac_testing) accepts --break, which makes the library's switch deliberately wrong:
// no-wait lets the reader run while the writer keeps its unordered translation,
// no-discard keeps the old translations, keep-callret leaves the call/return stacks (seen
// with --pattern call). Those must show forbidden outcomes
// (--expect fail). --mode on (ordering on from the start, no deferral) is the reference.
//
// cost: what the switch costs for a program with N translated blocks. One guest thread
// runs a chain of N blocks (`add rax, 1; jmp next`, MULTIBLOCK=0), then spins on a flag.
// While it spins the host calls fexmac_context_mark_memory_shared (the switch: discard
// every translation, stop the spinning thread at its next back-edge, wait for it) and
// times the call; then the thread runs the chain again (every block translated again:
// the recompilation burst) and a third time (all translated: the base). Reported per N.
#include <fexmac.h>
#ifdef SWITCH_TEST_HOOKS
#include "fexmac_testing.h"
#endif

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <vector>

#include <pthread.h>
#include <pthread/qos.h>

namespace {
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7, R8 = 8, R9 = 9 };

qos_class_t QoS = QOS_CLASS_USER_INTERACTIVE;

[[noreturn]] void Fail(const char* What) {
  fprintf(stderr, "FAIL: %s\n", What);
  exit(1);
}

void Quiet(fexmac_log_level Level, const char* Message, void*) {
  if (Level <= FEXMAC_LOG_ERROR) {
    fprintf(stderr, "[core] %s\n", Message);
  }
}

fexmac_context* MakeContext(const std::vector<fexmac_option>& Options) {
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = Options.data();
  Config.option_count = Options.size();
  Config.log = Quiet;
  fexmac_context* Context {};
  if (fexmac_context_create(&Config, &Context) != FEXMAC_OK) {
    Fail("fexmac_context_create");
  }
  return Context;
}

fexmac_thread* MakeThread(fexmac_context* Context, uint64_t RIP, uint64_t RBXValue, uint64_t R8Value) {
  fexmac_thread* Thread {};
  if (fexmac_thread_create(Context, &Thread) != FEXMAC_OK) {
    Fail("fexmac_thread_create");
  }
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  if (fexmac_thread_get_state(Thread, &State) != FEXMAC_OK) {
    Fail("get_state");
  }
  State.rip = RIP;
  State.gpr[RBX] = RBXValue;
  State.gpr[R8] = R8Value;
  if (fexmac_thread_set_state(Thread, &State) != FEXMAC_OK) {
    Fail("set_state");
  }
  return Thread;
}

fexmac_stop RunToStop(fexmac_thread* Thread) {
  pthread_set_qos_class_self_np(QoS, 0);
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  if (fexmac_thread_run(Thread, &Stop) != FEXMAC_OK) {
    Fail("fexmac_thread_run");
  }
  return Stop;
}

uint64_t Load(const uint8_t* Base, size_t Offset) {
  return __atomic_load_n(reinterpret_cast<const uint64_t*>(Base + Offset), __ATOMIC_ACQUIRE);
}
void Store(uint8_t* Base, size_t Offset, uint64_t Value) {
  __atomic_store_n(reinterpret_cast<uint64_t*>(Base + Offset), Value, __ATOMIC_RELEASE);
}

// ---- litmus ----
constexpr size_t X = 0, Y = 0x80, DONE = 0x100;

// clang-format off
const uint8_t Writer[] = {
  0x48, 0xFF, 0xC0,                         // loop: inc rax
  0x48, 0x89, 0x03,                         //   mov [rbx], rax          (X)
  0x48, 0x89, 0x83, 0x80, 0x00, 0x00, 0x00, //   mov [rbx+0x80], rax     (Y)
  0x48, 0x8B, 0x8B, 0x00, 0x01, 0x00, 0x00, //   mov rcx, [rbx+0x100]    (DONE)
  0x48, 0x85, 0xC9,                         //   test rcx, rcx
  0x74, 0xE7,                               //   jz loop
  0xF4,                                     //   hlt
};
// The same, with DONE read in a called function: at the switch the writer may be stopped
// at the function's entry, and the function's `ret` then returns through the call/return
// stack into the old translation of the loop unless that stack is cleared. The loop body
// repeats its two stores 16 times, so that most of the time is spent before the call and
// the switch usually finds the writer at the function's entry.
std::vector<uint8_t> WriterCall() {
  std::vector<uint8_t> P;
  constexpr int Repeats = 16;
  for (int i = 0; i < Repeats; ++i) {
    P.insert(P.end(), {0x48, 0xFF, 0xC0,                                // inc rax
                       0x48, 0x89, 0x03,                                // mov [rbx], rax          (X)
                       0x48, 0x89, 0x83, 0x80, 0x00, 0x00, 0x00});      // mov [rbx+0x80], rax     (Y)
  }
  P.insert(P.end(), {0xE8, 0x0A, 0x00, 0x00, 0x00, // call f
                     0x48, 0x85, 0xC9});           // test rcx, rcx
  const int Back = -static_cast<int>(P.size() + 6);
  P.insert(P.end(), {0x0F, 0x84});                 // jz loop (rel32)
  for (int i = 0; i < 4; ++i) {
    P.push_back(static_cast<uint8_t>(Back >> (8 * i)));
  }
  P.push_back(0xF4); // hlt
  // call rel32 = 10 (test, jz, hlt) lands here.
  // f: mov r10d, 64; l: mov rcx, [rbx+0x100] (DONE); dec r10d; jnz l; ret
  P.insert(P.end(), {0x41, 0xBA, 0x40, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x00, 0x01, 0x00, 0x00, 0x41, 0xFF, 0xCA, 0x75, 0xF4, 0xC3});
  return P;
}
const uint8_t Reader[] = {
  0x48, 0x8B, 0x93, 0x80, 0x00, 0x00, 0x00, // loop: mov rdx, [rbx+0x80] (Y)
  0x48, 0x8B, 0x33,                         //   mov rsi, [rbx]          (X)
  0x48, 0x39, 0xD6,                         //   cmp rsi, rdx
  0x73, 0x03,                               //   jae +3
  0x49, 0xFF, 0xC1,                         //   inc r9                  (X < Y: forbidden)
  0x49, 0xFF, 0xC8,                         //   dec r8
  0x75, 0xE9,                               //   jnz loop
  0x48, 0xC7, 0x83, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, // mov qword [rbx+0x100], 1
  0xF4,                                     //   hlt
};
// clang-format on

struct LitmusResult {
  uint64_t Forbidden;
  uint64_t WriterIterations;
  uint64_t DeferredBefore, DeferredAfter;
  uint64_t Stats[5];
};

LitmusResult LitmusTrial(const std::vector<fexmac_option>& Options, uint64_t Reads, bool Call) {
  constexpr size_t Size = 0x10000;
  auto* Mem = static_cast<uint8_t*>(mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Mem == MAP_FAILED) {
    Fail("mmap");
  }
  uint8_t* Code = Mem;
  uint8_t* Data = Mem + 0x8000;
  if (Call) {
    const auto P = WriterCall();
    memcpy(Code, P.data(), P.size());
  } else {
    memcpy(Code, Writer, sizeof(Writer));
  }
  memcpy(Code + 0x400, Reader, sizeof(Reader));
  const auto Base = reinterpret_cast<uint64_t>(Mem);

  fexmac_context* Context = MakeContext(Options);
  LitmusResult R {};
  R.DeferredBefore = fexmac_context_ordering_deferred(Context);
  // The call pattern needs a stack (rsp).
  fexmac_thread* T1 = MakeThread(Context, Base, Base + 0x8000, 0);
  if (Call) {
    fexmac_x86_state S;
    fexmac_x86_state_init(&S);
    fexmac_thread_get_state(T1, &S);
    S.gpr[RSP] = Base + 0x7000;
    fexmac_thread_set_state(T1, &S);
  }
  fexmac_stop Stop1 {};
  std::atomic<bool> Ended1 {};
  std::thread Host1([&] {
    Stop1 = RunToStop(T1);
    Ended1 = true;
  });
  // The writer is in its loop once X has moved well past its first translation.
  while (Load(Data, X) < 10000) {
    if (Ended1) {
      Fail("the writer stopped before the second thread was created");
    }
  }
  fexmac_thread* T2 = MakeThread(Context, Base + 0x400, Base + 0x8000, Reads);
  R.DeferredAfter = fexmac_context_ordering_deferred(Context);
  fexmac_stop Stop2 = RunToStop(T2);
  Host1.join();
  if (Stop1.reason != FEXMAC_STOP_HALT || Stop2.reason != FEXMAC_STOP_HALT) {
    Fail("a guest thread did not reach its hlt");
  }
  fexmac_x86_state S;
  fexmac_x86_state_init(&S);
  fexmac_thread_get_state(T2, &S);
  R.Forbidden = S.gpr[R9];
  if (S.gpr[R8] != 0) {
    Fail("reader did not finish its reads");
  }
  R.WriterIterations = Load(Data, X);
#ifdef SWITCH_TEST_HOOKS
  uint32_t Effective = 0;
  fexmac_testing_ordering_state(Context, &Effective, R.Stats);
#endif
  fexmac_thread_destroy(T2);
  fexmac_thread_destroy(T1);
  if (fexmac_context_destroy(Context) != FEXMAC_OK) {
    Fail("fexmac_context_destroy");
  }
  munmap(Mem, Size);
  return R;
}

int Litmus(int argc, char** argv) {
  uint64_t Trials = 2000, Reads = 100000;
  std::string Mode = "deferred", Expect, Break, Pattern = "loop";
  for (int i = 2; i < argc; ++i) {
    const std::string A = argv[i];
    auto Next = [&] {
      if (i + 1 >= argc) {
        Fail("missing value");
      }
      return std::string(argv[++i]);
    };
    if (A == "--trials") {
      Trials = strtoull(Next().c_str(), nullptr, 0);
    } else if (A == "--reads") {
      Reads = strtoull(Next().c_str(), nullptr, 0);
    } else if (A == "--mode") {
      Mode = Next();
    } else if (A == "--expect") {
      Expect = Next();
    } else if (A == "--pattern") {
      Pattern = Next();
      if (Pattern != "loop" && Pattern != "call") {
        Fail("--pattern loop|call");
      }
    } else if (A == "--break") {
      Break = Next();
    } else if (A == "--qos") {
      const auto Q = Next();
      QoS = Q == "background" ? QOS_CLASS_BACKGROUND : Q == "utility" ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE;
    } else {
      Fail(("unknown argument " + A).c_str());
    }
  }
  std::vector<fexmac_option> Options = {{"MULTIBLOCK", "1"}, {"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"MEMCPYSETTSOENABLED", "1"}};
  if (Mode == "deferred") {
    Options.push_back({"TSODEFERRED", "1"});
  } else if (Mode == "off") {
    Options[1].value = "0";
    Options[2].value = "0";
    Options[3].value = "0";
  } else if (Mode != "on") {
    Fail("--mode deferred|on|off");
  }
  if (!Break.empty()) {
#ifdef SWITCH_TEST_HOOKS
    uint32_t Flags = 0;
    if (Break == "no-wait") {
      Flags = FEXMAC_TESTING_ORDERING_NO_WAIT;
    } else if (Break == "no-discard") {
      Flags = FEXMAC_TESTING_ORDERING_NO_DISCARD;
    } else if (Break == "keep-callret") {
      Flags = FEXMAC_TESTING_ORDERING_KEEP_CALLRET;
    } else {
      Fail("--break no-wait|no-discard|keep-callret");
    }
    fexmac_testing_break_ordering_switch(Flags);
#else
    Fail("--break needs the FEXCoreOrderingSwitchTest_hooks build");
#endif
  }
  if (Expect.empty()) {
    Expect = (Mode == "off" || !Break.empty()) ? "fail" : "pass";
  }
  if (fexmac_install_fault_handlers() != FEXMAC_OK) {
    Fail("fexmac_install_fault_handlers");
  }
  printf("litmus: pattern %s, mode %s%s%s, %" PRIu64 " trials of %" PRIu64 " reads, expect %s\n", Pattern.c_str(), Mode.c_str(),
         Break.empty() ? "" : ", break ", Break.c_str(), Trials, Reads, Expect.c_str());
  uint64_t Total = 0, TrialsWith = 0, Iterations = 0, Max = 0;
  bool StateWrong = false;
  uint64_t Stats[5] {};
  const auto Start = std::chrono::steady_clock::now();
  for (uint64_t t = 0; t < Trials; ++t) {
    const auto R = LitmusTrial(Options, Reads, Pattern == "call");
    Total += R.Forbidden;
    TrialsWith += R.Forbidden != 0;
    Max = std::max(Max, R.Forbidden);
    Iterations += R.WriterIterations;
    for (int k = 0; k < 5; ++k) {
      Stats[k] += R.Stats[k];
    }
    const uint64_t WantBefore = Mode == "deferred" ? 1 : 0;
    if (R.DeferredBefore != WantBefore || R.DeferredAfter != 0) {
      StateWrong = true;
    }
  }
  const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
  printf("forbidden outcomes (X < Y after Y was read): %" PRIu64 " in %" PRIu64 " reads; trials with any: %" PRIu64 " of %" PRIu64
         " (max %" PRIu64 " in one); writer iterations %" PRIu64 "; %.1f s\n",
         Total, Trials * Reads, TrialsWith, Trials, Max, Iterations, Seconds);
#ifdef SWITCH_TEST_HOOKS
  printf("switches %" PRIu64 "; acknowledged at a resync %" PRIu64 ", at a system call or the end of a run %" PRIu64
         ", at the start of a run %" PRIu64 "; waits %" PRIu64 "\n",
         Stats[0], Stats[1], Stats[2], Stats[3], Stats[4]);
#endif
  if (StateWrong) {
    printf("FAIL: fexmac_context_ordering_deferred was not 1 before and 0 after the second thread\n");
    return 1;
  }
  if (Expect == "pass" && Total != 0) {
    printf("FAIL: forbidden outcomes after the switch\n");
    return 1;
  }
  if (Expect == "fail" && Total == 0) {
    printf("FAIL: expected forbidden outcomes (the test could not tell), saw none\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}

// ---- cost ----
constexpr size_t GO = 0x100, MARK = 0x200;

std::vector<uint8_t> ChainProgram(uint64_t Blocks) {
  std::vector<uint8_t> P;
  for (uint64_t i = 0; i < Blocks; ++i) {
    // add rax, 1; jmp +0 (the next block)
    P.insert(P.end(), {0x48, 0x83, 0xC0, 0x01, 0xEB, 0x00});
  }
  // inc rdx; mov [rbx+0x200], rdx
  P.insert(P.end(), {0x48, 0xFF, 0xC2, 0x48, 0x89, 0x93, 0x00, 0x02, 0x00, 0x00});
  const size_t Spin = P.size();
  // spin: mov rcx, [rbx+0x100]; cmp rcx, rdx; jb spin
  P.insert(P.end(), {0x48, 0x8B, 0x8B, 0x00, 0x01, 0x00, 0x00, 0x48, 0x39, 0xD1, 0x72});
  P.push_back(static_cast<uint8_t>(Spin - (P.size() + 1)));
  // cmp rdx, 3; jb start (rel32)
  P.insert(P.end(), {0x48, 0x83, 0xFA, 0x03, 0x0F, 0x82});
  const int32_t Rel = -static_cast<int32_t>(P.size() + 4);
  for (int i = 0; i < 4; ++i) {
    P.push_back(static_cast<uint8_t>(Rel >> (8 * i)));
  }
  P.push_back(0xF4);
  return P;
}

double Ms(std::chrono::steady_clock::time_point A, std::chrono::steady_clock::time_point B) {
  return std::chrono::duration<double, std::milli>(B - A).count();
}

uint64_t Compiled(fexmac_context* Context) {
  fexmac_translation_counters C;
  fexmac_translation_counters_init(&C);
  fexmac_context_get_counters(Context, &C);
  return C.blocks_compiled;
}

int Cost(int argc, char** argv) {
  std::vector<uint64_t> Sizes;
  int Repeat = 3;
  for (int i = 2; i < argc; ++i) {
    const std::string A = argv[i];
    if (A == "--blocks" && i + 1 < argc) {
      Sizes.push_back(strtoull(argv[++i], nullptr, 0));
    } else if (A == "--repeat" && i + 1 < argc) {
      Repeat = atoi(argv[++i]);
    } else {
      Fail(("unknown argument " + A).c_str());
    }
  }
  if (Sizes.empty()) {
    Sizes = {100, 10000, 100000};
  }
  if (fexmac_install_fault_handlers() != FEXMAC_OK) {
    Fail("fexmac_install_fault_handlers");
  }
  printf("%8s %6s %12s %14s %14s %14s %12s %12s\n", "blocks", "run", "first ms", "switch ms", "again ms", "base ms", "compiled 1",
         "compiled 2");
  for (uint64_t N : Sizes) {
    for (int r = 0; r < Repeat; ++r) {
      const auto Program = ChainProgram(N);
      const size_t CodeSize = (Program.size() + 0x3FFF) & ~size_t(0x3FFF);
      const size_t Size = CodeSize + 0x4000;
      auto* Mem = static_cast<uint8_t*>(mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
      if (Mem == MAP_FAILED) {
        Fail("mmap");
      }
      memcpy(Mem, Program.data(), Program.size());
      uint8_t* Data = Mem + CodeSize;
      const auto Base = reinterpret_cast<uint64_t>(Mem);
      fexmac_context* Context = MakeContext({{"MULTIBLOCK", "0"}, {"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"},
                                             {"MEMCPYSETTSOENABLED", "1"}, {"TSODEFERRED", "1"}});
      fexmac_thread* T = MakeThread(Context, Base, reinterpret_cast<uint64_t>(Data), 0);
      fexmac_stop Stop {};
      const auto T0 = std::chrono::steady_clock::now();
      std::thread Host([&] { Stop = RunToStop(T); });
      while (Load(Data, MARK) < 1) {
      }
      const auto T1 = std::chrono::steady_clock::now();
      const uint64_t C1 = Compiled(Context);
      if (fexmac_context_ordering_deferred(Context) != 1) {
        Fail("not deferred before the switch");
      }
      if (fexmac_context_mark_memory_shared(Context) != FEXMAC_OK) {
        Fail("fexmac_context_mark_memory_shared");
      }
      const auto T2 = std::chrono::steady_clock::now();
      Store(Data, GO, 1);
      while (Load(Data, MARK) < 2) {
      }
      const auto T3 = std::chrono::steady_clock::now();
      const uint64_t C2 = Compiled(Context) - C1;
      Store(Data, GO, 2);
      while (Load(Data, MARK) < 3) {
      }
      const auto T4 = std::chrono::steady_clock::now();
      Store(Data, GO, 3);
      Host.join();
      if (Stop.reason != FEXMAC_STOP_HALT || fexmac_context_ordering_deferred(Context) != 0) {
        Fail("cost program did not end at its hlt with ordering on");
      }
      fexmac_x86_state S;
      fexmac_x86_state_init(&S);
      fexmac_thread_get_state(T, &S);
      if (S.gpr[RAX] != 3 * N) {
        Fail("wrong count");
      }
      printf("%8" PRIu64 " %6d %12.3f %14.3f %14.3f %14.3f %12" PRIu64 " %12" PRIu64 "\n", N, r, Ms(T0, T1), Ms(T1, T2), Ms(T2, T3), Ms(T3, T4),
             C1, C2);
      fflush(stdout);
      fexmac_thread_destroy(T);
      fexmac_context_destroy(Context);
      munmap(Mem, Size);
    }
  }
  printf("first: run start to the end of the first pass (translating every block, without ordering); switch: "
         "fexmac_context_mark_memory_shared while the thread spins (discard + stop + wait); again: the second pass "
         "(every block translated again, with ordering); base: the third pass (nothing to translate)\n");
  return 0;
}
} // namespace

int main(int argc, char** argv) {
  if ((fexmac_version() >> 16) != FEXMAC_VERSION_MAJOR) {
    Fail("fexmac version");
  }
  if (argc >= 2 && strcmp(argv[1], "litmus") == 0) {
    return Litmus(argc, argv);
  }
  if (argc >= 2 && strcmp(argv[1], "cost") == 0) {
    return Cost(argc, argv);
  }
  fprintf(stderr, "usage: %s litmus|cost [options] (see Main.cpp)\n", argv[0]);
  return 2;
}
