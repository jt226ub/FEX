/* SPDX-License-Identifier: MIT */
/*
 * Tests of the fexmac C interface, written in C against the public header only, and
 * linked the way an outside project links the library (the standalone archive).
 *
 * Each subtest runs in its own child process, because contexts and signal handlers are
 * process-wide and several subtests check how the process dies. A child that hangs is
 * killed by an alarm and reported as hung. See TestSupport.c.
 *
 * Usage: fexmac_c_test            run every subtest
 *        fexmac_c_test <name>     run one subtest in this process
 */
#include "TestSupport.h"

#include <inttypes.h>
#include <stddef.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
};

static fexmac_syscall_action RunSyscall(fexmac_thread* Thread, void* UserData) {
  struct SyscallRecord* Record = (struct SyscallRecord*)UserData;
  fexmac_x86_state State = GetState(Thread);
  ++Record->Calls;
  if (State.gpr[RAX] == 39) {
    if (State.gpr[RDI] != 5050 || State.gpr[RCX] != Record->CodeBase + 22 || State.rip != Record->CodeBase + 22) {
      ++Record->Problems;
    }
    Record->RunFromCallback = fexmac_thread_run(Thread, NULL);
    Record->DestroyFromCallback = fexmac_thread_destroy(Thread);
    State.gpr[RAX] = 1234;
    SetState(Thread, &State);
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

  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_SYSCALL, "first run stops at a system call (reason %d)", Stop.reason);
  Check(Stop.instruction_address == Record.CodeBase + 29, "stop names the system call instruction");
  Check(Record.Calls == 2 && Record.Problems == 0, "callback saw both system calls with the right registers (%d calls, %d problems)",
        Record.Calls, Record.Problems);
  Check(State.gpr[RBX] == 1234, "value the callback returned in rax reached the guest (rbx = %" PRIu64 ")", State.gpr[RBX]);
  Check(State.gpr[RDI] == 5050, "guest summed 1 to 100 (rdi = %" PRIu64 ")", State.gpr[RDI]);
  Check(State.rip == Record.CodeBase + 31, "rip is after the stopping system call");
  CheckResult(Record.RunFromCallback, FEXMAC_ERROR_BUSY, "running the thread from its own callback");
  CheckResult(Record.DestroyFromCallback, FEXMAC_ERROR_BUSY, "destroying the thread from its own callback");

  Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT, "second run stops at hlt (reason %d)", Stop.reason);
  Check(State.gpr[RDX] == 0x77 && State.rip == Record.CodeBase + 36 && Stop.instruction_address == Record.CodeBase + 36,
        "guest continued after the stop and rip is at hlt");

  CheckResult(fexmac_thread_destroy(Thread), FEXMAC_OK, "thread destroy");
  CheckResult(fexmac_context_destroy(Context), FEXMAC_OK, "context destroy");
}

/* ---- system calls with prefixes, and restarting a system call ---- */

static int PrefixCalls;
static uint64_t PrefixRip, PrefixRcx;

static fexmac_syscall_action PrefixSyscall(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  fexmac_x86_state State = GetState(Thread);
  ++PrefixCalls;
  PrefixRip = State.rip;
  PrefixRcx = State.gpr[RCX];
  State.gpr[RAX] = 0;
  SetState(Thread, &State);
  return FEXMAC_SYSCALL_RESUME;
}

static void TestSyscallPrefix(void) {
  /* mov eax,60 ; rex.w syscall (48 0F 05) ; mov edx,0x77 ; hlt */
  const uint8_t Program[] = {0xB8, 0x3C, 0, 0, 0, 0x48, 0x0F, 0x05, 0xBA, 0x77, 0, 0, 0, 0xF4};
  uint8_t* Code = MapGuestMemory(0x10000);
  memcpy(Code, Program, sizeof(Program));
  const uint64_t Base = (uint64_t)(uintptr_t)Code;
  fexmac_context* Context = CreateContext(PrefixSyscall, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);
  const fexmac_stop Stop = Run(Thread);
  const fexmac_x86_state State = GetState(Thread);
  Check(PrefixCalls == 1 && PrefixRip == Base + 8 && PrefixRcx == Base + 8, "callback sees rip after the 3-byte instruction (rip +%" PRIu64 ")",
        PrefixRip - Base);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RDX] == 0x77 && State.rip == Base + 13, "guest resumed at the next instruction");
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

static int RestartCalls;
static uint64_t RestartRax[3];

static fexmac_syscall_action RestartSyscall(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  fexmac_x86_state State = GetState(Thread);
  if (RestartCalls < 3) {
    RestartRax[RestartCalls] = State.gpr[RAX];
  }
  ++RestartCalls;
  if (RestartCalls == 1) {
    /* As a kernel restarting an interrupted call: the same call, run again. */
    State.gpr[RAX] = 39;
    SetState(Thread, &State);
    return FEXMAC_SYSCALL_RESTART;
  }
  State.gpr[RAX] = 5;
  SetState(Thread, &State);
  return FEXMAC_SYSCALL_RESUME;
}

static void TestSyscallRestart(void) {
  /* mov eax,7 ; syscall ; mov ebx,eax ; hlt */
  const uint8_t Program[] = {0xB8, 0x07, 0, 0, 0, 0x0F, 0x05, 0x89, 0xC3, 0xF4};
  uint8_t* Code = MapGuestMemory(0x10000);
  memcpy(Code, Program, sizeof(Program));
  const uint64_t Base = (uint64_t)(uintptr_t)Code;
  fexmac_context* Context = CreateContext(RestartSyscall, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);
  const fexmac_stop Stop = Run(Thread);
  const fexmac_x86_state State = GetState(Thread);
  Check(RestartCalls == 2, "restarted system call ran twice (%d calls)", RestartCalls);
  Check(RestartRax[0] == 7 && RestartRax[1] == 39, "the restart used the state the callback set (rax %" PRIu64 " then %" PRIu64 ")",
        RestartRax[0], RestartRax[1]);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RBX] == 5, "after the second call the guest continued (rbx = %" PRIu64 ")",
        State.gpr[RBX]);
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- state: set registers, guest reads them; get/set round trip; guest faults ---- */

/* clang-format off */
static const uint8_t StateProgram[] = {
  0x66, 0x48, 0x0F, 0x7E, 0xD8, /* 0: movq rax, xmm3 */
  0x0F, 0x92, 0xC1,             /* 5: setc cl */
  0x48, 0x89, 0xF2,             /* 8: mov rdx, rsi */
  0x8C, 0xDF,                   /* 11: mov edi, ds */
  0xF4,                         /* 13: hlt */
  0x0F, 0x0B,                   /* 14: ud2 */
  0xCC,                         /* 16: int3 */
};
/* clang-format on */

static void TestState(void) {
  uint8_t* Code = MapGuestMemory(0x10000);
  memcpy(Code, StateProgram, sizeof(StateProgram));
  const uint64_t Base = (uint64_t)(uintptr_t)Code;

  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);

  fexmac_x86_state First = GetState(Thread);
  Check(First.rflags == 0x202, "initial rflags is 0x202 (got 0x%" PRIx64 ")", First.rflags);
  Check(First.mxcsr == 0x1F80 && First.fcw == 0x37F && First.mxcsr_mask == 0xFFFF, "initial mxcsr, mxcsr_mask and fcw");
  Check(First.cs == 0x30, "initial cs is the core's 64-bit code selector (0x%x)", First.cs);
  SetState(Thread, &First);
  fexmac_x86_state Second = GetState(Thread);
  Check(memcmp(&First, &Second, sizeof(First)) == 0, "get, set, get gives identical state");

  First.xmm[3][0] = 0x1122334455667788ULL;
  First.gpr[RSI] = 0xFEEDFACECAFEBEEFULL;
  First.rflags = 0x202 | 1; /* CF */
  First.fs_base = 0x123450000ULL;
  First.ds = 0x2B;
  First.fip = 0x4242;
  SetState(Thread, &First);
  fexmac_stop Stop = Run(Thread);
  Second = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT, "program halts");
  Check(Second.gpr[RAX] == 0x1122334455667788ULL, "guest read xmm3 that was set");
  Check((Second.gpr[RCX] & 0xFF) == 1, "guest read the carry flag that was set");
  Check(Second.gpr[RDX] == 0xFEEDFACECAFEBEEFULL, "guest read rsi that was set");
  Check((Second.gpr[RDI] & 0xFFFF) == 0x2B && Second.ds == 0x2B, "guest read the ds selector that was set");
  Check(Second.fs_base == 0x123450000ULL && Second.fip == 0x4242, "fs base and fip kept");
  Check(Second.fop == 0 && Second.fdp == 0, "fop and fdp are reported as 0");

  Second.rip = Base + 14;
  SetState(Thread, &Second);
  Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 6 && Stop.signal == SIGILL && Stop.instruction_address == Base + 14,
        "ud2 stops with a guest fault, #UD, SIGILL, at the instruction (reason %d, trap %u, signal %d)", Stop.reason, Stop.trap_number,
        Stop.signal);

  Second.rip = Base + 16;
  SetState(Thread, &Second);
  Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 3 && Stop.signal == SIGTRAP,
        "int3 stops with a guest fault, #BP, SIGTRAP (reason %d, trap %u, signal %d)", Stop.reason, Stop.trap_number, Stop.signal);

  uint32_t Regs[4] = {0};
  CheckResult(fexmac_cpuid(Context, 0, 0, Regs), FEXMAC_OK, "cpuid");
  Check(Regs[0] >= 1, "cpuid reports a maximum leaf (%u)", Regs[0]);

  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- abi: struct_size rules for caller-allocated structures ---- */

/* Nothing past a structure the library does not know the size of is ever written: an
 * uninitialised or garbage struct_size is refused with the memory untouched. */
static void TestABI(void) {
  uint8_t* Code = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xB8, 0x2A, 0, 0, 0, 0xF4};
  memcpy(Code, Program, sizeof(Program));
  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, 0);

  struct {
    fexmac_x86_state State;
    uint64_t After[64];
  } Frame;
  const uint32_t Sizes[] = {0, sizeof(fexmac_x86_state) - 8, sizeof(fexmac_x86_state) + 8, sizeof(fexmac_x86_state) + 256, 0xAAAAAAAA};
  int Untouched = 1, Refused = 1;
  for (size_t i = 0; i < sizeof(Sizes) / sizeof(Sizes[0]); ++i) {
    memset(&Frame, 0xAA, sizeof(Frame));
    Frame.State.struct_size = Sizes[i];
    Refused &= fexmac_thread_get_state(Thread, &Frame.State) == FEXMAC_ERROR_INVALID_ARGUMENT;
    Refused &= fexmac_thread_set_state(Thread, &Frame.State) == FEXMAC_ERROR_INVALID_ARGUMENT;
    const uint8_t* Bytes = (const uint8_t*)&Frame;
    for (size_t b = sizeof(uint32_t); b < sizeof(Frame); ++b) {
      Untouched &= Bytes[b] == 0xAA;
    }
  }
  Check(Refused, "get and set state refuse every struct_size but the known one (0, smaller, larger, garbage)");
  Check(Untouched, "and write nothing, inside the structure or past it");

  fexmac_stop Stop;
  memset(&Stop, 0xAA, sizeof(Stop));
  CheckResult(fexmac_thread_run(Thread, &Stop), FEXMAC_ERROR_INVALID_ARGUMENT, "run with an uninitialised stop record");
  CheckResult(fexmac_thread_run(Thread, NULL), FEXMAC_OK, "run with no stop record");
  /* The 2.0 stop record gained access and reserved (package D) before 2.0 shipped: 48
   * bytes; the 40-byte draft layout is not a size this library knows. */
  Check(sizeof(fexmac_stop) == 48 && offsetof(fexmac_stop, access) == 40, "stop record is 48 bytes with access at 40");
  memset(&Stop, 0xAA, sizeof(Stop));
  Stop.struct_size = 40;
  CheckResult(fexmac_thread_run(Thread, &Stop), FEXMAC_ERROR_INVALID_ARGUMENT, "run with a 40-byte stop record");
  fexmac_stop_init(&Stop);
  fexmac_x86_state Rewind;
  fexmac_x86_state_init(&Rewind);
  fexmac_thread_get_state(Thread, &Rewind);
  Rewind.rip = (uint64_t)(uintptr_t)Code;
  fexmac_thread_set_state(Thread, &Rewind);
  CheckResult(fexmac_thread_run(Thread, &Stop), FEXMAC_OK, "run with an initialised stop record");
  Check(Stop.reason == FEXMAC_STOP_HALT && Stop.access == 0 && Stop.reserved == 0, "a halt has no access kind");

  fexmac_config Config;
  fexmac_context* Other = NULL;
  memset(&Config, 0xAA, sizeof(Config));
  CheckResult(fexmac_context_create(&Config, &Other), FEXMAC_ERROR_INVALID_ARGUMENT, "uninitialised config");
  fexmac_config_init(&Config);
  Config.struct_size += 8;
  CheckResult(fexmac_context_create(&Config, &Other), FEXMAC_ERROR_INVALID_ARGUMENT, "config of an unknown (larger) size");
  fexmac_config_init(&Config);
  Config.code_cache_enabled = 2;
  CheckResult(fexmac_context_create(&Config, &Other), FEXMAC_ERROR_BAD_CONFIG, "code_cache_enabled other than 0 or 1");
  fexmac_config_init(&Config);
  Config.code_cache_directory = "/tmp";
  CheckResult(fexmac_context_create(&Config, &Other), FEXMAC_ERROR_BAD_CONFIG, "a cache directory with the cache off");
  fexmac_config_init(&Config);
  Config.code_cache_enabled = 1;
  Config.code_cache_directory = "";
  CheckResult(fexmac_context_create(&Config, &Other), FEXMAC_ERROR_BAD_CONFIG, "an empty cache directory name");
  fexmac_config_init(&Config);
  Config.reserved2 = 1;
  CheckResult(fexmac_context_create(&Config, &Other), FEXMAC_ERROR_UNSUPPORTED, "a reserved config field set");

  fexmac_translation_counters Counters;
  memset(&Counters, 0xAA, sizeof(Counters));
  CheckResult(fexmac_context_get_counters(Context, &Counters), FEXMAC_ERROR_INVALID_ARGUMENT, "counters with an uninitialised struct_size");
  fexmac_translation_counters_init(&Counters);
  Counters.struct_size += 8;
  CheckResult(fexmac_context_get_counters(Context, &Counters), FEXMAC_ERROR_INVALID_ARGUMENT, "counters of an unknown (larger) size");
  fexmac_translation_counters_init(&Counters);
  CheckResult(fexmac_context_get_counters(Context, &Counters), FEXMAC_OK, "counters of the known size");
  Check(fexmac_version() == ((2u << 16) | 0u) && FEXMAC_VERSION_MAJOR == 2, "version 2.0, the first with struct_size");

  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- invalidate: changed guest code is retranslated after fexmac_invalidate ---- */

static void TestInvalidate(void) {
  uint8_t* Code = MapGuestMemory(0x10000);
  const uint8_t Program[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF4}; /* mov eax, imm32 ; hlt */
  memcpy(Code, Program, sizeof(Program));
  const uint64_t Base = (uint64_t)(uintptr_t)Code;

  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);
  Run(Thread);
  fexmac_x86_state State = GetState(Thread);
  Check(State.gpr[RAX] == 1, "first version of the code returns 1");

  Code[1] = 2;
  State.rip = Base;
  SetState(Thread, &State);
  Run(Thread);
  State = GetState(Thread);
  printf("  info: without invalidation the changed code returns %" PRIu64 " (1 means the stale translation ran)\n", State.gpr[RAX]);

  Code[1] = 3;
  CheckResult(fexmac_invalidate(Context, Base, sizeof(Program)), FEXMAC_OK, "invalidate");
  State.rip = Base;
  SetState(Thread, &State);
  Run(Thread);
  State = GetState(Thread);
  Check(State.gpr[RAX] == 3, "after invalidation the changed code runs (rax = %" PRIu64 ")", State.gpr[RAX]);

  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- faults: misaligned atomics, in each signal-handler arrangement ---- */

static uint8_t* HostPage;

/* A host handler for faults of its own: makes the page accessible and retries. */
static void HostOwnHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Context;
  if ((uint8_t*)Info->si_addr >= HostPage && (uint8_t*)Info->si_addr < HostPage + PageSize) {
    Shared[0] += 1;
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

static void InstallHostHandler(void (*Handler)(int, siginfo_t*, void*), int Flags) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = Handler;
  Action.sa_flags = SA_SIGINFO | Flags;
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
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = (uint64_t)(uintptr_t)Value;
  State.gpr[RAX] = 3;
  SetState(Thread, &State);

  const fexmac_stop Stop = Run(Thread);
  State = GetState(Thread);
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
  const uint32_t Before = Shared[0];
  ((volatile uint8_t*)HostPage)[16] = 42;
  Check(Shared[0] == Before + 1 && HostPage[16] == 42, "host's own fault reached the host handler");
}

static void TestFaultsInstalled(void) {
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers");
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers again");
  RunMisalignedAtomic();
}

/* The host installed handlers first; the library chains to them. */
static void TestFaultsChained(void) {
  InstallHostHandler(HostOwnHandler, SA_NODEFER);
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers over the host's");
  RunMisalignedAtomic();
  TouchHostPage();
}

/* The host owns the handlers and forwards to fexmac_handle_fault. */
static void TestFaultsHosted(void) {
  InstallHostHandler(HostForwardingHandler, SA_NODEFER);
  RunMisalignedAtomic();
  TouchHostPage();
}

/* ---- call/return stack: overflow and underflow into its guard pages ---- */

static void RunCallRetProgram(const uint8_t* Program, size_t Size, uint32_t Count, size_t CountOffset, size_t StackSize, uint64_t* Rax,
                              uint64_t* Rcx) {
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* Stack = MapGuestMemory(StackSize);
  memcpy(Code, Program, Size);
  memcpy(Code + CountOffset, &Count, sizeof(Count));
  fexmac_install_fault_handlers();
  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, (uint64_t)(uintptr_t)Stack + StackSize - 64);
  const fexmac_stop Stop = Run(Thread);
  const fexmac_x86_state State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT, "program runs to the halt (reason %d)", Stop.reason);
  *Rax = State.gpr[RAX];
  *Rcx = State.gpr[RCX];
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* Recursion 100,000 deep: more calls than the call/return stack holds (overflow), then
 * every return. */
static void TestCallRetRecursion(void) {
  /* clang-format off */
  const uint8_t Program[] = {
    0xB9, 0, 0, 0, 0,             /*  0: mov ecx, N */
    0x31, 0xC0,                   /*  5: xor eax, eax */
    0xE8, 0x01, 0x00, 0x00, 0x00, /*  7: call 13 */
    0xF4,                         /* 12: hlt */
    0xFF, 0xC0,                   /* 13: inc eax */
    0xFF, 0xC9,                   /* 15: dec ecx */
    0x74, 0x05,                   /* 17: jz 24 */
    0xE8, 0xF5, 0xFF, 0xFF, 0xFF, /* 19: call 13 */
    0xC3,                         /* 24: ret */
  };
  /* clang-format on */
  uint64_t Rax, Rcx;
  RunCallRetProgram(Program, sizeof(Program), 100000, 1, 4 * 1024 * 1024, &Rax, &Rcx);
  Check(Rax == 100000 && Rcx == 0, "recursion 100,000 deep returns to the top (eax %" PRIu64 ")", Rax);
}

/* Recursion 1,000,000 deep, counting on the way back (the reviewer's rec.c): every
 * return after the stack was reset still lands where the guest expects. */
static void TestCallRetDeepRecursion(void) {
  /* clang-format off */
  const uint8_t Program[] = {
    0xB9, 0, 0, 0, 0,             /*  0: mov ecx, N */
    0xE8, 0x01, 0x00, 0x00, 0x00, /*  5: call 11 */
    0xF4,                         /* 10: hlt */
    0xFF, 0xC9,                   /* 11: dec ecx */
    0x74, 0x07,                   /* 13: jz 22 */
    0xE8, 0xF7, 0xFF, 0xFF, 0xFF, /* 15: call 11 */
    0xFF, 0xC2,                   /* 20: inc edx */
    0xC3,                         /* 22: ret */
  };
  /* clang-format on */
  uint8_t* Code = MapGuestMemory(0x10000);
  const size_t StackSize = 64u << 20;
  uint8_t* Stack = MapGuestMemory(StackSize);
  const uint32_t Count = 1000000;
  memcpy(Code, Program, sizeof(Program));
  memcpy(Code + 1, &Count, sizeof(Count));
  fexmac_install_fault_handlers();
  fexmac_context* Context = CreateContext(NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, (uint64_t)(uintptr_t)Stack + StackSize - 64);
  const fexmac_stop Stop = Run(Thread);
  const fexmac_x86_state State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RDX] == Count - 1 && State.rip == (uint64_t)(uintptr_t)Code + 10,
        "recursion 1,000,000 deep returns through every level (edx %" PRIu64 ")", State.gpr[RDX]);
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* 200,000 calls that never return (the guest stack is rebalanced with pop): overflow. */
static void TestCallRetOverflow(void) {
  /* clang-format off */
  const uint8_t Program[] = {
    0xB9, 0, 0, 0, 0,             /*  0: mov ecx, N */
    0xE8, 0x00, 0x00, 0x00, 0x00, /*  5: call 10 (the next instruction) */
    0x58,                         /* 10: pop rax */
    0xFF, 0xC9,                   /* 11: dec ecx */
    0x75, 0xF6,                   /* 13: jnz 5 */
    0xF4,                         /* 15: hlt */
  };
  /* clang-format on */
  uint64_t Rax, Rcx;
  RunCallRetProgram(Program, sizeof(Program), 200000, 1, 0x10000, &Rax, &Rcx);
  Check(Rcx == 0, "200,000 unmatched calls complete");
}

/* 200,000 returns that were never called (push; ret): underflow. */
static void TestCallRetUnderflow(void) {
  /* clang-format off */
  const uint8_t Program[] = {
    0xB9, 0, 0, 0, 0,                         /*  0: mov ecx, N */
    0x48, 0x8D, 0x05, 0x02, 0x00, 0x00, 0x00, /*  5: lea rax, [rip+2] (14) */
    0x50,                                     /* 12: push rax */
    0xC3,                                     /* 13: ret */
    0xFF, 0xC9,                               /* 14: dec ecx */
    0x75, 0xF3,                               /* 16: jnz 5 */
    0xF4,                                     /* 18: hlt */
  };
  /* clang-format on */
  uint64_t Rax, Rcx;
  RunCallRetProgram(Program, sizeof(Program), 200000, 1, 0x10000, &Rax, &Rcx);
  Check(Rcx == 0, "200,000 unmatched returns complete");
}

/* ---- guest memory faults in translated code: delivered as stops, then fixed and re-run ---- */

/* Runs a one-instruction access on a protected page (rdi = page + Offset), checks the
 * stop the library reports, makes the page accessible and runs the same instruction
 * again to the hlt after it. */
static void RunGuestAccess(const uint8_t* Program, size_t Size, int Protection, const char* TSO, uint64_t Offset, uint32_t Access) {
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* Data = mmap(NULL, (size_t)PageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memcpy(Code, Program, Size);
  if (Protection == PROT_READ) {
    (void)*(volatile uint8_t*)Data; /* touched, so the store is a permission fault */
  }
  mprotect(Data, (size_t)PageSize, Protection);
  const fexmac_option Options[] = {{"TSOENABLED", TSO}};
  fexmac_context* Context = CreateContextWithOptions(Options, 1, NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = (uint64_t)(uintptr_t)Data + Offset;
  State.gpr[RAX] = 5;
  SetState(Thread, &State);
  Shared[0] = 1; /* reached the guest run */
  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state After = GetState(Thread);
  const uint64_t Expected = (uint64_t)(uintptr_t)Data + Offset;
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 14, "the access stops the run as a page fault (reason %d, trap %u)",
        Stop.reason, Stop.trap_number);
  Check(Stop.instruction_address == (uint64_t)(uintptr_t)Code && After.rip == (uint64_t)(uintptr_t)Code,
        "rip and instruction_address name the faulting instruction");
  Check(Stop.fault_address >= (uint64_t)(uintptr_t)Data && Stop.fault_address <= Expected + 15 &&
          (Stop.fault_address == Expected || Offset % 16 != 0),
        "fault address 0x%" PRIx64 " is the access (0x%" PRIx64 ")", Stop.fault_address, Expected);
  Check(Stop.access == Access, "access %u (expected %u)", Stop.access, Access);
  Check(Stop.signal == SIGSEGV && Stop.si_code == SEGV_ACCERR, "SIGSEGV with SEGV_ACCERR (signal %d, si_code %d)", Stop.signal, Stop.si_code);
  const uint32_t ExpectedError = FEXMAC_PF_USER | (Access == FEXMAC_ACCESS_WRITE ? FEXMAC_PF_WRITE : 0) |
                                 (Protection == PROT_READ ? FEXMAC_PF_PRESENT : 0);
  Check(Stop.error_code == ExpectedError, "#PF error code 0x%x (expected 0x%x)", Stop.error_code, ExpectedError);
  Check(After.gpr[RAX] == 5 && After.gpr[RDI] == Expected, "registers as before the instruction");
  mprotect(Data, (size_t)PageSize, PROT_READ | PROT_WRITE);
  Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT, "after the page is made accessible the same instruction runs (reason %d)", Stop.reason);
  Shared[0] = 2;
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

static const uint8_t LockXadd[] = {0xF0, 0x0F, 0xC1, 0x07, 0xF4};
static const uint8_t LockCmpxchg[] = {0xF0, 0x0F, 0xB1, 0x0F, 0xF4};
static const uint8_t Load[] = {0x8B, 0x07, 0xF4};
static const uint8_t Store[] = {0x89, 0x07, 0xF4};

static void TestGuestLoadProtNone(void) {
  fexmac_install_fault_handlers();
  RunGuestAccess(Load, sizeof(Load), PROT_NONE, "1", 64, FEXMAC_ACCESS_READ);
}
static void TestGuestLockXaddProtNone(void) {
  fexmac_install_fault_handlers();
  RunGuestAccess(LockXadd, sizeof(LockXadd), PROT_NONE, "1", 64, FEXMAC_ACCESS_WRITE);
}
static void TestGuestLockCmpxchgProtNone(void) {
  fexmac_install_fault_handlers();
  RunGuestAccess(LockCmpxchg, sizeof(LockCmpxchg), PROT_NONE, "0", 64, FEXMAC_ACCESS_WRITE);
}
/* A misaligned atomic on a PROT_NONE page: the alignment fault comes first. The library
 * sees that the memory is inaccessible before the core starts rewriting the access (which
 * would fault inside the handler) and reports the guest's page fault. */
static void TestGuestMisalignedProtNone(void) {
  fexmac_install_fault_handlers();
  RunGuestAccess(LockXadd, sizeof(LockXadd), PROT_NONE, "0", 64 + 14, FEXMAC_ACCESS_WRITE);
}

static void TestGuestStoreReadOnly(void) {
  fexmac_install_fault_handlers();
  RunGuestAccess(Store, sizeof(Store), PROT_READ, "0", 64, FEXMAC_ACCESS_WRITE);
}

/* With the host owning the signals, fexmac_handle_fault claims the guest's fault (the run
 * stops) and the host handler never sees it; a fault of the host's own still reaches it. */
static volatile int HostSawFault;
static sigjmp_buf HostFaultJump;
static void GuestFaultHostHandler(int Signal, siginfo_t* Info, void* Context) {
  if (fexmac_handle_fault(Signal, Info, Context)) {
    return;
  }
  HostSawFault = 1;
  siglongjmp(HostFaultJump, 1);
}

static void TestGuestFaultHosted(void) {
  InstallHostHandler(GuestFaultHostHandler, SA_NODEFER);
  RunGuestAccess(LockXadd, sizeof(LockXadd), PROT_NONE, "1", 64, FEXMAC_ACCESS_WRITE);
  Check(!HostSawFault, "the host handler did not see the guest's fault");
  volatile uint8_t* Page = mmap(NULL, (size_t)PageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!sigsetjmp(HostFaultJump, 1)) {
    (void)Page[0];
  }
  Check(HostSawFault, "a fault in host code still reaches the host handler");
  Shared[0] = HostSawFault ? 3 : 4;
}

/* ---- callbacks: what a system call callback may and may not do ---- */

static uint8_t* CloneCode;
static fexmac_thread* Victim;
static fexmac_thread* Cloned;
static fexmac_result CloneResults[4];

static fexmac_syscall_action CloneSyscall(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  fexmac_context* Context = fexmac_thread_context(Thread);
  CloneResults[0] = fexmac_thread_create(Context, &Cloned);
  if (CloneResults[0] == FEXMAC_OK) {
    fexmac_x86_state State = GetState(Cloned);
    State.rip = (uint64_t)(uintptr_t)CloneCode + 16;
    SetState(Cloned, &State);
  }
  CloneResults[1] = fexmac_thread_destroy(Victim);
  CloneResults[2] = Cloned ? fexmac_thread_run(Cloned, NULL) : FEXMAC_OK;
  CloneResults[3] = fexmac_invalidate(Context, (uint64_t)(uintptr_t)CloneCode + 0x1000, 16);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RAX] = 1;
  SetState(Thread, &State);
  return FEXMAC_SYSCALL_RESUME;
}

static void TestCallbacks(void) {
  CloneCode = MapGuestMemory(0x10000);
  /* 0: mov eax,56 ; syscall ; hlt     16: mov eax,7 ; hlt */
  const uint8_t Parent[] = {0xB8, 0x38, 0, 0, 0, 0x0F, 0x05, 0xF4};
  const uint8_t Child[] = {0xB8, 0x07, 0, 0, 0, 0xF4};
  memcpy(CloneCode, Parent, sizeof(Parent));
  memcpy(CloneCode + 16, Child, sizeof(Child));
  fexmac_context* Context = CreateContext(CloneSyscall, NULL);
  Victim = CreateThreadAt(Context, (uint64_t)(uintptr_t)CloneCode + 16, 0);
  fexmac_thread* Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)CloneCode, 0);
  fexmac_stop Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT, "parent ran to its halt");
  CheckResult(CloneResults[0], FEXMAC_OK, "creating a thread from a callback (a guest clone)");
  CheckResult(CloneResults[1], FEXMAC_OK, "destroying another, stopped thread from a callback");
  CheckResult(CloneResults[2], FEXMAC_ERROR_BUSY, "running another thread from a callback");
  CheckResult(CloneResults[3], FEXMAC_OK, "invalidating from a callback");
  CheckResult(fexmac_thread_run(Victim, NULL), FEXMAC_ERROR_INVALID_HANDLE, "the destroyed thread is gone");
  Stop = Run(Cloned);
  const fexmac_x86_state State = GetState(Cloned);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 7, "the thread created in the callback runs afterwards");
  fexmac_thread_destroy(Cloned);
  fexmac_thread_destroy(Thread);
  CheckResult(fexmac_context_destroy(Context), FEXMAC_OK, "context destroy");
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
  Config.option_count = 1;
  CheckResult(fexmac_context_create(&Config, &Context), FEXMAC_ERROR_INVALID_ARGUMENT, "create with options missing");

  static const struct {
    const char* Name;
    const char* Value;
    const char* Why;
  } Bad[] = {
    {"NOSUCHOPTION", "1", "unknown option"},
    {"DISKCACHE", "1", "core option this port does not support yet (aborts the process if allowed)"},
    {"DISKCACHEPATH", "/tmp", "code cache option (package F)"},
    {"JITFEATURES", "nonsense", "core option not on the supported list"},
    {"GDBSERVER", "1", "core option not on the supported list"},
    {"SINGLESTEP", "1", "core option not on the supported list"},
    {"ENABLECODECACHINGWIP", "1", "code cache option (package F)"},
    {"FORCESVEWIDTH", "12345", "core option not on the supported list"},
    {"STRICTINPROCESSSPLITLOCKS", "1", "option that makes the fault handler spin"},
    {"MAXINST", "lots", "number that does not parse"},
    {"MAXINST", "12abc", "number with trailing text"},
    {"MAXINST", "010", "number with a leading zero (would be octal)"},
    {"MAXINST", "-5", "negative number"},
    {"MAXINST", "0", "zero instructions per block"},
    {"MAXINST", "4294967296", "number out of range"},
    {"MULTIBLOCK", "2", "boolean that is not 0 or 1"},
    {"MULTIBLOCK", "true", "boolean spelt true"},
    {"SMCCHECKS", "sometimes", "unknown named value"},
    {"HOSTFEATURES", "disablesve,nonsense", "unknown host feature name"},
    {"HOSTFEATURES", "enablesve", "forcing a feature on (the M4 has no SVE: SIGILL)"},
    {"HOSTFEATURES", "disableavx,enableafp", "any enable name"},
    {"IS64BIT_MODE", "0", "32-bit mode"},
    {NULL, "1", "option without a name"},
  };
  for (size_t i = 0; i < sizeof(Bad) / sizeof(Bad[0]); ++i) {
    char What[160];
    snprintf(What, sizeof(What), "%s=%s (%s)", Bad[i].Name ? Bad[i].Name : "(null)", Bad[i].Value, Bad[i].Why);
    CheckResult(CreateWithOption(Bad[i].Name, Bad[i].Value, &Context), FEXMAC_ERROR_BAD_CONFIG, What);
  }
  Check(Context == NULL, "failed creates leave the output NULL");

  Check(fexmac_option_known("MULTIBLOCK") && fexmac_option_known("Multiblock"), "option names known in both spellings");
  Check(!fexmac_option_known("FEX_MULTIBLOCK") && !fexmac_option_known("DISKCACHE") && !fexmac_option_known(NULL), "other names unknown");

  /* After all those failures, a good configuration still works. */
  fexmac_option Good[] = {{"SMCCHECKS", "full"}, {"MaxInst", "0x100"}, {"MULTIBLOCK", "0"}, {"HOSTFEATURES", "disablesve,disablefrintts"},
                          {"HostFeatures", "off"}};
  fexmac_config_init(&Config);
  Config.options = Good;
  Config.option_count = sizeof(Good) / sizeof(Good[0]);
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
  fexmac_x86_state_init(&State);
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
  const fexmac_stop Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 42, "a second context, created after the first, runs code");
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);

  Check(fexmac_version() >> 16 == FEXMAC_VERSION_MAJOR, "library version matches the header");
}

/* ---- the previous handler: masks, SA_RESETHAND, faults inside it, sent signals ---- */

static void MaskCheckingHandler(int Signal, siginfo_t* Info, void* Context) {
  sigset_t Current;
  pthread_sigmask(SIG_BLOCK, NULL, &Current);
  /* Its own mask (SIGUSR1) applied; its own (synchronous) signal never blocked, so a
   * fault inside it cannot hang. */
  Shared[1] = (uint32_t)(sigismember(&Current, SIGUSR1) && !sigismember(&Current, Signal));
  HostOwnHandler(Signal, Info, Context);
}

static void TestChainedMask(void) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = MaskCheckingHandler;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  sigaddset(&Action.sa_mask, SIGUSR1);
  sigaction(SIGBUS, &Action, NULL);
  sigaction(SIGSEGV, &Action, NULL);
  fexmac_install_fault_handlers();
  TouchHostPage();
  Check(Shared[1] == 1, "the previous handler ran with its sa_mask, and SIGSEGV/SIGBUS not blocked");
  sigset_t Current;
  pthread_sigmask(SIG_BLOCK, NULL, &Current);
  Check(!sigismember(&Current, SIGUSR1), "the mask is restored afterwards");
}

/* A previous handler that returns without fixing the cause gets the same fault again,
 * every time, as with only it installed: here it fixes the page on its 1,000th entry, and
 * every one of those entries must have reached it. */
static void RetryingHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Context;
  if ((uint8_t*)Info->si_addr >= HostPage && (uint8_t*)Info->si_addr < HostPage + PageSize && ++Shared[0] == 1000) {
    mprotect(HostPage, (size_t)PageSize, PROT_READ | PROT_WRITE);
  }
}

static void TestChainedRetry(void) {
  InstallHostHandler(RetryingHandler, SA_NODEFER);
  fexmac_install_fault_handlers();
  HostPage = MapGuestMemory((size_t)PageSize);
  mprotect(HostPage, (size_t)PageSize, PROT_NONE);
  ((volatile uint8_t*)HostPage)[16] = 42;
  Check(Shared[0] == 1000 && HostPage[16] == 42, "a previous handler that returns sees every retry until it fixes the page (%u entries)",
        Shared[0]);
}

/* SA_RESETHAND: the first fault goes to the host's handler, the second gets the default. */
static void TestChainedResetHand(void) {
  InstallHostHandler(HostOwnHandler, SA_RESETHAND);
  fexmac_install_fault_handlers();
  TouchHostPage();
  RunMisalignedAtomic(); /* the library still works after the reset */
  uint8_t* Page = MapGuestMemory((size_t)PageSize);
  mprotect(Page, (size_t)PageSize, PROT_NONE);
  ((volatile uint8_t*)Page)[0] = 1;
  printf("  FAIL: survived a second fault after SA_RESETHAND\n");
  exit(1);
}

/* A previous handler that faults itself. The library does not police handlers it chains
 * to: the nested fault is passed to the same handler again, which faults again, until
 * the stack overflows and the process dies. It must die, quickly, not hang, with and
 * without SA_NODEFER on the previous handler and with and without frame pointers in it
 * (FaultingHandlerNoFramePointer is compiled with -fomit-frame-pointer). */
static void FaultingHostHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Info;
  (void)Context;
  Shared[0] = 1;
  *(volatile int*)HostPage = 1; /* the host's handler has a bug */
}

void FaultingHandlerNoFramePointer(int Signal, siginfo_t* Info, void* Context);
uint8_t* FaultingHandlerPage(void);
uint8_t* FaultingHandlerPage(void) {
  return HostPage;
}

static void RunFaultingPrevious(void (*Handler)(int, siginfo_t*, void*), int Flags) {
  HostPage = MapGuestMemory((size_t)PageSize);
  mprotect(HostPage, (size_t)PageSize, PROT_NONE);
  InstallHostHandler(Handler, Flags);
  fexmac_install_fault_handlers();
  ((volatile uint8_t*)HostPage)[16] = 42;
  printf("  FAIL: survived a fault inside the host's handler\n");
  exit(1);
}

static void TestChainedHandlerFaults(void) {
  RunFaultingPrevious(FaultingHostHandler, SA_NODEFER);
}
static void TestChainedHandlerFaultsDeferring(void) {
  RunFaultingPrevious(FaultingHostHandler, 0);
}
static void TestChainedHandlerFaultsNoFramePointer(void) {
  RunFaultingPrevious(FaultingHandlerNoFramePointer, 0);
}

/* The reviewer's ljmp4/ljmp2: a previous handler that probes memory with siglongjmp,
 * called from frames of many sizes and depths, so that later faults happen above,
 * below and exactly at the depth of earlier, abandoned handler frames. Every layout
 * must survive: no later fault may be mistaken for a fault inside a handler. */
static sigjmp_buf GridJump;
static volatile uint8_t* GridBad;

static void GridHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Info;
  (void)Context;
  siglongjmp(GridJump, 1);
}

__attribute__((noinline)) static int GridProbe(void) {
  if (sigsetjmp(GridJump, 1)) {
    return 0;
  }
  (void)*GridBad;
  return 1;
}

__attribute__((noinline)) static int GridRecurse(int Depth, size_t Pad) {
  volatile char* Space = __builtin_alloca(Pad);
  (void)Space;
  if (Depth == 0) {
    return GridProbe();
  }
  return GridRecurse(Depth - 1, Pad) + 0 * (int)Space[0];
}

__attribute__((noinline)) static int GridFirst(size_t Pad) {
  volatile char* Space = __builtin_alloca(Pad);
  (void)Space;
  return GridProbe();
}

static void TestSiglongjmpLayouts(void) {
  int Trials = 0, Bad = 0;
  for (size_t Pad1 = 0; Pad1 <= 4096; Pad1 += 256) {
    for (int Depth = 0; Depth <= 5; ++Depth) {
      for (size_t Pad2 = 16; Pad2 <= 2048; Pad2 += 1016) {
        const pid_t Child = fork();
        if (Child == 0) {
          alarm(5);
          struct sigaction Action;
          memset(&Action, 0, sizeof(Action));
          Action.sa_sigaction = GridHandler;
          Action.sa_flags = SA_SIGINFO;
          sigemptyset(&Action.sa_mask);
          sigaction(SIGSEGV, &Action, NULL);
          sigaction(SIGBUS, &Action, NULL);
          fexmac_install_fault_handlers();
          GridBad = mmap(NULL, (size_t)PageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
          const int Results = GridFirst(Pad1) + GridRecurse(Depth, Pad2) + GridRecurse(Depth + 3, Pad2 / 2 + 16);
          _exit(Results == 0 ? 0 : 1);
        }
        int Status = 0;
        waitpid(Child, &Status, 0);
        ++Trials;
        if (!WIFEXITED(Status) || WEXITSTATUS(Status) != 0) {
          if (++Bad <= 5) {
            printf("  layout pad1 %zu depth %d pad2 %zu: status 0x%x\n", Pad1, Depth, Pad2, Status);
          }
        }
      }
    }
  }
  Check(Bad == 0, "%d stack layouts of probe, abandoned handler frame, later probe: %d failed", Trials, Bad);
}

/* A previous handler that leaves with siglongjmp (the usual memory-probe idiom; the
 * reviewer's ljmp.c). The library must not take the next fault for a fault inside that
 * handler, must keep handling its own faults, and logging must still reach the callback. */
static sigjmp_buf ProbeJump;

static void ProbeHandler(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Info;
  (void)Context;
  siglongjmp(ProbeJump, 1);
}

static int Readable(volatile uint8_t* Address) {
  if (sigsetjmp(ProbeJump, 1)) {
    return 0;
  }
  (void)*Address;
  return 1;
}

static uint32_t LogCalls;
static void CountLog(fexmac_log_level Level, const char* Message, void* UserData) {
  (void)Level;
  (void)Message;
  (void)UserData;
  ++LogCalls;
}

static void TestChainedSiglongjmp(void) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = ProbeHandler;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  sigaction(SIGSEGV, &Action, NULL);
  sigaction(SIGBUS, &Action, NULL);
  fexmac_install_fault_handlers();
  uint8_t* Bad = mmap(NULL, (size_t)PageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int Probes = 0;
  for (int i = 0; i < 3; ++i) {
    Probes += Readable(Bad) == 0;
  }
  Check(Probes == 3, "three probes in a row, each left with siglongjmp, all report unreadable");
  RunMisalignedAtomic();
  /* A log message still reaches the callback (the bad option is logged). */
  const fexmac_option BadOption = {"NOSUCHOPTION", "1"};
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = &BadOption;
  Config.option_count = 1;
  Config.log = CountLog;
  fexmac_context* Context = NULL;
  fexmac_context_create(&Config, &Context);
  Check(LogCalls == 1, "after the jumps the log callback is still called (%u calls)", LogCalls);
  Check(Readable(Bad) == 0, "and a fourth probe still works");
}

/* A SIGSEGV sent with kill() while the previous action was "ignore" is ignored, and the
 * library stays armed. */
static void TestSentSignalIgnored(void) {
  signal(SIGSEGV, SIG_IGN);
  fexmac_install_fault_handlers();
  kill(getpid(), SIGSEGV);
  Check(1, "survived a sent SIGSEGV that was ignored before the library");
  RunMisalignedAtomic();
}

/* A SIGSEGV sent with kill() while the previous action was the default kills. */
static void TestSentSignalDefaultDies(void) {
  fexmac_install_fault_handlers();
  Shared[0] = 1;
  kill(getpid(), SIGSEGV);
  sleep(1);
  printf("  FAIL: survived a sent SIGSEGV with the default action\n");
  exit(1);
}

/* A fault nobody handles, outside guest code, kills the process normally and silently. */
static void TestUnclaimedFaultDies(void) {
  fexmac_install_fault_handlers();
  uint8_t* Page = MapGuestMemory((size_t)PageSize);
  mprotect(Page, (size_t)PageSize, PROT_NONE);
  Shared[0] = 1;
  ((volatile uint8_t*)Page)[0] = 1;
  Shared[0] = 2;
  printf("  FAIL: survived a fault nobody handles\n");
  exit(1);
}

/* ---- concurrency: invalidation from callbacks and another thread ---- */

static fexmac_context* MtContext;
static uint8_t* MtCode;
static atomic_int MtDone;
static atomic_long MtCalls;

static fexmac_syscall_action InvalidatingSyscall(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  atomic_fetch_add(&MtCalls, 1);
  if (fexmac_invalidate(MtContext, (uint64_t)(uintptr_t)MtCode + 0x100, 16) != FEXMAC_OK) {
    Shared[2] = 1;
  }
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RAX] = 0;
  SetState(Thread, &State);
  return FEXMAC_SYSCALL_RESUME;
}

static void* InvalidatingWorker(void* Arg) {
  (void)Arg;
  fexmac_thread* Thread = NULL;
  if (fexmac_thread_create(MtContext, &Thread) != FEXMAC_OK) {
    Shared[2] = 1;
    atomic_fetch_add(&MtDone, 1);
    return NULL;
  }
  fexmac_x86_state State = GetState(Thread);
  State.rip = (uint64_t)(uintptr_t)MtCode;
  SetState(Thread, &State);
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  if (fexmac_thread_run(Thread, &Stop) != FEXMAC_OK || Stop.reason != FEXMAC_STOP_HALT) {
    Shared[2] = 1;
  }
  fexmac_thread_destroy(Thread);
  atomic_fetch_add(&MtDone, 1);
  return NULL;
}

/* A hang here is the failure this test exists to catch, so it watches progress itself
 * and fails within seconds instead of waiting for the driver's alarm. */
static atomic_long MtInvalidations;

static void* ProgressWatchdog(void* Arg) {
  (void)Arg;
  long LastCalls = -1, LastInvalidations = -1;
  for (;;) {
    sleep(5);
    const long Calls = atomic_load(&MtCalls), Invalidations = atomic_load(&MtInvalidations);
    if (Calls == LastCalls && Invalidations == LastInvalidations) {
      printf("  FAIL: no progress for 5 seconds (hang): %ld system calls, %ld invalidations so far\n", Calls, Invalidations);
      fflush(stdout);
      _exit(3);
    }
    LastCalls = Calls;
    LastInvalidations = Invalidations;
  }
  return NULL;
}

static void TestConcurrentInvalidation(void) {
  enum { Threads = 8, MinimumIterations = 12, BudgetSeconds = 20 };
  MtCode = MapGuestMemory(0x10000);
  /* mov ebx,20000 ; loop: mov eax,1 ; syscall ; dec ebx ; jnz loop ; hlt */
  const uint8_t Program[] = {0xBB, 0x20, 0x4E, 0, 0, 0xB8, 1, 0, 0, 0, 0x0F, 0x05, 0xFF, 0xCB, 0x75, 0xF5, 0xF4};
  memcpy(MtCode, Program, sizeof(Program));
  fexmac_install_fault_handlers();
  pthread_t Watchdog;
  pthread_create(&Watchdog, NULL, ProgressWatchdog, NULL);
  const time_t Start = time(NULL);
  int Iteration = 0;
  for (; Iteration < MinimumIterations || time(NULL) - Start < BudgetSeconds; ++Iteration) {
    MtContext = CreateContext(InvalidatingSyscall, NULL);
    atomic_store(&MtDone, 0);
    pthread_t Workers[Threads];
    for (int i = 0; i < Threads; ++i) {
      pthread_create(&Workers[i], NULL, InvalidatingWorker, NULL);
    }
    while (atomic_load(&MtDone) < Threads) {
      fexmac_invalidate(MtContext, (uint64_t)(uintptr_t)MtCode, 0x40);
      atomic_fetch_add(&MtInvalidations, 1);
    }
    for (int i = 0; i < Threads; ++i) {
      pthread_join(Workers[i], NULL);
    }
    if (fexmac_context_destroy(MtContext) != FEXMAC_OK) {
      Shared[2] = 1;
    }
  }
  Check(Shared[2] == 0, "%d runs of %d threads, each invalidating in every system call while the main thread invalidates", Iteration,
        Threads);
  printf("  info: %ld system calls, %ld invalidations from the main thread, %ld seconds\n", atomic_load(&MtCalls),
         atomic_load(&MtInvalidations), (long)(time(NULL) - Start));
}

/* ---- concurrency: destroy racing with use ---- */

struct RacePair {
  fexmac_thread* Thread;
  long Runs;
  long BusyDestroys;
  int RunnerEnd;    /* result that ended the runner */
  int DestroyEnd;   /* result that ended the destroyer */
  int Unexpected;
};

static void* RaceRunner(void* Arg) {
  struct RacePair* Pair = Arg;
  for (;;) {
    fexmac_stop Stop;
    fexmac_stop_init(&Stop);
    const fexmac_result Result = fexmac_thread_run(Pair->Thread, &Stop);
    if (Result == FEXMAC_OK) {
      ++Pair->Runs;
      continue;
    }
    if (Result != FEXMAC_ERROR_INVALID_HANDLE) {
      Pair->Unexpected = 1;
    }
    Pair->RunnerEnd = Result;
    return NULL;
  }
}

static void* RaceDestroyer(void* Arg) {
  struct RacePair* Pair = Arg;
  usleep(2000);
  for (;;) {
    const fexmac_result Result = fexmac_thread_destroy(Pair->Thread);
    if (Result == FEXMAC_ERROR_BUSY) {
      ++Pair->BusyDestroys;
      continue;
    }
    if (Result != FEXMAC_OK) {
      Pair->Unexpected = 1;
    }
    Pair->DestroyEnd = Result;
    return NULL;
  }
}

static fexmac_context* HammerContext;
static atomic_int HammerStop;
static atomic_int HammerUnexpected;

static void* ContextHammer(void* Arg) {
  const int Kind = (int)(intptr_t)Arg;
  while (!atomic_load(&HammerStop)) {
    fexmac_result Result;
    if (Kind == 0) {
      uint32_t Regs[4];
      Result = fexmac_cpuid(HammerContext, 1, 0, Regs);
    } else if (Kind == 1) {
      Result = fexmac_invalidate(HammerContext, 0x100000000ULL, 0x1000);
    } else {
      fexmac_thread* Thread = NULL;
      Result = fexmac_thread_create(HammerContext, &Thread);
      if (Result == FEXMAC_OK && fexmac_thread_destroy(Thread) != FEXMAC_OK) {
        atomic_store(&HammerUnexpected, 1);
      }
    }
    if (Result == FEXMAC_ERROR_INVALID_HANDLE) {
      return NULL; /* the context was destroyed */
    }
    if (Result != FEXMAC_OK) {
      atomic_store(&HammerUnexpected, 1);
    }
  }
  return NULL;
}

static void TestLifetimeRaces(void) {
  enum { Pairs = 6, Hammers = 6 };
  uint8_t* Code = MapGuestMemory(0x10000);
  /* mov ecx,2000 ; loop: dec ecx ; jnz loop ; hlt */
  const uint8_t Program[] = {0xB9, 0xD0, 0x07, 0, 0, 0xFF, 0xC9, 0x75, 0xFC, 0xF4};
  memcpy(Code, Program, sizeof(Program));
  fexmac_install_fault_handlers();
  fexmac_context* Context = CreateContext(NULL, NULL);

  /* Runs racing with destroys. Every thread is created first, so no handle memory is
   * reused while the race runs. */
  struct RacePair PairState[Pairs];
  memset(PairState, 0, sizeof(PairState));
  pthread_t Runners[Pairs], Destroyers[Pairs];
  for (int i = 0; i < Pairs; ++i) {
    PairState[i].Thread = CreateThreadAt(Context, (uint64_t)(uintptr_t)Code, 0);
  }
  for (int i = 0; i < Pairs; ++i) {
    pthread_create(&Runners[i], NULL, RaceRunner, &PairState[i]);
    pthread_create(&Destroyers[i], NULL, RaceDestroyer, &PairState[i]);
  }
  int Bad = 0;
  long Runs = 0, Busy = 0;
  for (int i = 0; i < Pairs; ++i) {
    pthread_join(Runners[i], NULL);
    pthread_join(Destroyers[i], NULL);
    Bad |= PairState[i].Unexpected || PairState[i].RunnerEnd != FEXMAC_ERROR_INVALID_HANDLE || PairState[i].DestroyEnd != FEXMAC_OK;
    Runs += PairState[i].Runs;
    Busy += PairState[i].BusyDestroys;
  }
  Check(!Bad, "each destroy waited for the run in progress, and the run after it found the thread gone (%ld runs, %ld busy destroys)", Runs,
        Busy);

  /* Context destroy racing with calls that use the context. */
  HammerContext = Context;
  pthread_t HammerThreads[Hammers];
  for (int i = 0; i < Hammers; ++i) {
    pthread_create(&HammerThreads[i], NULL, ContextHammer, (void*)(intptr_t)(i % 3));
  }
  usleep(20000);
  long BusyContext = 0;
  fexmac_result Result;
  while ((Result = fexmac_context_destroy(Context)) == FEXMAC_ERROR_BUSY) {
    ++BusyContext;
  }
  atomic_store(&HammerStop, 1);
  for (int i = 0; i < Hammers; ++i) {
    pthread_join(HammerThreads[i], NULL);
  }
  CheckResult(Result, FEXMAC_OK, "context destroy, once no call was using it");
  Check(!atomic_load(&HammerUnexpected), "calls racing with the destroy got success or a dead handle, nothing else (%ld busy destroys)",
        BusyContext);
}

/* ---- code cache: on and off, flush, counters, cold and warm, refusal ---- */

/* A loop of a few blocks that halts with rax = 5050. */
/* clang-format off */
static const uint8_t CacheProgram[] = {
  0x31, 0xC0,                   /*  0: xor eax, eax */
  0xB9, 0x64, 0x00, 0x00, 0x00, /*  2: mov ecx, 100 */
  0x01, 0xC8,                   /*  7: add eax, ecx */
  0xFF, 0xC9,                   /*  9: dec ecx */
  0x75, 0xFA,                   /* 11: jnz 7 */
  0x48, 0x8D, 0x1D, 0x00, 0x00, 0x00, 0x00, /* 13: lea rbx, [rip] */
  0xF4,                         /* 20: hlt */
};
/* clang-format on */

static char CacheDir[64];

static void MakeCacheDir(void) {
  strcpy(CacheDir, "/tmp/fexmac-cache-XXXXXX");
  if (!mkdtemp(CacheDir)) {
    fprintf(stderr, "mkdtemp failed\n");
    exit(2);
  }
}

static void RemoveCacheDir(void) {
  char Command[128];
  snprintf(Command, sizeof(Command), "chmod -RN %s 2>/dev/null; rm -rf %s", CacheDir, CacheDir);
  (void)!system(Command);
}

static fexmac_context* CreateCacheContext(const char* Directory) {
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.code_cache_enabled = 1;
  Config.code_cache_directory = Directory;
  Config.log = QuietLog;
  fexmac_context* Context = NULL;
  const fexmac_result Result = fexmac_context_create(&Config, &Context);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "fexmac_context_create: %s\n", fexmac_result_string(Result));
    exit(2);
  }
  return Context;
}

static fexmac_translation_counters Counters(fexmac_context* Context) {
  fexmac_translation_counters C;
  fexmac_translation_counters_init(&C);
  CheckResult(fexmac_context_get_counters(Context, &C), FEXMAC_OK, "fexmac_context_get_counters");
  return C;
}

/* Maps the program (at a different address each time), registers it, runs it to its hlt
 * and checks the answer. */
static void RunCacheProgram(fexmac_context* Context, const char* What) {
  uint8_t* Code = MapGuestMemory(0x4000);
  memcpy(Code, CacheProgram, sizeof(CacheProgram));
  const uint64_t Base = (uint64_t)(uintptr_t)Code;
  CheckResult(fexmac_memory_map(Context, Base, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_EXEC), FEXMAC_OK, "registering the guest code");
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);
  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 5050 && State.gpr[RBX] == Base + 20, "%s: right answer (rax %" PRIu64 ")",
        What, State.gpr[RAX]);
  fexmac_thread_destroy(Thread);
  fexmac_memory_unmap(Context, Base, 0x4000);
  munmap(Code, 0x4000);
}

static void TestCacheOff(void) {
  fexmac_context* Context = CreateContext(NULL, NULL);
  RunCacheProgram(Context, "cache off");
  fexmac_translation_counters C = Counters(Context);
  Check(C.cache_state == FEXMAC_CACHE_OFF && C.blocks_compiled > 0 && C.blocks_loaded == 0 && C.cache_entries_queued == 0,
        "cache off: state OFF, blocks compiled (%" PRIu64 "), nothing loaded or queued", C.blocks_compiled);
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_ERROR_UNSUPPORTED, "flush with the cache off");
  fexmac_context_destroy(Context);
}

static void TestCacheColdWarm(void) {
  MakeCacheDir();
  fexmac_context* Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "cold");
  fexmac_translation_counters C = Counters(Context);
  Check(C.cache_state == FEXMAC_CACHE_ACTIVE && C.blocks_compiled > 0 && C.cache_entries_queued > 0,
        "cold: state ACTIVE, %" PRIu64 " compiled, %" PRIu64 " queued", C.blocks_compiled, C.cache_entries_queued);
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_OK, "flush");
  fexmac_context_destroy(Context);

  /* A new context in the same process loads what the first one saved. */
  Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "warm, same process");
  C = Counters(Context);
  Check(C.blocks_compiled == 0 && C.blocks_loaded > 0 && C.cache_entries_rejected == 0,
        "warm in the same process: nothing compiled, %" PRIu64 " loaded", C.blocks_loaded);
  fexmac_context_destroy(Context);
  RemoveCacheDir();
}

static void TestCacheCrossProcess(void) {
  MakeCacheDir();
  /* The cold run in a child process, before this one has any context. */
  const pid_t Child = fork();
  if (Child == 0) {
    fexmac_context* Context = CreateCacheContext(CacheDir);
    RunCacheProgram(Context, "cold, in a child process");
    const int Ok = fexmac_context_flush_cache(Context) == FEXMAC_OK && Counters(Context).cache_entries_queued > 0;
    fexmac_context_destroy(Context);
    _exit(Ok && Failures == 0 ? 0 : 1);
  }
  int Status = 0;
  waitpid(Child, &Status, 0);
  Check(WIFEXITED(Status) && WEXITSTATUS(Status) == 0, "child process filled the cache");
  fexmac_context* Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "warm, another process");
  fexmac_translation_counters C = Counters(Context);
  Check(C.blocks_compiled == 0 && C.blocks_loaded > 0, "warm in another process: nothing compiled, %" PRIu64 " loaded", C.blocks_loaded);
  fexmac_context_destroy(Context);
  RemoveCacheDir();
}

static void TestCacheUntrusted(void) {
  MakeCacheDir();
  chmod(CacheDir, 0777);
  fexmac_context* Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "untrusted directory");
  fexmac_translation_counters C = Counters(Context);
  Check(C.cache_state == FEXMAC_CACHE_UNTRUSTED && C.blocks_loaded == 0 && C.cache_entries_queued == 0,
        "a directory writable by others: state UNTRUSTED, nothing loaded or queued");
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_ERROR_UNSUPPORTED, "flush with the cache refused");
  struct stat Info;
  Check(stat(CacheDir, &Info) == 0 && (Info.st_mode & 0777) == 0777, "and the directory was left as it was");
  fexmac_context_destroy(Context);
  chmod(CacheDir, 0700);
  RemoveCacheDir();
}

/* A cache file others may write is removed and started again: the cache stays usable
 * (ACTIVE) and the replacement is counted. */
static void TestCacheUntrustedFile(void) {
  MakeCacheDir();
  fexmac_context* Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "cold");
  fexmac_context_flush_cache(Context);
  fexmac_context_destroy(Context);
  char Command[256];
  snprintf(Command, sizeof(Command), "chmod 666 %s/*/RWCacheDB_*_idx.foz", CacheDir);
  (void)!system(Command);
  Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "after the file was opened up");
  fexmac_translation_counters C = Counters(Context);
  Check(C.cache_state == FEXMAC_CACHE_ACTIVE && C.untrusted_files_replaced == 1 && C.blocks_loaded == 0,
        "an index file writable by others: replaced (counted %u), state ACTIVE, nothing loaded from it", C.untrusted_files_replaced);
  fexmac_context_destroy(Context);
  RemoveCacheDir();
}

/* Not a matter of trust: a directory that cannot be written, and a database another
 * process keeps locked, give UNAVAILABLE, and the context runs. */
static void TestCacheUnavailable(void) {
  MakeCacheDir();
  chmod(CacheDir, 0500);
  fexmac_context* Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "read-only directory");
  fexmac_translation_counters C = Counters(Context);
  Check(C.cache_state == FEXMAC_CACHE_UNAVAILABLE, "a directory that cannot be written: state UNAVAILABLE (%d)", C.cache_state);
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_ERROR_UNSUPPORTED, "flush with the cache unavailable");
  fexmac_context_destroy(Context);
  chmod(CacheDir, 0700);

  /* Fill it, then hold its locks from another process while a new context opens it. */
  Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "cold");
  fexmac_context_flush_cache(Context);
  fexmac_context_destroy(Context);
  int Ready[2];
  if (pipe(Ready) != 0) {
    exit(2);
  }
  const pid_t Holder = fork();
  if (Holder == 0) {
    char Command[512];
    snprintf(Command, sizeof(Command),
             "exec python3 -c 'import fcntl,glob,os,sys,time\nfds=[os.open(f,os.O_RDONLY) for f in glob.glob(\"%s/*/*.foz\")]\n"
             "[fcntl.flock(fd,fcntl.LOCK_EX) for fd in fds]\nos.write(%d,b\"x\")\ntime.sleep(60)'",
             CacheDir, Ready[1]);
    execl("/bin/sh", "sh", "-c", Command, (char*)NULL);
    _exit(1);
  }
  char Byte;
  (void)!read(Ready[0], &Byte, 1);
  Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "database locked by another process");
  C = Counters(Context);
  Check(C.cache_state == FEXMAC_CACHE_UNAVAILABLE, "a database another process keeps locked: state UNAVAILABLE (%d)", C.cache_state);
  fexmac_context_destroy(Context);
  kill(Holder, SIGKILL);
  waitpid(Holder, NULL, 0);
  RemoveCacheDir();
}

static void TestCacheOptions(void) {
  fexmac_context* Context = NULL;
  CheckResult(CreateWithOption("DISKCACHE", "1", &Context), FEXMAC_ERROR_BAD_CONFIG, "the core's DISKCACHE option");
  CheckResult(CreateWithOption("DISKCACHEPATH", "/tmp", &Context), FEXMAC_ERROR_BAD_CONFIG, "the core's DISKCACHEPATH option");
  CheckResult(CreateWithOption("DISKCACHEMAXFILESIZE", "0", &Context), FEXMAC_ERROR_BAD_CONFIG, "a cache size limit of 0");
  CheckResult(CreateWithOption("DISKCACHEMAXFILESIZE", "1048576", &Context), FEXMAC_OK, "a cache size limit");
  fexmac_context_destroy(Context);
  CheckResult(CreateWithOption("DISKCACHEVALIDATION", "1", &Context), FEXMAC_OK, "cache validation");
  fexmac_context_destroy(Context);
}

static void TestCacheFork(void) {
  MakeCacheDir();
  fexmac_context* Context = CreateCacheContext(CacheDir);
  RunCacheProgram(Context, "before the fork");
  CheckResult(fexmac_before_fork(Context), FEXMAC_OK, "fexmac_before_fork");
  CheckResult(fexmac_before_fork(Context), FEXMAC_ERROR_BUSY, "a second fexmac_before_fork");
  const pid_t Child = fork();
  if (Child == 0) {
    fexmac_after_fork(Context, 1);
    RunCacheProgram(Context, "in the child");
    const fexmac_translation_counters C = Counters(Context);
    const int Ok = C.cache_state == FEXMAC_CACHE_READ_ONLY && fexmac_context_flush_cache(Context) == FEXMAC_ERROR_UNSUPPORTED;
    _exit(Ok && Failures == 0 ? 0 : 1);
  }
  CheckResult(fexmac_after_fork(Context, 0), FEXMAC_OK, "fexmac_after_fork in the parent");
  int Status = 0;
  waitpid(Child, &Status, 0);
  Check(WIFEXITED(Status) && WEXITSTATUS(Status) == 0, "child ran guest code, read-only cache, flush refused");
  RunCacheProgram(Context, "parent after the fork");
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_OK, "parent still flushes");
  fexmac_context_destroy(Context);
  RemoveCacheDir();
}

/* ---- code ranges: only registered memory is code once any range is registered ---- */

static void TestCodeRanges(void) {
  fexmac_install_fault_handlers();
  uint8_t* Code = MapGuestMemory(0x8000);
  const uint64_t Base = (uint64_t)(uintptr_t)Code;
  /* First page: mov eax, 1; jmp to the second page. Second page: mov eax, 2; hlt. */
  const uint8_t First[] = {0xB8, 0x01, 0, 0, 0, 0xE9, 0xF6, 0x3F, 0, 0};
  const uint8_t Second[] = {0xB8, 0x02, 0, 0, 0, 0xF4};
  memcpy(Code, First, sizeof(First));
  memcpy(Code + 0x4000, Second, sizeof(Second));
  fexmac_context* Context = CreateContext(NULL, NULL);

  CheckResult(fexmac_memory_map(Context, Base, 0, FEXMAC_PROT_READ | FEXMAC_PROT_EXEC), FEXMAC_ERROR_INVALID_ARGUMENT, "an empty range");
  CheckResult(fexmac_memory_map(Context, UINT64_MAX - 0xFFF, 0x2000, FEXMAC_PROT_READ), FEXMAC_ERROR_INVALID_ARGUMENT, "a range that wraps");
  CheckResult(fexmac_memory_map(Context, Base, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_EXEC), FEXMAC_OK, "registering the first page only");
  fexmac_thread* Thread = CreateThreadAt(Context, Base, 0);
  fexmac_stop Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.instruction_address == Base + 0x4000 && Stop.si_code == SEGV_MAPERR &&
          Stop.error_code == (FEXMAC_PF_USER | FEXMAC_PF_INSTRUCTION),
        "running into unregistered memory stops with a fetch fault there, as for an unmapped page (reason %d at 0x%" PRIx64 ", si_code %d, error 0x%x)",
        Stop.reason, Stop.instruction_address, Stop.si_code, Stop.error_code);

  CheckResult(fexmac_memory_map(Context, Base + 0x4000, 0x4000, FEXMAC_PROT_READ), FEXMAC_OK, "registering the second page, not executable");
  {
    fexmac_x86_state Again = GetState(Thread);
    Again.rip = Base;
    SetState(Thread, &Again);
    Stop = Run(Thread);
    Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.si_code == SEGV_ACCERR &&
            Stop.error_code == (FEXMAC_PF_PRESENT | FEXMAC_PF_USER | FEXMAC_PF_INSTRUCTION),
          "a mapped page that is not executable: SEGV_ACCERR with the present bit (si_code %d, error 0x%x)", Stop.si_code, Stop.error_code);
  }
  CheckResult(fexmac_memory_protect(Context, Base + 0x4000, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_EXEC), FEXMAC_OK, "making it executable");
  fexmac_x86_state State = GetState(Thread);
  State.rip = Base;
  SetState(Thread, &State);
  Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 2, "with both pages registered the program runs to its hlt");

  CheckResult(fexmac_memory_unmap(Context, Base + 0x4000, 0x4000), FEXMAC_OK, "removing the second page");
  State.rip = Base;
  SetState(Thread, &State);
  Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT, "after removing it (and its translations) it faults again");
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* ---- concurrency: locked operations on words that cross 16 bytes ---- */

/* ARM64 atomics fault on these; the library hands them to the core, which emulates them
 * with two compare-and-swaps under its process-wide split-lock spin lock. Eight threads
 * hammer one word at once and the total must be exact. The increment, 0x100000001,
 * changes both halves of the word every time, so that the two compare-and-swaps of one
 * emulation can interleave with another's: without the lock (the core's default elsewhere)
 * the counts come out short. The xchg lock word only ever changes its lower half, as a
 * real lock's 0/1 does, so a release store landing between the two compare-and-swaps must
 * be retried rather than reported as a tear. */

/* clang-format off */
static const uint8_t SplitXaddProgram[] = {
  0x4C, 0x89, 0xC0,                         /*  0: mov rax, r8 */
  0xF0, 0x48, 0x0F, 0xC1, 0x07,             /*  3: lock xadd [rdi], rax */
  0x48, 0xFF, 0xC9,                         /*  8: dec rcx */
  0x75, 0xF3,                               /* 11: jnz 0 */
  0xF4,                                     /* 13: hlt */
};
static const uint8_t SplitCmpxchgProgram[] = {
  0x48, 0x8B, 0x07,                         /*  0: mov rax, [rdi] */
  0x4A, 0x8D, 0x14, 0x00,                   /*  3: lea rdx, [rax + r8] */
  0xF0, 0x48, 0x0F, 0xB1, 0x17,             /*  7: lock cmpxchg [rdi], rdx */
  0x75, 0xF2,                               /* 12: jnz 0 (lost the race: retry) */
  0x48, 0xFF, 0xC9,                         /* 14: dec rcx */
  0x75, 0xED,                               /* 17: jnz 0 */
  0xF4,                                     /* 19: hlt */
};
/* A spin lock taken with xchg on a word crossing 16 bytes and released with a plain
 * store, as x86 code does, guarding a plain increment of an aligned counter. */
static const uint8_t SplitXchgLockProgram[] = {
  0xB8, 0x01, 0x00, 0x00, 0x00,             /*  0: mov eax, 1 */
  0x48, 0x87, 0x06,                         /*  5: xchg [rsi], rax */
  0x48, 0x85, 0xC0,                         /*  8: test rax, rax */
  0x75, 0xF3,                               /* 11: jnz 0 (lock taken: spin) */
  0x48, 0xFF, 0x07,                         /* 13: inc qword [rdi] */
  0x48, 0xC7, 0x06, 0x00, 0x00, 0x00, 0x00, /* 16: mov qword [rsi], 0 */
  0x48, 0xFF, 0xC9,                         /* 23: dec rcx */
  0x75, 0xE4,                               /* 26: jnz 0 */
  0xF4,                                     /* 28: hlt */
};
/* clang-format on */

enum { SplitThreads = 8, SplitIterations = 30000 };

static fexmac_context* SplitContext;
static uint64_t SplitCode, SplitRdi, SplitRsi;
static const uint64_t SplitIncrement = 0x100000001ULL;

static void* SplitWorker(void* Arg) {
  (void)Arg;
  fexmac_thread* Thread = CreateThreadAt(SplitContext, SplitCode, 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = SplitRdi;
  State.gpr[RSI] = SplitRsi;
  State.gpr[RCX] = SplitIterations;
  State.gpr[8] = SplitIncrement; /* r8 */
  SetState(Thread, &State);
  const fexmac_stop Stop = Run(Thread);
  if (Stop.reason != FEXMAC_STOP_HALT) {
    Shared[2] = 1;
  }
  fexmac_thread_destroy(Thread);
  return NULL;
}

static uint64_t RunSplit(const uint8_t* Program, size_t Size, uint8_t* Rdi, uint8_t* Rsi) {
  uint8_t* Code = MapGuestMemory(0x10000);
  memcpy(Code, Program, Size);
  SplitCode = (uint64_t)(uintptr_t)Code;
  SplitRdi = (uint64_t)(uintptr_t)Rdi;
  SplitRsi = (uint64_t)(uintptr_t)Rsi;
  pthread_t Workers[SplitThreads];
  for (int i = 0; i < SplitThreads; ++i) {
    pthread_create(&Workers[i], NULL, SplitWorker, NULL);
  }
  for (int i = 0; i < SplitThreads; ++i) {
    pthread_join(Workers[i], NULL);
  }
  uint64_t Value;
  memcpy(&Value, Rdi, sizeof(Value));
  return Value;
}

static void TestSplitLocks(void) {
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers");
  SplitContext = CreateContext(NULL, NULL);
  const uint64_t Want = (uint64_t)SplitThreads * SplitIterations * SplitIncrement;
  /* Two 16K pages, so that a word can cross the page boundary. */
  uint8_t* Data = MapGuestMemory(0x8000);
  struct {
    const char* What;
    size_t Offset;
  } Xadds[] = {{"across 16 bytes", 0x100 + 12}, {"across a 128-byte line", 0x200 + 124}, {"across the 16K page", 0x4000 - 4}};
  for (size_t i = 0; i < sizeof(Xadds) / sizeof(Xadds[0]); ++i) {
    const uint64_t Got = RunSplit(SplitXaddProgram, sizeof(SplitXaddProgram), Data + Xadds[i].Offset, NULL);
    Check(Got == Want, "lock xadd %s from %d threads: %" PRIu64 " of %" PRIu64, Xadds[i].What, SplitThreads, Got, Want);
  }
  {
    const uint64_t Got = RunSplit(SplitCmpxchgProgram, sizeof(SplitCmpxchgProgram), Data + 0x1000 + 12, NULL);
    Check(Got == Want, "lock cmpxchg loop across 16 bytes from %d threads: %" PRIu64 " of %" PRIu64, SplitThreads, Got, Want);
  }
  {
    const uint64_t Got = RunSplit(SplitXchgLockProgram, sizeof(SplitXchgLockProgram), Data + 0x2000, Data + 0x2400 + 12);
    const uint64_t WantLocked = (uint64_t)SplitThreads * SplitIterations;
    Check(Got == WantLocked, "xchg spin lock across 16 bytes from %d threads: %" PRIu64 " of %" PRIu64, SplitThreads, Got, WantLocked);
  }
  Check(Shared[2] == 0, "every thread ran to its halt");
  CheckResult(fexmac_context_destroy(SplitContext), FEXMAC_OK, "context destroy");
}

/* ---- every emulated split form, with plain stores to the neighbouring bytes ---- */

/* Each form runs `op` in a loop (counter r10; the xadd forms copy r8 to rax first, since
 * xadd returns the old value in its register) from 8 threads on one operand that crosses
 * 16 bytes, with operands that change both halves where the operation allows it. A host
 * thread meanwhile writes the bytes that share the operand's compare-and-swap units but
 * are not part of it (neighbouring data, written with plain stores, as a real program's
 * neighbouring fields would be). The core then sees its second compare-and-swap fail
 * because of bytes that are not its own; it must complete or retry the operation, never
 * drop it or apply it twice. The final values are exact. */

enum { FormIterations = 10000, FormMaxCode = 64 };

struct Form {
  const char* What;
  uint8_t Code[FormMaxCode];
  size_t CodeSize; /* 0: the op bytes below, wrapped in the r10 loop */
  uint8_t Op[16];
  size_t OpSize;
  size_t Offset;      /* operand offset in the data area */
  size_t OperandSize; /* bytes */
  size_t Unit;        /* compare-and-swap unit of the emulation: 1, 4 or 8 */
  size_t CounterOffset; /* for the lock forms: the counter the lock guards, else 0 */
  uint64_t Initial;
  uint64_t (*Want)(uint64_t Initial, uint64_t Total);
  void (*Registers)(int Index, uint64_t* R8, uint64_t* R9);
};

static uint64_t WantAdd(uint64_t I, uint64_t N) { return I + N * 0x0000000100000001ULL; }
static uint64_t WantSub(uint64_t I, uint64_t N) { return I - N * 0x0000000100000001ULL; }
static uint64_t WantSame(uint64_t I, uint64_t N) { (void)N; return I; }
static uint64_t WantInc(uint64_t I, uint64_t N) { return I + N; }
static uint64_t WantDec(uint64_t I, uint64_t N) { return I - N; }
static uint64_t WantAdd16(uint64_t I, uint64_t N) { return (uint16_t)(I + N * 0x0101); }
static uint64_t WantAdd32(uint64_t I, uint64_t N) { return (uint32_t)(I + N * 0x00010001); }
static uint64_t WantCounter(uint64_t I, uint64_t N) { (void)I; return N; }

static void RegsIncrement(int Index, uint64_t* R8, uint64_t* R9) { (void)Index; *R8 = 0x0000000100000001ULL; *R9 = 0; }
static void RegsInc16(int Index, uint64_t* R8, uint64_t* R9) { (void)Index; *R8 = 0x0101; *R9 = 0; }
static void RegsInc32(int Index, uint64_t* R8, uint64_t* R9) { (void)Index; *R8 = 0x00010001; *R9 = 0; }
static void RegsXor(int Index, uint64_t* R8, uint64_t* R9) { (void)Index; *R8 = 0x5A5A5A5AA5A5A5A5ULL; *R9 = 0; }
/* or and and: each thread sets and clears its own bit in each half. */
static void RegsBits(int Index, uint64_t* R8, uint64_t* R9) {
  *R8 = (1ULL << Index) | (1ULL << (Index + 32));
  *R9 = ~*R8;
}

/* clang-format off */
static const struct Form Forms[] = {
  {"lock xadd 16-bit across 16 bytes", {0}, 0, {0x4C, 0x89, 0xC0, 0x66, 0xF0, 0x0F, 0xC1, 0x07}, 8, 0x100 + 15, 2, 1, 0, 0, WantAdd16, RegsInc16},
  {"lock xadd 32-bit across 16 bytes", {0}, 0, {0x4C, 0x89, 0xC0, 0xF0, 0x0F, 0xC1, 0x07}, 7, 0x200 + 14, 4, 4, 0, 0, WantAdd32, RegsInc32},
  {"lock xadd 64-bit across 16 bytes", {0}, 0, {0x4C, 0x89, 0xC0, 0xF0, 0x48, 0x0F, 0xC1, 0x07}, 8, 0x280 + 12, 8, 8, 0, 0, WantAdd, RegsIncrement},
  {"lock add", {0}, 0, {0xF0, 0x4C, 0x01, 0x07}, 4, 0x300 + 12, 8, 8, 0, 0, WantAdd, RegsIncrement},
  {"lock sub", {0}, 0, {0xF0, 0x4C, 0x29, 0x07}, 4, 0x400 + 12, 8, 8, 0, 0, WantSub, RegsIncrement},
  {"lock xor (even count)", {0}, 0, {0xF0, 0x4C, 0x31, 0x07}, 4, 0x500 + 12, 8, 8, 0, 0x1122334455667788ULL, WantSame, RegsXor},
  {"lock inc (one byte below the boundary)", {0}, 0, {0xF0, 0x48, 0xFF, 0x07}, 4, 0x600 + 15, 8, 8, 0, 0, WantInc, RegsIncrement},
  {"lock dec (one byte below the boundary)", {0}, 0, {0xF0, 0x48, 0xFF, 0x0F}, 4, 0x700 + 15, 8, 8, 0, 0, WantDec, RegsIncrement},
  {"lock not (even count)", {0}, 0, {0xF0, 0x48, 0xF7, 0x17}, 4, 0x800 + 12, 8, 8, 0, 0x0123456789ABCDEFULL, WantSame, RegsIncrement},
  {"lock neg (even count)", {0}, 0, {0xF0, 0x48, 0xF7, 0x1F}, 4, 0x900 + 12, 8, 8, 0, 0x0123456789ABCDEFULL, WantSame, RegsIncrement},
  {"lock or then lock and of each thread's bits", {0}, 0, {0xF0, 0x4C, 0x09, 0x07, 0xF0, 0x4C, 0x21, 0x0F}, 8, 0xA00 + 12, 8, 8, 0, 0, WantSame, RegsBits},
  {"lock cmpxchg8b increment loop", {
     0x8B, 0x07,                   /*  0: mov eax, [rdi] */
     0x8B, 0x57, 0x04,             /*  2: mov edx, [rdi + 4] */
     0x8D, 0x58, 0x01,             /*  5: lea ebx, [rax + 1] */
     0x8D, 0x4A, 0x01,             /*  8: lea ecx, [rdx + 1] */
     0xF0, 0x0F, 0xC7, 0x0F,       /* 11: lock cmpxchg8b [rdi] */
     0x75, 0xEF,                   /* 15: jnz 0 */
     0x49, 0xFF, 0xCA,             /* 17: dec r10 */
     0x75, 0xEA,                   /* 20: jnz 0 */
     0xF4},                        /* 22: hlt */
   23, {0}, 0, 0xB00 + 12, 8, 8, 0, 0, WantAdd, RegsIncrement},
  {"cmpxchg8b spin lock, plain release", {
     0x31, 0xC0,                   /*  0: xor eax, eax */
     0x31, 0xD2,                   /*  2: xor edx, edx */
     0xBB, 0x01, 0x00, 0x00, 0x00, /*  4: mov ebx, 1 */
     0x31, 0xC9,                   /*  9: xor ecx, ecx */
     0xF0, 0x0F, 0xC7, 0x0F,       /* 11: lock cmpxchg8b [rdi] */
     0x75, 0xEF,                   /* 15: jnz 0 */
     0x48, 0xFF, 0x06,             /* 17: inc qword [rsi] */
     0x48, 0xC7, 0x07, 0, 0, 0, 0, /* 20: mov qword [rdi], 0 */
     0x49, 0xFF, 0xCA,             /* 27: dec r10 */
     0x75, 0xE0,                   /* 30: jnz 0 */
     0xF4},                        /* 32: hlt */
   33, {0}, 0, 0xC00 + 12, 8, 8, 0xD00, 0, WantCounter, RegsIncrement},
  {"xchg spin lock changing both halves, xchg release", {
     0x4C, 0x89, 0xC0,             /*  0: mov rax, r8 */
     0x48, 0x87, 0x07,             /*  3: xchg [rdi], rax */
     0x48, 0x85, 0xC0,             /*  6: test rax, rax */
     0x75, 0xF5,                   /*  9: jnz 0 */
     0x48, 0xFF, 0x06,             /* 11: inc qword [rsi] */
     0x31, 0xC0,                   /* 14: xor eax, eax */
     0x48, 0x87, 0x07,             /* 16: xchg [rdi], rax */
     0x49, 0xFF, 0xCA,             /* 19: dec r10 */
     0x75, 0xE8,                   /* 22: jnz 0 */
     0xF4},                        /* 24: hlt */
   25, {0}, 0, 0xE00 + 12, 8, 8, 0xF00, 0, WantCounter, RegsIncrement},
};
/* clang-format on */

struct FormWorkerArgs {
  int Index;
  uint64_t Code;
  uint64_t Operand;
  uint64_t Counter;
  const struct Form* F;
};

static void* FormWorker(void* Arg) {
  const struct FormWorkerArgs* A = Arg;
  fexmac_thread* Thread = CreateThreadAt(SplitContext, A->Code, 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = A->Operand;
  State.gpr[RSI] = A->Counter;
  State.gpr[10] = FormIterations;
  A->F->Registers(A->Index, &State.gpr[8], &State.gpr[9]);
  SetState(Thread, &State);
  const fexmac_stop Stop = Run(Thread);
  if (Stop.reason != FEXMAC_STOP_HALT) {
    Shared[2] = 1;
  }
  fexmac_thread_destroy(Thread);
  return NULL;
}

/* Plain stores to the bytes that share the operand's compare-and-swap units, spaced out
 * so that most emulations still succeed at once. */
static volatile uint8_t* NeighbourBytes[16];
static int NeighbourCount;
static atomic_int NeighbourStop;
static atomic_long NeighbourStores;

static void* NeighbourWriter(void* Arg) {
  (void)Arg;
  uint8_t Value = 1;
  while (!atomic_load(&NeighbourStop)) {
    for (int i = 0; i < NeighbourCount; ++i) {
      NeighbourBytes[i][0] = Value;
    }
    ++Value;
    atomic_fetch_add(&NeighbourStores, 1);
    for (volatile int Spin = 0; Spin < 300; ++Spin) {
    }
  }
  return NULL;
}

static void TestSplitLockForms(void) {
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers");
  SplitContext = CreateContext(NULL, NULL);
  uint8_t* Data = MapGuestMemory(0x8000);
  const uint64_t Total = (uint64_t)SplitThreads * FormIterations;
  for (size_t f = 0; f < sizeof(Forms) / sizeof(Forms[0]); ++f) {
    const struct Form* F = &Forms[f];
    uint8_t* Code = MapGuestMemory(0x10000);
    if (F->CodeSize) {
      memcpy(Code, F->Code, F->CodeSize);
    } else {
      /* op; dec r10; jnz op; hlt */
      memcpy(Code, F->Op, F->OpSize);
      const uint8_t Tail[] = {0x49, 0xFF, 0xCA, 0x75, (uint8_t)(-(int)(F->OpSize + 5)), 0xF4};
      memcpy(Code + F->OpSize, Tail, sizeof(Tail));
    }
    uint8_t* Operand = Data + F->Offset;
    memcpy(Operand, &F->Initial, F->OperandSize);

    NeighbourCount = 0;
    const size_t UnitBegin = F->Offset & ~(F->Unit - 1);
    const size_t UnitEnd = (F->Offset + F->OperandSize + F->Unit - 1) & ~(F->Unit - 1);
    for (size_t b = UnitBegin; b < UnitEnd; ++b) {
      if ((b < F->Offset || b >= F->Offset + F->OperandSize) && NeighbourCount < 16) {
        NeighbourBytes[NeighbourCount++] = Data + b;
      }
    }
    atomic_store(&NeighbourStop, 0);
    atomic_store(&NeighbourStores, 0);
    pthread_t Writer;
    pthread_create(&Writer, NULL, NeighbourWriter, NULL);

    struct FormWorkerArgs Args[SplitThreads];
    pthread_t Workers[SplitThreads];
    for (int i = 0; i < SplitThreads; ++i) {
      Args[i] = (struct FormWorkerArgs) {i, (uint64_t)(uintptr_t)Code, (uint64_t)(uintptr_t)Operand,
                                         F->CounterOffset ? (uint64_t)(uintptr_t)(Data + F->CounterOffset) : 0, F};
      pthread_create(&Workers[i], NULL, FormWorker, &Args[i]);
    }
    for (int i = 0; i < SplitThreads; ++i) {
      pthread_join(Workers[i], NULL);
    }
    atomic_store(&NeighbourStop, 1);
    pthread_join(Writer, NULL);

    uint64_t Got = 0;
    if (F->CounterOffset) {
      memcpy(&Got, Data + F->CounterOffset, 8);
    } else {
      memcpy(&Got, Operand, F->OperandSize);
    }
    const uint64_t Want = F->Want(F->Initial, Total);
    Check(Got == Want, "%s from %d threads, %ld neighbouring stores: 0x%" PRIx64 ", expected 0x%" PRIx64, F->What, SplitThreads,
          atomic_load(&NeighbourStores), Got, Want);
  }
  Check(Shared[2] == 0, "every thread ran to its halt");
  CheckResult(fexmac_context_destroy(SplitContext), FEXMAC_OK, "context destroy");
}

/* ---- split locked operations while asynchronous signals keep arriving ---- */

/* The host owns the fault signals (way 2) with an empty mask, and a signal handler of its
 * own keeps interrupting one guest thread (worker 0) while 8 threads run split lock xadd
 * and a host thread writes the neighbouring bytes (which makes the core's attempts under
 * the split-lock lock longer). When a signal lands while worker 0 is inside its fault
 * handler, the signal handler waits until another thread's operation has changed the
 * shared word. If the interrupted thread held the core's split-lock lock, nothing can
 * change it and the wait times out: fexmac_handle_fault must keep asynchronous signals
 * blocked while the core can hold its locks.
 *
 * The workload runs until a minimum number of signals has landed inside fault handling
 * (or a stall ends it early), not for a fixed number of operations, so a fast or a small
 * machine checks as many landings as a large one. The storm starts once every thread has
 * translated its code, since a thread interrupted while it holds a lock of the translator
 * would stall the others too. */
enum { StormMinimumInside = 3000, StormBudgetSeconds = 90 };
static volatile uint64_t* StormWord;
static volatile uint64_t StormStopFlag; /* read by the guest loop */
static atomic_int StormStopped;
static atomic_long StormSignals, StormInside, StormStalls;
static pthread_t StormTarget;
static volatile sig_atomic_t StormTargetInFault;
static uint64_t StormOperations[SplitThreads];

/* lock xadd [rdi], r8 until [rsi] is set, counting in rcx. */
/* clang-format off */
static const uint8_t StormProgram[] = {
  0x4C, 0x89, 0xC0,             /*  0: mov rax, r8 */
  0xF0, 0x48, 0x0F, 0xC1, 0x07, /*  3: lock xadd [rdi], rax */
  0x48, 0xFF, 0xC1,             /*  8: inc rcx */
  0x48, 0x83, 0x3E, 0x00,       /* 11: cmp qword [rsi], 0 */
  0x74, 0xEF,                   /* 15: je 0 */
  0xF4,                         /* 17: hlt */
};
/* clang-format on */

static void StormFaultHandler(int Signal, siginfo_t* Info, void* Context) {
  const int Target = pthread_equal(pthread_self(), StormTarget);
  if (Target) {
    StormTargetInFault = 1;
  }
  const int Handled = fexmac_handle_fault(Signal, Info, Context);
  if (Target) {
    StormTargetInFault = 0;
  }
  if (!Handled) {
    signal(Signal, SIG_DFL);
  }
}

static void StormHandler(int Signal) {
  (void)Signal;
  if (atomic_load(&StormStopped)) {
    return;
  }
  atomic_fetch_add(&StormSignals, 1);
  if (!StormTargetInFault) {
    return;
  }
  atomic_fetch_add(&StormInside, 1);
  const uint64_t Before = *StormWord;
  for (int Spin = 0; Spin < 20000; ++Spin) { /* at least 20 ms */
    if (*StormWord != Before || atomic_load(&StormStopped)) {
      return;
    }
    usleep(1);
  }
  atomic_fetch_add(&StormStalls, 1);
  atomic_store(&StormStopped, 1);
}

static void* StormWorker(void* Arg) {
  const int Index = (int)(intptr_t)Arg;
  fexmac_thread* Thread = CreateThreadAt(SplitContext, SplitCode, 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = SplitRdi;
  State.gpr[RSI] = (uint64_t)(uintptr_t)&StormStopFlag;
  State.gpr[RCX] = 0;
  State.gpr[8] = SplitIncrement;
  SetState(Thread, &State);
  const fexmac_stop Stop = Run(Thread);
  if (Stop.reason != FEXMAC_STOP_HALT) {
    Shared[2] = 1;
  }
  StormOperations[Index] = GetState(Thread).gpr[RCX];
  fexmac_thread_destroy(Thread);
  return NULL;
}

static void* StormTargetWorker(void* Arg) {
  StormTarget = pthread_self();
  return StormWorker(Arg);
}

static void TestSplitLockSignalStorm(void) {
  InstallHostHandler(StormFaultHandler, SA_NODEFER);
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_handler = StormHandler;
  sigemptyset(&Action.sa_mask);
  sigaction(SIGUSR1, &Action, NULL);

  SplitContext = CreateContext(NULL, NULL);
  uint8_t* Code = MapGuestMemory(0x10000);
  memcpy(Code, StormProgram, sizeof(StormProgram));
  uint8_t* Data = MapGuestMemory(0x8000);
  SplitCode = (uint64_t)(uintptr_t)Code;
  SplitRdi = (uint64_t)(uintptr_t)(Data + 0x100 + 12);
  StormWord = (volatile uint64_t*)(Data + 0x100 + 8); /* aligned, covers the operand's low half */
  /* The neighbouring bytes of the operand's two compare-and-swap units. */
  NeighbourCount = 0;
  for (int b = 8; b < 12; ++b) {
    NeighbourBytes[NeighbourCount++] = Data + 0x100 + b;
  }
  for (int b = 20; b < 24; ++b) {
    NeighbourBytes[NeighbourCount++] = Data + 0x100 + b;
  }

  pthread_t Workers[SplitThreads];
  pthread_create(&Workers[0], NULL, StormTargetWorker, (void*)(intptr_t)0);
  for (int i = 1; i < SplitThreads; ++i) {
    pthread_create(&Workers[i], NULL, StormWorker, (void*)(intptr_t)i);
  }
  /* The operand's low half counts the operations (the increment adds 1 to it). */
  while (!StormTarget || (*(volatile uint64_t*)SplitRdi & 0xFFFFFFFFULL) < 4000) {
    usleep(100);
  }
  atomic_store(&NeighbourStop, 0);
  pthread_t Writer;
  pthread_create(&Writer, NULL, NeighbourWriter, NULL);
  const time_t Start = time(NULL);
  while (!atomic_load(&StormStopped) && atomic_load(&StormInside) < StormMinimumInside && time(NULL) - Start < StormBudgetSeconds) {
    pthread_kill(StormTarget, SIGUSR1);
    usleep(50);
  }
  atomic_store(&StormStopped, 1);
  StormStopFlag = 1;
  for (int i = 0; i < SplitThreads; ++i) {
    pthread_join(Workers[i], NULL);
  }
  atomic_store(&NeighbourStop, 1);
  pthread_join(Writer, NULL);

  uint64_t Operations = 0;
  for (int i = 0; i < SplitThreads; ++i) {
    Operations += StormOperations[i];
  }
  uint64_t Got;
  memcpy(&Got, Data + 0x100 + 12, 8);
  const uint64_t Want = Operations * SplitIncrement;
  Check(Got == Want, "lock xadd across 16 bytes from %d threads under a signal storm: 0x%" PRIx64 ", expected 0x%" PRIx64 " (%" PRIu64
        " operations)", SplitThreads, Got, Want, Operations);
  Check(atomic_load(&StormStalls) == 0, "no signal interrupted a thread holding the split-lock lock (%ld signals, %ld inside fault handling, %ld stalls)",
        atomic_load(&StormSignals), atomic_load(&StormInside), atomic_load(&StormStalls));
  Check(atomic_load(&StormStalls) != 0 || atomic_load(&StormInside) >= StormMinimumInside,
        "at least %d signals landed inside fault handling within %d seconds (%ld)", StormMinimumInside, StormBudgetSeconds,
        atomic_load(&StormInside));
  Check(Shared[2] == 0, "every thread ran to its halt");
  CheckResult(fexmac_context_destroy(SplitContext), FEXMAC_OK, "context destroy");
}

/* ---- driver ---- */


/* ---- the translator's scratch buffer and the code buffers: large blocks ---- */

/* The translator emits each block into a per-thread scratch buffer sized from the
 * block's IR, followed by a guard page; a block whose code is larger runs into the guard
 * and the translation restarts with a buffer twice as generous. The finished block is
 * copied into the shared code buffer, which is replaced by a larger one when the block
 * does not fit in what is left. `rep movsq` produces far more host code per IR node than
 * the first size allows (FEX_bugs/CodeBufferOverflow uses it the same way), so runs of
 * it overflow; with rcx = 0 it does nothing. fexmac_hook_test counts the overflows. */

/* Blocks of every size from below the first overflow to well past it, in steps of one
 * instruction, so the guard is hit at every offset in its page. A thread keeps its
 * scratch buffer between translations and only claims a larger one when a translation
 * needs it, so each size gets a fresh context, and with it a first buffer of exactly
 * the size the translator asks for. */
static void TestScratchOverflowSizes(void) {
  fexmac_install_fault_handlers();
  uint8_t* Data = MapGuestMemory(0x10000);
  int Wrong = 0, Runs = 0;
  for (size_t Count = 96; Count <= 416; Count += 1, ++Runs) {
    fexmac_context* Context = CreateContextWithOptions(BigBlockOptions, 2, NULL, NULL);
    fexmac_thread* Thread = CreateThreadAt(Context, 0, (uint64_t)(uintptr_t)Data + 0x8000);
    uint8_t* Code = MapGuestMemory(0x4000);
    uint64_t Expected = 0;
    const size_t Length = WriteBigBlock(Code, Count, 32, &Expected);
    if (!RunBigBlock(Thread, Code, Length, Expected, Data) && Wrong++ < 5) {
      printf("  block of %zu rep movsq: wrong result\n", Count);
    }
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
    munmap(Code, 0x4000);
  }
  Check(Wrong == 0, "%d blocks of 96 to 416 rep movsq, each one instruction longer, ran correctly", Runs);
  /* Very large blocks, far past the first overflow, one after another on one thread. */
  fexmac_context* Context = CreateContextWithOptions(BigBlockOptions, 2, NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, 0, (uint64_t)(uintptr_t)Data + 0x8000);
  for (size_t Count = 2048; Count <= 8192; Count *= 2) {
    uint8_t* Code = MapGuestMemory(0x10000);
    uint64_t Expected = 0;
    const size_t Length = WriteBigBlock(Code, Count, 64, &Expected);
    Check(RunBigBlock(Thread, Code, Length, Expected, Data), "a block of %zu rep movsq ran correctly", Count);
    fexmac_invalidate(Context, (uint64_t)(uintptr_t)Code, 0x10000);
    munmap(Code, 0x10000);
  }
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* Many threads translating large blocks at once: each has its own scratch buffer and
 * restart point, and all share the code buffer. */
#define SCRATCH_THREADS 8
static fexmac_context* ScratchContext;
static atomic_int ScratchWrong;

static void* ScratchWorker(void* Arg) {
  const size_t Index = (size_t)(uintptr_t)Arg;
  uint8_t* Data = MapGuestMemory(0x10000);
  fexmac_thread* Thread = NULL;
  if (fexmac_thread_create(ScratchContext, &Thread) != FEXMAC_OK) {
    atomic_fetch_add(&ScratchWrong, 1);
    return NULL;
  }
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RSP] = (uint64_t)(uintptr_t)Data + 0x8000;
  SetState(Thread, &State);
  for (size_t Round = 0; Round < 12; ++Round) {
    uint8_t* Code = MapGuestMemory(0x8000);
    uint64_t Expected = 0;
    const size_t Length = WriteBigBlock(Code, 200 + Index * 97 + Round * 131, 16, &Expected);
    if (!RunBigBlock(Thread, Code, Length, Expected, Data)) {
      atomic_fetch_add(&ScratchWrong, 1);
    }
    fexmac_invalidate(ScratchContext, (uint64_t)(uintptr_t)Code, 0x8000);
    munmap(Code, 0x8000);
  }
  fexmac_thread_destroy(Thread);
  return NULL;
}

static void TestScratchOverflowThreads(void) {
  fexmac_install_fault_handlers();
  ScratchContext = CreateContextWithOptions(BigBlockOptions, 2, NULL, NULL);
  pthread_t Workers[SCRATCH_THREADS];
  for (size_t i = 0; i < SCRATCH_THREADS; ++i) {
    pthread_create(&Workers[i], NULL, ScratchWorker, (void*)(uintptr_t)i);
  }
  for (size_t i = 0; i < SCRATCH_THREADS; ++i) {
    pthread_join(Workers[i], NULL);
  }
  Check(atomic_load(&ScratchWrong) == 0, "%d threads each translated and ran 12 large blocks at the same time", SCRATCH_THREADS);
  fexmac_context_destroy(ScratchContext);
}

static const struct Subtest Subtests[] = {
  {"run", TestRun, 0, NULL, NULL, 0},
  {"syscall-prefix", TestSyscallPrefix, 0, NULL, NULL, 0},
  {"syscall-restart", TestSyscallRestart, 0, NULL, NULL, 0},
  {"state", TestState, 0, NULL, NULL, 0},
  {"abi", TestABI, 0, NULL, NULL, 0},
  {"invalidate", TestInvalidate, 0, NULL, NULL, 0},
  {"faults-installed", TestFaultsInstalled, 0, NULL, NULL, 0},
  {"faults-chained", TestFaultsChained, 0, NULL, NULL, 0},
  {"faults-hosted", TestFaultsHosted, 0, NULL, NULL, 0},
  {"callret-recursion", TestCallRetRecursion, 0, NULL, NULL, 0},
  {"callret-deep-recursion", TestCallRetDeepRecursion, 0, NULL, NULL, 0},
  {"callret-overflow", TestCallRetOverflow, 0, NULL, NULL, 0},
  {"callret-underflow", TestCallRetUnderflow, 0, NULL, NULL, 0},
  {"guest-load-prot-none", TestGuestLoadProtNone, 0, NULL, "fexmac:", 2},
  {"guest-lock-xadd-prot-none", TestGuestLockXaddProtNone, 0, NULL, "fexmac:", 2},
  {"guest-lock-cmpxchg-prot-none", TestGuestLockCmpxchgProtNone, 0, NULL, "fexmac:", 2},
  {"guest-store-read-only", TestGuestStoreReadOnly, 0, NULL, "fexmac:", 2},
  {"guest-misaligned-atomic-prot-none", TestGuestMisalignedProtNone, 0, NULL, "fexmac:", 2},
  {"guest-fault-hosted", TestGuestFaultHosted, 0, NULL, "fexmac:", 3},
  {"callbacks", TestCallbacks, 0, NULL, NULL, 0},
  {"errors", TestErrors, 0, NULL, NULL, 0},
  {"chained-mask", TestChainedMask, 0, NULL, NULL, 0},
  {"chained-resethand-dies", TestChainedResetHand, SIGBUS, NULL, "fexmac:", 1},
  {"chained-retry", TestChainedRetry, 0, NULL, "fexmac:", 1000},
  {"chained-handler-faults-dies", TestChainedHandlerFaults, -1, NULL, "fault inside the fault handler", 1},
  {"chained-handler-faults-deferring-dies", TestChainedHandlerFaultsDeferring, -1, NULL, "fault inside the fault handler", 1},
  {"chained-handler-faults-no-frame-pointer-dies", TestChainedHandlerFaultsNoFramePointer, -1, NULL, "fault inside the fault handler", 1},
  {"chained-siglongjmp", TestChainedSiglongjmp, 0, NULL, "fault inside", 0},
  {"chained-siglongjmp-layouts", TestSiglongjmpLayouts, 0, NULL, "fault inside", 0},
  {"sent-signal-ignored", TestSentSignalIgnored, 0, NULL, NULL, 0},
  {"sent-signal-default-dies", TestSentSignalDefaultDies, SIGSEGV, NULL, "fexmac:", 1},
  {"unclaimed-fault-dies", TestUnclaimedFaultDies, SIGBUS, NULL, "fexmac:", 1},
  {"invalidate-concurrent", TestConcurrentInvalidation, 0, NULL, NULL, 0},
  {"lifetime-races", TestLifetimeRaces, 0, NULL, NULL, 0},
  {"cache-off", TestCacheOff, 0, NULL, NULL, 0},
  {"cache-cold-warm", TestCacheColdWarm, 0, NULL, NULL, 0},
  {"cache-cross-process", TestCacheCrossProcess, 0, NULL, NULL, 0},
  {"cache-untrusted", TestCacheUntrusted, 0, NULL, NULL, 0},
  {"cache-untrusted-file", TestCacheUntrustedFile, 0, NULL, NULL, 0},
  {"cache-unavailable", TestCacheUnavailable, 0, NULL, NULL, 0},
  {"cache-options", TestCacheOptions, 0, NULL, NULL, 0},
  {"cache-fork", TestCacheFork, 0, NULL, NULL, 0},
  {"code-ranges", TestCodeRanges, 0, NULL, NULL, 0},
  {"split-locks-concurrent", TestSplitLocks, 0, NULL, NULL, 0},
  {"split-locks-every-form", TestSplitLockForms, 0, NULL, NULL, 0},
  {"split-locks-signal-storm", TestSplitLockSignalStorm, 0, NULL, NULL, 0},
  {"scratch-overflow-sizes", TestScratchOverflowSizes, 0, NULL, NULL, 0},
  {"scratch-overflow-threads", TestScratchOverflowThreads, 0, NULL, NULL, 0},
};

/* FaultHandlingTest.c */
extern const struct Subtest FaultSubtests[];
extern const size_t FaultSubtestCount;

int main(int argc, char** argv) {
  enum { Own = sizeof(Subtests) / sizeof(Subtests[0]) };
  struct Subtest* All = malloc(sizeof(struct Subtest) * (Own + FaultSubtestCount));
  memcpy(All, Subtests, sizeof(Subtests));
  memcpy(All + Own, FaultSubtests, sizeof(struct Subtest) * FaultSubtestCount);
  return RunSubtests(argc, argv, All, Own + FaultSubtestCount, 120);
}
