/* SPDX-License-Identifier: MIT */
/*
 * Tests of the fexmac C interface, written in C against the public header only (plus
 * the library's test hook for the fault-inside-handler case), and linked the way an
 * outside project links the library.
 *
 * Each subtest runs in its own child process, because contexts and signal handlers are
 * process-wide and two subtests check what happens when the process dies. A child that
 * hangs is killed by an alarm and reported as hung.
 *
 * Usage: fexmac_c_test            run every subtest
 *        fexmac_c_test <name>     run one subtest in this process
 */
#include "fexmac.h"
#include "fexmac_testing.h"

#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int Failures;

static void Check(int Condition, const char* Format, ...) {
  va_list Args;
  va_start(Args, Format);
  printf("  %s: ", Condition ? "ok  " : "FAIL");
  vprintf(Format, Args);
  printf("\n");
  va_end(Args);
  fflush(stdout);
  if (!Condition) {
    ++Failures;
  }
}

static void CheckResult(fexmac_result Got, fexmac_result Expected, const char* What) {
  Check(Got == Expected, "%s returns %s (got %s)", What, fexmac_result_string(Expected), fexmac_result_string(Got));
}

static uint8_t* MapGuestMemory(size_t Size) {
  void* Memory = mmap(NULL, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Memory == MAP_FAILED) {
    fprintf(stderr, "could not map guest memory\n");
    exit(2);
  }
  return (uint8_t*)Memory;
}

static fexmac_context* CreateContext(fexmac_syscall_fn Syscall, void* UserData) {
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.syscall = Syscall;
  Config.syscall_user_data = UserData;
  fexmac_context* Context = NULL;
  const fexmac_result Result = fexmac_context_create(&Config, &Context);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "fexmac_context_create: %s\n", fexmac_result_string(Result));
    exit(2);
  }
  return Context;
}

static fexmac_thread* CreateThreadAt(fexmac_context* Context, uint64_t RIP, uint64_t RSP) {
  fexmac_thread* Thread = NULL;
  if (fexmac_thread_create(Context, &Thread) != FEXMAC_OK) {
    fprintf(stderr, "fexmac_thread_create failed\n");
    exit(2);
  }
  fexmac_x86_state State;
  fexmac_thread_get_state(Thread, &State);
  State.rip = RIP;
  State.gpr[4] = RSP;
  fexmac_thread_set_state(Thread, &State);
  return Thread;
}

enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RSI = 6, RDI = 7 };

/* ---- run: system calls handled in the callback and stopping, then a halt ---- */

/* clang-format off */
static const uint8_t SumProgram[] = {
  0x31, 0xC0,                   /*  0: xor eax, eax */
  0xB9, 0x64, 0x00, 0x00, 0x00, /*  2: mov ecx, 100 */
  0x01, 0xC8,                   /*  7: add eax, ecx */
  0xFF, 0xC9,                   /*  9: dec ecx */
  0x75, 0xFA,                   /* 11: jnz 7 */
  0x89, 0xC7,                   /* 13: mov edi, eax */
  0xB8, 0x27, 0x00, 0x00, 0x00, /* 15: mov eax, 39 */
  0x0F, 0x05,                   /* 20: syscall (resumed by the callback) */
  0x89, 0xC3,                   /* 22: mov ebx, eax */
  0xB8, 0x3C, 0x00, 0x00, 0x00, /* 24: mov eax, 60 */
  0x0F, 0x05,                   /* 29: syscall (stops the run) */
  0xBA, 0x77, 0x00, 0x00, 0x00, /* 31: mov edx, 0x77 */
  0xF4,                         /* 36: hlt */
};
/* clang-format on */

struct SyscallRecord {
  uint64_t CodeBase;
  int Calls;
  int Problems;
  fexmac_result RunFromCallback;
  fexmac_result DestroyFromCallback;
  fexmac_result CreateFromCallback;
};

static fexmac_syscall_action RunSyscall(fexmac_thread* Thread, void* UserData) {
  struct SyscallRecord* Record = (struct SyscallRecord*)UserData;
  fexmac_x86_state State;
  if (fexmac_thread_get_state(Thread, &State) != FEXMAC_OK) {
    ++Record->Problems;
    return FEXMAC_SYSCALL_STOP;
  }
  ++Record->Calls;
  if (State.gpr[RAX] == 39) {
    if (State.gpr[RDI] != 5050 || State.gpr[RCX] != Record->CodeBase + 22) {
      ++Record->Problems;
    }
    /* Calls the callback must not make. */
    Record->RunFromCallback = fexmac_thread_run(Thread, NULL);
    Record->DestroyFromCallback = fexmac_thread_destroy(Thread);
    fexmac_thread* Other = NULL;
    Record->CreateFromCallback = fexmac_thread_create(fexmac_thread_context(Thread), &Other);

    State.gpr[RAX] = 1234;
    if (fexmac_thread_set_state(Thread, &State) != FEXMAC_OK) {
      ++Record->Problems;
    }
    return FEXMAC_SYSCALL_RESUME;
  }
  return FEXMAC_SYSCALL_STOP;
}

static void TestRun(void) {
  struct SyscallRecord Record = {0};
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* Stack = MapGuestMemory(0x10000);
  memcpy(Code, SumProgram, sizeof(SumProgram));
  Record.CodeBase = (uint64_t)(uintptr_t)Code;

  fexmac_context* Context = CreateContext(RunSyscall, &Record);
  fexmac_thread* Thread = CreateThreadAt(Context, Record.CodeBase, (uint64_t)(uintptr_t)Stack + 0x10000 - 64);
  fexmac_thread_set_user_data(Thread, &Record);
  Check(fexmac_thread_get_user_data(Thread) == &Record, "user data round trip");
  Check(fexmac_thread_context(Thread) == Context, "thread knows its context");

  fexmac_stop Stop;
  CheckResult(fexmac_thread_run(Thread, &Stop), FEXMAC_OK, "first run");
  fexmac_x86_state State;
  fexmac_thread_get_state(Thread, &State);
  Check(Stop.reason == FEXMAC_STOP_SYSCALL, "first run stops at a system call (reason %d)", Stop.reason);
  Check(Record.Calls == 2 && Record.Problems == 0, "callback saw both system calls with the right registers (%d calls, %d problems)",
        Record.Calls, Record.Problems);
  Check(State.gpr[RBX] == 1234, "value the callback returned in rax reached the guest (rbx = %" PRIu64 ")", State.gpr[RBX]);
  Check(State.gpr[RDI] == 5050, "guest summed 1 to 100 (rdi = %" PRIu64 ")", State.gpr[RDI]);
  Check(State.rip == Record.CodeBase + 31, "rip is after the stopping system call");
  CheckResult(Record.RunFromCallback, FEXMAC_ERROR_BUSY, "running the thread from its own callback");
  CheckResult(Record.DestroyFromCallback, FEXMAC_ERROR_BUSY, "destroying the thread from its own callback");
  CheckResult(Record.CreateFromCallback, FEXMAC_ERROR_BUSY, "creating a thread from a callback");

  CheckResult(fexmac_thread_run(Thread, &Stop), FEXMAC_OK, "second run");
  fexmac_thread_get_state(Thread, &State);
  Check(Stop.reason == FEXMAC_STOP_HALT, "second run stops at hlt (reason %d)", Stop.reason);
  Check(State.gpr[RDX] == 0x77 && State.rip == Record.CodeBase + 36, "guest continued after the stop and rip is at hlt");

  CheckResult(fexmac_thread_destroy(Thread), FEXMAC_OK, "thread destroy");
  CheckResult(fexmac_context_destroy(Context), FEXMAC_OK, "context destroy");
}

/* ---- state: set registers, guest reads them; get/set round trip; guest faults ---- */

/* clang-format off */
static const uint8_t StateProgram[] = {
  0x66, 0x48, 0x0F, 0x7E, 0xD8, /* 0: movq rax, xmm3 */
  0x0F, 0x92, 0xC1,             /* 5: setc cl */
  0x48, 0x89, 0xF2,             /* 8: mov rdx, rsi */
  0xF4,                         /* 11: hlt */
  0x0F, 0x0B,                   /* 12: ud2 */
  0xCC,                         /* 14: int3 */
};
/* clang-format on */

static void TestState(void) {
  uint8_t* Code = MapGuestMemory(0x10000);
  memcpy(Code, StateProgram, sizeof(StateProgram));
  const uint64_t Base = (uint64_t)(uintptr_t)Code;

  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);

  fexmac_x86_state First, Second;
  fexmac_thread_get_state(Thread, &First);
  Check(First.rflags == 0x202, "initial rflags is 0x202 (got 0x%" PRIx64 ")", First.rflags);
  Check(First.mxcsr == 0x1F80 && First.fcw == 0x37F, "initial mxcsr and fcw are the x86 defaults");
  fexmac_thread_set_state(Thread, &First);
  fexmac_thread_get_state(Thread, &Second);
  Check(memcmp(&First, &Second, sizeof(First)) == 0, "get, set, get gives identical state");

  First.xmm[3][0] = 0x1122334455667788ULL;
  First.gpr[RSI] = 0xFEEDFACECAFEBEEFULL;
  First.rflags = 0x202 | 1; /* CF */
  First.fs_base = 0x123450000ULL;
  fexmac_thread_set_state(Thread, &First);
  fexmac_stop Stop;
  fexmac_thread_run(Thread, &Stop);
  fexmac_thread_get_state(Thread, &Second);
  Check(Stop.reason == FEXMAC_STOP_HALT, "program halts");
  Check(Second.gpr[RAX] == 0x1122334455667788ULL, "guest read xmm3 that was set");
  Check((Second.gpr[RCX] & 0xFF) == 1, "guest read the carry flag that was set");
  Check(Second.gpr[RDX] == 0xFEEDFACECAFEBEEFULL, "guest read rsi that was set");
  Check(Second.fs_base == 0x123450000ULL, "fs base kept");

  Second.rip = Base + 12;
  fexmac_thread_set_state(Thread, &Second);
  fexmac_thread_run(Thread, &Stop);
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 6 && Stop.signal == SIGILL,
        "ud2 stops with a guest fault, #UD, SIGILL (reason %d, trap %u, signal %d)", Stop.reason, Stop.trap_number, Stop.signal);

  Second.rip = Base + 14;
  fexmac_thread_set_state(Thread, &Second);
  fexmac_thread_run(Thread, &Stop);
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 3 && Stop.signal == SIGTRAP,
        "int3 stops with a guest fault, #BP, SIGTRAP (reason %d, trap %u, signal %d)", Stop.reason, Stop.trap_number, Stop.signal);

  uint32_t Regs[4] = {0};
  CheckResult(fexmac_cpuid(Context, 0, 0, Regs), FEXMAC_OK, "cpuid");
  Check(Regs[0] >= 1, "cpuid reports a maximum leaf (%u)", Regs[0]);

  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- invalidate: changed guest code is retranslated after fexmac_invalidate ---- */

static void TestInvalidate(void) {
  /* mov eax, imm32 ; hlt */
  uint8_t* Code = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF4};
  memcpy(Code, Program, sizeof(Program));
  const uint64_t Base = (uint64_t)(uintptr_t)Code;

  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);
  fexmac_stop Stop;
  fexmac_x86_state State;

  fexmac_thread_run(Thread, &Stop);
  fexmac_thread_get_state(Thread, &State);
  Check(State.gpr[RAX] == 1, "first version of the code returns 1");

  Code[1] = 2;
  State.rip = Base;
  fexmac_thread_set_state(Thread, &State);
  fexmac_thread_run(Thread, &Stop);
  fexmac_thread_get_state(Thread, &State);
  printf("  info: without invalidation the changed code returns %" PRIu64 " (1 means the stale translation ran)\n", State.gpr[RAX]);

  Code[1] = 3;
  CheckResult(fexmac_invalidate(Context, Base, sizeof(Program)), FEXMAC_OK, "invalidate");
  State.rip = Base;
  fexmac_thread_set_state(Thread, &State);
  fexmac_thread_run(Thread, &Stop);
  fexmac_thread_get_state(Thread, &State);
  Check(State.gpr[RAX] == 3, "after invalidation the changed code runs (rax = %" PRIu64 ")", State.gpr[RAX]);

  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- faults: misaligned atomics, in each signal-handler arrangement ---- */

static volatile sig_atomic_t HostFaults;
static uint8_t* HostPage;
static long PageSize;

/* A host handler for faults of its own: makes the page accessible and retries. */
static void HostOwnHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Context;
  if ((uint8_t*)Info->si_addr >= HostPage && (uint8_t*)Info->si_addr < HostPage + PageSize) {
    ++HostFaults;
    mprotect(HostPage, (size_t)PageSize, PROT_READ | PROT_WRITE);
    return;
  }
  signal(Signal, SIG_DFL);
}

/* A host that owns the signals and asks the library first. */
static void HostForwardingHandler(int Signal, siginfo_t* Info, void* Context) {
  if (fexmac_handle_fault(Signal, Info, Context)) {
    return;
  }
  HostOwnHandler(Signal, Info, Context);
}

static void InstallHostHandler(void (*Handler)(int, siginfo_t*, void*)) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = Handler;
  Action.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&Action.sa_mask);
  sigaction(SIGBUS, &Action, NULL);
  sigaction(SIGSEGV, &Action, NULL);
}

static void RunMisalignedAtomic(void) {
  /* lock xadd [rdi], eax ; hlt, on a 4-byte value that crosses a 16-byte boundary. */
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* Data = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xF0, 0x0F, 0xC1, 0x07, 0xF4};
  memcpy(Code, Program, sizeof(Program));
  uint8_t* Value = Data + 64 + 14;
  const uint32_t Initial = 5;
  memcpy(Value, &Initial, sizeof(Initial));

  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, 0);
  fexmac_x86_state State;
  fexmac_thread_get_state(Thread, &State);
  State.gpr[RDI] = (uint64_t)(uintptr_t)Value;
  State.gpr[RAX] = 3;
  fexmac_thread_set_state(Thread, &State);

  fexmac_stop Stop;
  fexmac_thread_run(Thread, &Stop);
  fexmac_thread_get_state(Thread, &State);
  uint32_t Result;
  memcpy(&Result, Value, sizeof(Result));
  Check(Stop.reason == FEXMAC_STOP_HALT, "misaligned atomic runs to the halt");
  Check(Result == 8 && State.gpr[RAX] == 5, "misaligned lock xadd gives the x86 result (memory %u, eax %" PRIu64 ")", Result,
        State.gpr[RAX]);
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

static void TouchHostPage(void) {
  HostPage = MapGuestMemory((size_t)PageSize);
  mprotect(HostPage, (size_t)PageSize, PROT_NONE);
  HostFaults = 0;
  ((volatile uint8_t*)HostPage)[16] = 42;
  Check(HostFaults == 1 && HostPage[16] == 42, "host's own fault reached the host handler (%d faults)", (int)HostFaults);
}

static void TestFaultsInstalled(void) {
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers");
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers again");
  RunMisalignedAtomic();
}

/* The host installed handlers first; the library chains to them. */
static void TestFaultsChained(void) {
  InstallHostHandler(HostOwnHandler);
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers over the host's");
  RunMisalignedAtomic();
  TouchHostPage();
}

/* The host owns the handlers and forwards to fexmac_handle_fault. */
static void TestFaultsHosted(void) {
  InstallHostHandler(HostForwardingHandler);
  RunMisalignedAtomic();
  TouchHostPage();
}

/* Without any handler, a fault that is not the library's kills the process normally:
 * run in a child that is expected to die. */
static void TestUnclaimedFaultDies(void) {
  fexmac_install_fault_handlers();
  uint8_t* Page = MapGuestMemory((size_t)PageSize);
  mprotect(Page, (size_t)PageSize, PROT_NONE);
  ((volatile uint8_t*)Page)[0] = 1;
  printf("  FAIL: survived a fault nobody handles\n");
  exit(1);
}

/* A bug inside the library's handler must crash, not hang. */
static void TestFaultInsideHandlerDies(void) {
  fexmac_install_fault_handlers();
  fexmac_testing_fault_inside_handler(1);
  uint8_t* Page = MapGuestMemory((size_t)PageSize);
  mprotect(Page, (size_t)PageSize, PROT_NONE);
  ((volatile uint8_t*)Page)[0] = 1;
  printf("  FAIL: survived a fault inside the fault handler\n");
  exit(1);
}

/* ---- errors: bad arguments, bad options, lifetime misuse ---- */

static fexmac_result CreateWithOption(const char* Name, const char* Value, fexmac_context** Out) {
  fexmac_option Option = {Name, Value};
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = &Option;
  Config.option_count = 1;
  return fexmac_context_create(&Config, Out);
}

static void QuietLog(fexmac_log_level Level, const char* Message, void* UserData) {
  (void)Level;
  (void)UserData;
  printf("  log: %s\n", Message);
}

static void TestErrors(void) {
  fexmac_context* Context = NULL;
  fexmac_config Config;
  fexmac_config_init(&Config);

  CheckResult(fexmac_context_create(NULL, &Context), FEXMAC_ERROR_INVALID_ARGUMENT, "create with no config");
  CheckResult(fexmac_context_create(&Config, NULL), FEXMAC_ERROR_INVALID_ARGUMENT, "create with no output");
  Config.struct_size = 8;
  CheckResult(fexmac_context_create(&Config, &Context), FEXMAC_ERROR_INVALID_ARGUMENT, "create with a wrong struct size");
  fexmac_config_init(&Config);
  Config.option_count = 1;
  CheckResult(fexmac_context_create(&Config, &Context), FEXMAC_ERROR_INVALID_ARGUMENT, "create with options missing");

  CheckResult(CreateWithOption("NOSUCHOPTION", "1", &Context), FEXMAC_ERROR_BAD_CONFIG, "unknown option");
  CheckResult(CreateWithOption("MAXINST", "lots", &Context), FEXMAC_ERROR_BAD_CONFIG, "number that does not parse");
  CheckResult(CreateWithOption("MAXINST", "12abc", &Context), FEXMAC_ERROR_BAD_CONFIG, "number with trailing text");
  CheckResult(CreateWithOption("MULTIBLOCK", "2", &Context), FEXMAC_ERROR_BAD_CONFIG, "boolean that is not 0 or 1");
  CheckResult(CreateWithOption("MULTIBLOCK", "true", &Context), FEXMAC_ERROR_BAD_CONFIG, "boolean spelt true");
  CheckResult(CreateWithOption("SMCCHECKS", "sometimes", &Context), FEXMAC_ERROR_BAD_CONFIG, "unknown named value");
  CheckResult(CreateWithOption("IS64BIT_MODE", "0", &Context), FEXMAC_ERROR_BAD_CONFIG, "32-bit mode");
  CheckResult(CreateWithOption(NULL, "1", &Context), FEXMAC_ERROR_BAD_CONFIG, "option without a name");
  Check(Context == NULL, "failed creates leave the output NULL");

  Check(fexmac_option_known("MULTIBLOCK") && fexmac_option_known("Multiblock"), "option names known in both spellings");
  Check(!fexmac_option_known("FEX_MULTIBLOCK") && !fexmac_option_known("nope") && !fexmac_option_known(NULL), "other names unknown");

  /* After all those failures, a good configuration still works. */
  fexmac_option Good[] = {{"SMCCHECKS", "full"}, {"MaxInst", "0x100"}, {"MULTIBLOCK", "0"}};
  fexmac_config_init(&Config);
  Config.options = Good;
  Config.option_count = 3;
  Config.log = QuietLog;
  CheckResult(fexmac_context_create(&Config, &Context), FEXMAC_OK, "create with good options");

  fexmac_context* Second = NULL;
  CheckResult(fexmac_context_create(&Config, &Second), FEXMAC_ERROR_BUSY, "second context while one exists");

  fexmac_thread* Thread = NULL;
  CheckResult(fexmac_thread_create(Context, NULL), FEXMAC_ERROR_INVALID_ARGUMENT, "thread create with no output");
  CheckResult(fexmac_thread_create(NULL, &Thread), FEXMAC_ERROR_INVALID_HANDLE, "thread create with no context");
  CheckResult(fexmac_thread_create(Context, &Thread), FEXMAC_OK, "thread create");
  CheckResult(fexmac_context_destroy(Context), FEXMAC_ERROR_BUSY, "destroy a context that has a thread");

  fexmac_x86_state State;
  CheckResult(fexmac_thread_get_state(Thread, NULL), FEXMAC_ERROR_INVALID_ARGUMENT, "get state with no output");
  CheckResult(fexmac_thread_get_state(NULL, &State), FEXMAC_ERROR_INVALID_HANDLE, "get state of no thread");
  CheckResult(fexmac_thread_run(NULL, NULL), FEXMAC_ERROR_INVALID_HANDLE, "run no thread");
  CheckResult(fexmac_thread_run((fexmac_thread*)Context, NULL), FEXMAC_ERROR_INVALID_HANDLE, "run a context as a thread");
  CheckResult(fexmac_thread_destroy((fexmac_thread*)&State), FEXMAC_ERROR_INVALID_HANDLE, "destroy a pointer that is no thread");

  CheckResult(fexmac_thread_destroy(Thread), FEXMAC_OK, "thread destroy");
  CheckResult(fexmac_thread_destroy(Thread), FEXMAC_ERROR_INVALID_HANDLE, "thread destroyed twice");
  CheckResult(fexmac_thread_get_state(Thread, &State), FEXMAC_ERROR_INVALID_HANDLE, "get state after destroy");
  CheckResult(fexmac_thread_set_state(Thread, &State), FEXMAC_ERROR_INVALID_HANDLE, "set state after destroy");
  CheckResult(fexmac_thread_run(Thread, NULL), FEXMAC_ERROR_INVALID_HANDLE, "run after destroy");
  Check(fexmac_thread_context(Thread) == NULL && fexmac_thread_get_user_data(Thread) == NULL, "accessors after destroy return NULL");

  CheckResult(fexmac_context_destroy(Context), FEXMAC_OK, "context destroy");
  CheckResult(fexmac_context_destroy(Context), FEXMAC_ERROR_INVALID_HANDLE, "context destroyed twice");
  CheckResult(fexmac_context_destroy(NULL), FEXMAC_ERROR_INVALID_HANDLE, "destroy no context");
  CheckResult(fexmac_thread_create(Context, &Thread), FEXMAC_ERROR_INVALID_HANDLE, "thread create on a destroyed context");
  uint32_t Regs[4];
  CheckResult(fexmac_cpuid(Context, 0, 0, Regs), FEXMAC_ERROR_INVALID_HANDLE, "cpuid on a destroyed context");
  CheckResult(fexmac_invalidate(Context, 0, 1), FEXMAC_ERROR_INVALID_HANDLE, "invalidate on a destroyed context");

  /* A new context after the old one is gone, and it runs code. */
  uint8_t* Code = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xF4}; /* mov eax, 42 ; hlt */
  memcpy(Code, Program, sizeof(Program));
  Context = CreateContext(NULL, NULL);
  Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, 0);
  fexmac_stop Stop;
  fexmac_thread_run(Thread, &Stop);
  fexmac_thread_get_state(Thread, &State);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 42, "a second context, created after the first, runs code");
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);

  Check(fexmac_version() >> 16 == FEXMAC_VERSION_MAJOR, "library version matches the header");
}

/* ---- driver ---- */

struct Subtest {
  const char* Name;
  void (*Run)(void);
  int ExpectSignal; /* 0: must exit 0. Otherwise must die by SIGSEGV or SIGBUS. */
};

static const struct Subtest Subtests[] = {
  {"run", TestRun, 0},
  {"state", TestState, 0},
  {"invalidate", TestInvalidate, 0},
  {"faults-installed", TestFaultsInstalled, 0},
  {"faults-chained", TestFaultsChained, 0},
  {"faults-hosted", TestFaultsHosted, 0},
  {"errors", TestErrors, 0},
  {"unclaimed-fault-dies", TestUnclaimedFaultDies, 1},
  {"fault-inside-handler-dies", TestFaultInsideHandlerDies, 1},
};

int main(int argc, char** argv) {
  PageSize = sysconf(_SC_PAGESIZE);
  const size_t Count = sizeof(Subtests) / sizeof(Subtests[0]);

  if (argc > 1) {
    for (size_t i = 0; i < Count; ++i) {
      if (strcmp(argv[1], Subtests[i].Name) == 0) {
        Subtests[i].Run();
        return Failures ? 1 : 0;
      }
    }
    fprintf(stderr, "unknown subtest %s\n", argv[1]);
    return 2;
  }

  int Failed = 0;
  for (size_t i = 0; i < Count; ++i) {
    printf("%s\n", Subtests[i].Name);
    fflush(stdout);
    const pid_t Child = fork();
    if (Child == 0) {
      alarm(60);
      Subtests[i].Run();
      fflush(stdout);
      _exit(Failures ? 1 : 0);
    }
    int Status = 0;
    waitpid(Child, &Status, 0);
    int Passed;
    if (WIFSIGNALED(Status) && WTERMSIG(Status) == SIGALRM) {
      printf("  FAIL: hung, killed after 60 seconds\n");
      Passed = 0;
    } else if (Subtests[i].ExpectSignal) {
      Passed = WIFSIGNALED(Status) && (WTERMSIG(Status) == SIGSEGV || WTERMSIG(Status) == SIGBUS);
      printf("  %s: process died by signal %d\n", Passed ? "ok  " : "FAIL", WIFSIGNALED(Status) ? WTERMSIG(Status) : 0);
    } else {
      Passed = WIFEXITED(Status) && WEXITSTATUS(Status) == 0;
      if (!Passed) {
        printf("  FAIL: exit status %d, signal %d\n", WIFEXITED(Status) ? WEXITSTATUS(Status) : -1,
               WIFSIGNALED(Status) ? WTERMSIG(Status) : 0);
      }
    }
    Failed += Passed ? 0 : 1;
    fflush(stdout);
  }
  printf("%s: %zu subtests, %d failed\n", Failed ? "FAIL" : "PASS", Count, Failed);
  return Failed ? 1 : 0;
}
