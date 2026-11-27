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
};

int main(int argc, char** argv) {
  return RunSubtests(argc, argv, Subtests, sizeof(Subtests) / sizeof(Subtests[0]), 60);
}
