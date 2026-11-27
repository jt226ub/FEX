/* SPDX-License-Identifier: MIT */
/*
 * Tests of fexmac behaviour that needs the library's test-only hooks (fexmac_testing.h),
 * linked against the fexmac_testing build of the library. The production library has no
 * hooks.
 *
 * Usage: fexmac_hook_test            run every subtest
 *        fexmac_hook_test <name>     run one subtest in this process
 */
#include "TestSupport.h"

#include <inttypes.h>
#include "fexmac_testing.h"

#include <inttypes.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* A bug inside the library's handler (the hook writes to an inaccessible page) must
 * crash with a message, not hang. Shared[0] counts how often the hook fired: exactly
 * once proves the death came from the hook and not from something else. */
static void TestFaultInsideHandlerDies(void) {
  fexmac_install_fault_handlers();
  fexmac_testing_fault_inside_handler(1, &Shared[0]);
  uint8_t* Page = MapGuestMemory((size_t)PageSize);
  mprotect(Page, (size_t)PageSize, PROT_NONE);
  ((volatile uint8_t*)Page)[0] = 1;
  printf("  FAIL: survived a fault inside the fault handler\n");
  exit(1);
}

static void CountingLog(fexmac_log_level Level, const char* Message, void* UserData) {
  (void)Level;
  (void)UserData;
  (void)Message;
  Shared[0] += 1;
}

/* A core message produced while the library handles a fault never reaches the log
 * callback; it goes to standard error with write(2). */
static void TestNoLogCallbackInHandler(void) {
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* Data = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xF0, 0x0F, 0xC1, 0x07, 0xF4}; /* lock xadd [rdi], eax ; hlt */
  memcpy(Code, Program, sizeof(Program));
  fexmac_install_fault_handlers();
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.log = CountingLog;
  fexmac_context* Context = NULL;
  if (fexmac_context_create(&Config, &Context) != FEXMAC_OK) {
    exit(2);
  }
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = (uint64_t)(uintptr_t)Data + 64 + 14; /* misaligned across 16 bytes */
  SetState(Thread, &State);
  fexmac_testing_log_from_fault_handler(1);
  const fexmac_stop Stop = Run(Thread);
  fexmac_testing_log_from_fault_handler(0);
  Check(Stop.reason == FEXMAC_STOP_HALT, "the misaligned atomic was handled");
  Check(Shared[0] == 0, "the log callback was not called from the signal handler (%u calls)", Shared[0]);
  fexmac_testing_log("outside the handler");
  Check(Shared[0] == 1, "outside a handler the log callback is called");
  Shared[0] = 7; /* driver expects this value: the counts above held */
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* A failed fexmac_context_create leaves no log hook behind. */
static void TestLogRestoredAfterFailedCreate(void) {
  const fexmac_option Bad = {"NOSUCHOPTION", "1"};
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = &Bad;
  Config.option_count = 1;
  Config.log = CountingLog;
  fexmac_context* Context = NULL;
  CheckResult(fexmac_context_create(&Config, &Context), FEXMAC_ERROR_BAD_CONFIG, "create with a bad option");
  const uint32_t AfterFailure = Shared[0];
  Check(AfterFailure == 1, "the bad option was reported through the log callback (%u calls)", AfterFailure);
  fexmac_testing_log("after the failed create");
  Check(Shared[0] == AfterFailure, "after the failure the callback is no longer called");

  /* The same after a successful create and destroy. */
  Config.options = NULL;
  Config.option_count = 0;
  CheckResult(fexmac_context_create(&Config, &Context), FEXMAC_OK, "create");
  CheckResult(fexmac_context_destroy(Context), FEXMAC_OK, "destroy");
  const uint32_t AfterDestroy = Shared[0];
  fexmac_testing_log("after destroy");
  Check(Shared[0] == AfterDestroy, "after destroy the callback is no longer called");
}

/* ---- invalidation from a non-guest thread leaves fault signals unblocked ---- */

static uint8_t* FaultPage;

static void FixPage(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Context;
  if ((uint8_t*)Info->si_addr >= FaultPage && (uint8_t*)Info->si_addr < FaultPage + PageSize) {
    mprotect(FaultPage, (size_t)PageSize, PROT_READ | PROT_WRITE);
    Shared[1] += 1;
  }
}

static void InsideInvalidation(void) {
  sigset_t Current;
  pthread_sigmask(SIG_BLOCK, NULL, &Current);
  Shared[2] = (uint32_t)(!sigismember(&Current, SIGSEGV) && !sigismember(&Current, SIGBUS) && !sigismember(&Current, SIGILL) &&
                         sigismember(&Current, SIGUSR1) && sigismember(&Current, SIGALRM));
  /* A real fault here must reach its handler, not hang. */
  ((volatile uint8_t*)FaultPage)[0] = 1;
}

static void TestInvalidationMask(void) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = FixPage;
  Action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGBUS, &Action, NULL);
  sigaction(SIGSEGV, &Action, NULL);
  FaultPage = MapGuestMemory((size_t)PageSize);
  mprotect(FaultPage, (size_t)PageSize, PROT_NONE);
  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_testing_set_invalidation_hook(InsideInvalidation);
  CheckResult(fexmac_invalidate(Context, 0x100000000ULL, 0x1000), FEXMAC_OK, "invalidate from a host thread");
  fexmac_testing_set_invalidation_hook(NULL);
  Check(Shared[2] == 1, "while invalidating, fault signals are unblocked and asynchronous ones blocked");
  Check(Shared[1] == 1, "a fault while holding the invalidation lock reached its handler");
  fexmac_context_destroy(Context);
}

/* ---- guard pages: only the core's own accesses are claimed ---- */

/* Runs `Program` with rdi pointing into one of the thread's guard pages (Which: 0 low
 * call/return guard, 1 high call/return guard, 2 scratch-buffer guard). The thread runs
 * a block first so that it has a scratch buffer. */
static fexmac_thread* GuardThread;
static uint8_t* GuardCode;

static uint64_t GuardAddress(int Which) {
  uint64_t Low = 0, High = 0, Scratch = 0;
  fexmac_testing_guard_pages(GuardThread, &Low, &High, &Scratch);
  return (Which == 0 ? Low : Which == 1 ? High : Scratch) + 64;
}

static void GuestTouchesGuard(int Which, const uint8_t* Program, size_t Size) {
  fexmac_install_fault_handlers();
  GuardCode = MapGuestMemory(0x10000);
  const uint8_t Warm[] = {0xB8, 0x01, 0, 0, 0, 0xF4}; /* mov eax,1 ; hlt: translates one block */
  memcpy(GuardCode, Warm, sizeof(Warm));
  memcpy(GuardCode + 0x100, Program, Size);
  fexmac_context* Context = CreateContext(NULL, NULL);
  GuardThread = CreateThreadAt(Context, (uint64_t)(uintptr_t)GuardCode, 0);
  Run(GuardThread);
  fexmac_x86_state State = GetState(GuardThread);
  State.rip = (uint64_t)(uintptr_t)GuardCode + 0x100;
  State.gpr[RDI] = GuardAddress(Which);
  Check(State.gpr[RDI] != 64, "found the guard page");
  SetState(GuardThread, &State);
  Shared[0] = 1;
  const fexmac_stop Stop = Run(GuardThread);
  const fexmac_x86_state After = GetState(GuardThread);
  /* Delivered as the guest's page fault at the guard address: no reset of the call/return
   * stack pointer and no retry loop. */
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 14 && Stop.fault_address == State.gpr[RDI] &&
          After.rip == State.rip,
        "the guest's access is its own page fault at the guard address (reason %d, address 0x%" PRIx64 ")", Stop.reason,
        Stop.fault_address);
  Shared[0] = 2;
}

static const uint8_t GuardLoad[] = {0x8B, 0x07, 0xF4};  /* mov eax,[rdi] ; hlt */
static const uint8_t GuardStore[] = {0x89, 0x07, 0xF4}; /* mov [rdi],eax ; hlt */

static void TestGuestLoadCallRetLowGuard(void) {
  GuestTouchesGuard(0, GuardLoad, sizeof(GuardLoad));
}
static void TestGuestStoreCallRetHighGuard(void) {
  GuestTouchesGuard(1, GuardStore, sizeof(GuardStore));
}
/* The scratch-buffer guard is armed only while a block is being emitted; after a run
 * it is disarmed, so no later access to that page (by the guest or by a callback) can be
 * taken for the translator's overflow. */
static void TestScratchGuardDisarmed(void) {
  fexmac_install_fault_handlers();
  GuardCode = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xB8, 0x01, 0, 0, 0, 0xF4};
  memcpy(GuardCode, Program, sizeof(Program));
  fexmac_context* Context = CreateContext(NULL, NULL);
  GuardThread = CreateThreadAt(Context, (uint64_t)(uintptr_t)GuardCode, 0);
  const fexmac_stop Stop = Run(GuardThread);
  uint64_t Low = 0, High = 0, Scratch = 1;
  fexmac_testing_guard_pages(GuardThread, &Low, &High, &Scratch);
  Check(Stop.reason == FEXMAC_STOP_HALT && Low != 0, "a block was translated and ran");
  Check(Scratch == 0, "afterwards the scratch-buffer guard is disarmed");
  fexmac_thread_destroy(GuardThread);
  fexmac_context_destroy(Context);
}

/* The embedder's system call callback touches a guard page (a host bug): the fault is
 * passed on to the host's handler, not claimed. */
static int CallbackGuard;

static void RecordingHostHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Context;
  Shared[0] = (uint64_t)(uintptr_t)Info->si_addr == GuardAddress(CallbackGuard) ? 2 : 3;
  _exit(0);
}

static fexmac_syscall_action TouchingSyscall(fexmac_thread* Thread, void* UserData) {
  (void)Thread;
  (void)UserData;
  volatile uint8_t* Address = (volatile uint8_t*)(uintptr_t)GuardAddress(CallbackGuard);
  if (CallbackGuard == 2) {
    *Address = 1; /* a write, like the translator's own */
  } else {
    (void)*Address;
  }
  return FEXMAC_SYSCALL_STOP;
}

static void CallbackTouchesGuard(int Which) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = RecordingHostHandler;
  Action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGBUS, &Action, NULL);
  sigaction(SIGSEGV, &Action, NULL);
  fexmac_install_fault_handlers();
  CallbackGuard = Which;
  GuardCode = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xB8, 0x27, 0, 0, 0, 0x0F, 0x05, 0xF4}; /* mov eax,39 ; syscall ; hlt */
  memcpy(GuardCode, Program, sizeof(Program));
  fexmac_context* Context = CreateContext(TouchingSyscall, NULL);
  GuardThread = CreateThreadAt(Context, (uint64_t)(uintptr_t)GuardCode, 0);
  Shared[0] = 1;
  fexmac_thread_run(GuardThread, NULL);
  printf("  FAIL: the callback's access to the guard page did not fault\n");
  exit(1);
}

static void TestCallbackCallRetGuard(void) {
  CallbackTouchesGuard(0);
}

/* ---- a log callback that calls back into the library ---- */

static fexmac_context* LogContext;
static fexmac_result FromLog[3];
static int LogEntries;

static void ReentrantLog(fexmac_log_level Level, const char* Message, void* UserData) {
  (void)Level;
  (void)Message;
  (void)UserData;
  uint32_t Regs[4];
  fexmac_thread* Thread = NULL;
  FromLog[0] = fexmac_cpuid(LogContext, 0, 0, Regs);
  FromLog[1] = fexmac_thread_create(LogContext, &Thread);
  FromLog[2] = fexmac_context_destroy(LogContext);
  ++LogEntries;
}

static void TestLogCallbackReentry(void) {
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.log = ReentrantLog;
  if (fexmac_context_create(&Config, &LogContext) != FEXMAC_OK) {
    exit(2);
  }
  fexmac_testing_log("a message");
  Check(LogEntries == 1, "the log callback ran");
  CheckResult(FromLog[0], FEXMAC_ERROR_BUSY, "fexmac_cpuid from the log callback");
  CheckResult(FromLog[1], FEXMAC_ERROR_BUSY, "fexmac_thread_create from the log callback");
  CheckResult(FromLog[2], FEXMAC_ERROR_BUSY, "fexmac_context_destroy from the log callback");
  uint32_t Regs[4];
  CheckResult(fexmac_cpuid(LogContext, 0, 0, Regs), FEXMAC_OK, "fexmac_cpuid afterwards");
  CheckResult(fexmac_context_destroy(LogContext), FEXMAC_OK, "fexmac_context_destroy afterwards");
}

/* ---- a fault inside the split-lock emulation ends the process, in both arrangements ---- */

/* The page is made inaccessible after the library checked it and before the core
 * emulates the misaligned lock xadd (the race with a raw mprotect that the contract
 * excludes): the nested fault, taken with the split-lock lock held, must not reach a
 * host handler (which might leave it with siglongjmp) and must not hang; the process ends. */
static void HostSawNested(int Signal, siginfo_t* Info, void* Context) {
  if (fexmac_handle_fault(Signal, Info, Context)) {
    return;
  }
  Shared[0] = 99; /* the host was given the fault */
  _exit(3);
}

static void RunSplitEmulationFault(int Hosted) {
  if (Hosted) {
    struct sigaction Action;
    memset(&Action, 0, sizeof(Action));
    Action.sa_sigaction = HostSawNested;
    Action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &Action, NULL);
    sigaction(SIGSEGV, &Action, NULL);
  } else {
    fexmac_install_fault_handlers();
  }
  static const uint8_t LockXadd[] = {0xF0, 0x0F, 0xC1, 0x07, 0xF4}; /* lock xadd [rdi], eax ; hlt */
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* Data = MapGuestMemory(0x4000);
  memcpy(Code, LockXadd, sizeof(LockXadd));
  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = (uint64_t)(uintptr_t)Data + 64 + 14;
  SetState(Thread, &State);
  fexmac_testing_protect_before_emulation(Data);
  Shared[0] = 1;
  fexmac_thread_run(Thread, NULL);
  Shared[0] = 98; /* survived */
  exit(1);
}

static void TestSplitEmulationFaultInstalled(void) {
  RunSplitEmulationFault(0);
}
static void TestSplitEmulationFaultHosted(void) {
  RunSplitEmulationFault(1);
}

/* ---- the scratch buffer: overflows, restarts and the limit ---- */

/* Translations that overflow the scratch buffer once, twice and many times, at every
 * offset in the guard page. Each overflow restarts the translation from the restart
 * point with a buffer twice as generous; the block must then run correctly, the thread
 * must be back in executable state (or the translated code would fault), and the guard
 * must be disarmed. Starting from 16 bytes per IR node, a long run of rep movsq
 * overflows once; starting from 1 byte per IR node, blocks overflow up to 6 times. */
static uint64_t OverflowsForBlock(size_t Count, size_t Every, uint32_t Start, uint8_t* Data, int* Wrong) {
  fexmac_context* Context = CreateContextWithOptions(BigBlockOptions, 2, NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, 0, (uint64_t)(uintptr_t)Data + 0x8000);
  fexmac_testing_set_scratch_start(Thread, Start);
  uint8_t* Code = MapGuestMemory(0x4000);
  uint64_t Expected = 0;
  const size_t Length = WriteBigBlock(Code, Count, Every, &Expected);
  if (!RunBigBlock(Thread, Code, Length, Expected, Data)) {
    ++*Wrong;
  }
  const uint64_t Overflows = fexmac_testing_scratch_overflows(Thread);
  uint64_t Low = 0, High = 0, Scratch = 1;
  fexmac_testing_guard_pages(Thread, &Low, &High, &Scratch);
  if (Scratch != 0) {
    ++*Wrong;
  }
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
  munmap(Code, 0x4000);
  return Overflows;
}

static void TestScratchOverflowMany(void) {
  fexmac_install_fault_handlers();
  uint8_t* Data = MapGuestMemory(0x10000);
  int Wrong = 0;

  /* rep movsq translates to about 19 bytes of host code per IR node (the in-place loop
   * of the macOS port; the core's general loop was about 26), under the core's first
   * allowance of 24, so blocks of it no longer overflow at the default. With a first
   * allowance of 16 bytes per node, 96 fit and 416 overflow; every size between, one
   * instruction apart, runs correctly, so the guard is hit at offsets across its page. */
  const uint64_t Smallest = OverflowsForBlock(96, 32, 16, Data, &Wrong);
  const uint64_t Largest = OverflowsForBlock(416, 32, 16, Data, &Wrong);
  Check(Wrong == 0 && Smallest == 0 && Largest >= 1,
        "with 16 bytes per IR node first, 96 rep movsq fit (%" PRIu64 " overflows) and 416 overflow (%" PRIu64 ")", Smallest, Largest);
  int Swept = 0, SweptOverflowing = 0;
  for (size_t Count = 96; Count <= 416; ++Count, ++Swept) {
    SweptOverflowing += OverflowsForBlock(Count, 32, 16, Data, &Wrong) > 0;
  }
  Check(Wrong == 0 && SweptOverflowing > 100, "%d blocks of 96 to 416 rep movsq at 16 bytes per node ran correctly (%d overflowed)", Swept,
        SweptOverflowing);

  /* A thread keeps its scratch buffer between translations and only claims a larger
   * one when a translation needs it, so each translation gets a fresh context, and with
   * it a scratch buffer of exactly the requested size. */
  int Seen[8] = {0};
  uint64_t Most = 0;
  const uint32_t Starts[] = {1, 6, 12};
  for (size_t S = 0; S < sizeof(Starts) / sizeof(Starts[0]); ++S) {
    for (size_t Count = 1; Count <= 600; Count += (Count < 200 ? 3 : 20)) {
      const uint64_t Overflows = OverflowsForBlock(Count, 4, Starts[S], Data, &Wrong);
      Seen[Overflows < 7 ? Overflows : 7] = 1;
      Most = Overflows > Most ? Overflows : Most;
    }
  }
  printf("  info: overflows per translation seen:");
  for (int i = 0; i < 8; ++i) {
    if (Seen[i]) {
      printf(" %d%s", i, i == 7 ? "+" : "");
    }
  }
  printf("\n");
  Check(Wrong == 0, "blocks of 1 to 600 rep movsq, starting at 1, 6 and 12 bytes per IR node, ran correctly, guard disarmed after each");
  Check(Seen[0] && Seen[1] && Seen[2] && Most >= 4, "they overflowed 0, 1, 2 and up to %" PRIu64 " times", Most);
}

/* A block that never fits: the translator stops at its limit instead of growing the
 * buffer for ever, and the run stops cleanly as untranslatable at the block's start.
 * The thread is left usable: with the limit lifted the same block translates and runs. */
static void TestScratchLimitStops(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = CreateContextWithOptions(BigBlockOptions, 2, NULL, NULL);
  uint8_t* Data = MapGuestMemory(0x10000);
  fexmac_thread* Thread = CreateThreadAt(Context, 0, (uint64_t)(uintptr_t)Data + 0x8000);
  uint8_t* Code = MapGuestMemory(0x4000);
  uint64_t Expected = 0;
  const size_t Length = WriteBigBlock(Code, 1024, 8, &Expected);
  fexmac_testing_set_scratch_start(Thread, 1);
  fexmac_testing_set_scratch_limit(Thread, 64 * 1024);
  fexmac_x86_state State = GetState(Thread);
  State.rip = (uint64_t)(uintptr_t)Code;
  SetState(Thread, &State);
  const fexmac_stop Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_UNSUPPORTED_INSTRUCTION && Stop.instruction_address == (uint64_t)(uintptr_t)Code,
        "a block that does not fit within the limit stops as untranslatable at its start (reason %d)", Stop.reason);
  Check(fexmac_testing_scratch_overflows(Thread) >= 2, "after overflowing %" PRIu64 " times (at least twice)", fexmac_testing_scratch_overflows(Thread));
  uint64_t Low = 0, High = 0, Scratch = 1;
  fexmac_testing_guard_pages(Thread, &Low, &High, &Scratch);
  Check(Scratch == 0, "the scratch-buffer guard is disarmed after giving up");
  fexmac_testing_set_scratch_limit(Thread, 0);
  fexmac_testing_set_scratch_start(Thread, 0);
  Check(RunBigBlock(Thread, Code, Length, Expected, Data), "with the limit lifted the same block translates and runs");
  Shared[0] = 1;
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- the code buffer's guard page ---- */

/* Protection of the page at Address, or -1 if nothing is mapped there. */
static int PageProtection(uint64_t Address) {
  mach_vm_address_t Region = Address;
  mach_vm_size_t Size = 0;
  vm_region_basic_info_data_64_t Info;
  mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t Object;
  if (mach_vm_region(mach_task_self(), &Region, &Size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&Info, &Count, &Object) != KERN_SUCCESS ||
      Region > Address) {
    return -1;
  }
  return Info.protection;
}

/* Every code buffer, the first and the larger ones that replace it when a block does
 * not fit in what is left, ends in an inaccessible host page, and the code before it is
 * executable. */
static void TestCodeBufferGuardPages(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = CreateContextWithOptions(BigBlockOptions, 2, NULL, NULL);
  uint8_t* Data = MapGuestMemory(0x10000);
  fexmac_thread* Thread = CreateThreadAt(Context, 0, (uint64_t)(uintptr_t)Data + 0x8000);
  uint8_t* Code = MapGuestMemory(0x4000);
  uint64_t Expected = 0;
  const size_t Length = WriteBigBlock(Code, 1024, 8, &Expected);
  uint64_t Guard = 0;
  int Buffers = 0, Wrong = 0, Copies = 0, Unguarded = 0;
  for (; Copies < 400 && Buffers < 3; ++Copies) {
    if (!RunBigBlock(Thread, Code, Length, Expected, Data)) {
      ++Wrong;
    }
    const uint64_t Now = fexmac_testing_code_buffer_guard(Thread);
    if (Now != Guard) {
      /* A new buffer: the one before it has been released. */
      Guard = Now;
      ++Buffers;
      const int Protection = PageProtection(Guard);
      const int Before = PageProtection(Guard - 4);
      printf("  info: code buffer %d after %d copies: guard page at 0x%" PRIx64 " protection %d, the page before it %d\n", Buffers, Copies, Guard,
             Protection, Before);
      if (Protection != VM_PROT_NONE || !(Before & VM_PROT_EXECUTE)) {
        ++Unguarded;
      }
    }
    fexmac_invalidate(Context, (uint64_t)(uintptr_t)Code, Length);
  }
  Check(Wrong == 0 && Buffers == 3, "%d copies of a large block ran correctly and filled %d code buffers", Copies, Buffers);
  Check(Unguarded == 0, "each code buffer ends in an inaccessible page, with executable code before it");
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* A write running past the end of the code buffer, with the thread switched to writable
 * as the translator is when it copies code in, faults on the guard page instead of
 * landing in whatever is mapped next, and the process dies with a message naming it.
 * Done from the system call callback, on the thread's own host thread while it runs. */
static fexmac_syscall_action OverrunningSyscall(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  Shared[0] = 1;
  fexmac_testing_overrun_code_buffer(Thread);
  Shared[0] = 3;
  return FEXMAC_SYSCALL_STOP;
}

static void OverrunCodeBuffer(void) {
  fexmac_install_fault_handlers();
  GuardCode = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xB8, 0x27, 0, 0, 0, 0x0F, 0x05, 0xF4}; /* mov eax,39 ; syscall ; hlt */
  memcpy(GuardCode, Program, sizeof(Program));
  fexmac_context* Context = CreateContext(OverrunningSyscall, NULL);
  GuardThread = CreateThreadAt(Context, (uint64_t)(uintptr_t)GuardCode, 0);
  fexmac_thread_run(GuardThread, NULL);
  printf("  FAIL: survived a write past the end of the code buffer\n");
  exit(1);
}

static void TestCodeBufferOverrunDies(void) {
  OverrunCodeBuffer();
}

/* With a handler of the host's installed before the library's, the message is still
 * written, and the fault (at the guard page) is passed on to that handler. */
static void OverrunHostHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Context;
  Shared[0] = (uint64_t)(uintptr_t)Info->si_addr == fexmac_testing_code_buffer_guard(GuardThread) ? 2 : 4;
  _exit(0);
}

static void TestCodeBufferOverrunChained(void) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = OverrunHostHandler;
  Action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigaction(SIGBUS, &Action, NULL);
  sigaction(SIGSEGV, &Action, NULL);
  OverrunCodeBuffer();
}

#define GUARD_TEXT "guest memory access fault in translated code at address 0x"
#define OVERRUN_TEXT "fexmac: SIGBUS: write past the end of a code buffer, at address 0x"

static const struct Subtest Subtests[] = {
  {"guest-load-callret-low-guard", TestGuestLoadCallRetLowGuard, 0, NULL, "fexmac:", 2},
  {"guest-store-callret-high-guard", TestGuestStoreCallRetHighGuard, 0, NULL, "fexmac:", 2},
  {"scratch-guard-disarmed-after-translation", TestScratchGuardDisarmed, 0, NULL, NULL, 0},
  {"callback-callret-guard-passed-on", TestCallbackCallRetGuard, 0, NULL, "fexmac:", 2},
  {"log-callback-reentry", TestLogCallbackReentry, 0, NULL, NULL, 0},
  {"fault-inside-handler-dies", TestFaultInsideHandlerDies, SIGBUS, "fexmac: fault inside the fault handler", NULL, 1},
  {"no-log-callback-in-handler", TestNoLogCallbackInHandler, 0, "core message during fault handling: test message from the fault handler",
   NULL, 7},
  {"log-restored-after-failed-create", TestLogRestoredAfterFailedCreate, 0, NULL, NULL, 0},
  {"invalidation-mask", TestInvalidationMask, 0, NULL, NULL, 0},
  {"split-emulation-fault-dies", TestSplitEmulationFaultInstalled, SIGBUS, "misaligned atomic", NULL, 1},
  {"split-emulation-fault-hosted-dies", TestSplitEmulationFaultHosted, SIGBUS, "fault while emulating a misaligned atomic", NULL, 1},
  {"scratch-overflow-many", TestScratchOverflowMany, 0, NULL, NULL, 0},
  {"scratch-limit-stops", TestScratchLimitStops, 0, "still overflows its translation buffer", NULL, 1},
  {"code-buffer-guard-pages", TestCodeBufferGuardPages, 0, NULL, NULL, 0},
  {"code-buffer-overrun-dies", TestCodeBufferOverrunDies, SIGBUS, OVERRUN_TEXT, "fault inside", 1},
  {"code-buffer-overrun-chained", TestCodeBufferOverrunChained, 0, OVERRUN_TEXT, "fault inside", 2},
};

int main(int argc, char** argv) {
  return RunSubtests(argc, argv, Subtests, sizeof(Subtests) / sizeof(Subtests[0]), 60);
}
