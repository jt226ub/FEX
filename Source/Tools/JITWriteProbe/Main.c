// SPDX-License-Identifier: MIT
//
// Reports how this machine treats executable memory, so the port's assumptions can be
// checked on any Mac (hosted virtual runner or physical hardware).
//
// Each case runs in a child process so a fault is reported instead of ending the probe.
//
//   JITWriteProbe                    prints the facts and exits 0, whatever they are.
//   JITWriteProbe --expect-enforced  also exits 1 unless every asserted case gives the
//                                    result the port relies on when write protection
//                                    is enforced (a physical Apple silicon Mac).
//
// GitHub's hosted Mac does not enforce write protection, so only the first form can run
// there.

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

// mov w0, #42 ; ret
static const uint32_t Code[] = {0x52800540, 0xd65f03c0};
#define MAP_SIZE 0x4000

typedef int (*Fn)(void);

// What a case can report. A case returns one of the values below FAULT as its exit
// status; the rest come from how the child ended.
enum Outcome {
  OK = 0,
  WRONG_RESULT,
  CALL_REFUSED,
  MAPPING_REFUSED,
  HANDLER_NOT_RUN,
  NOT_ENFORCED,
  SETTING_RESTORED,
  SETTING_NOT_RESTORED,
  STALE_CODE_RAN,
  ALIGNMENT_FAULT,
  ACCESS_FAULT,
  OTHER_FAULT_STATUS,
  FAULT,   // SIGBUS or SIGSEGV
  ILLEGAL, // SIGILL
  OTHER_SIGNAL,
  HANG,
  // Not an outcome: marks a case that --expect-enforced does not assert.
  REPORT_ONLY,
};

static const char* OutcomeName(enum Outcome O) {
  switch (O) {
  case OK: return "ok";
  case WRONG_RESULT: return "wrong result";
  case CALL_REFUSED: return "call refused";
  case MAPPING_REFUSED: return "mapping refused";
  case HANDLER_NOT_RUN: return "handler did not run (test not valid)";
  case NOT_ENFORCED: return "not enforced (write was allowed)";
  case SETTING_RESTORED: return "restored (thread executable again)";
  case SETTING_NOT_RESTORED: return "not restored (handler's setting kept)";
  case STALE_CODE_RAN: return "stale code ran";
  case ALIGNMENT_FAULT: return "alignment fault (DFSC 0x21)";
  case ACCESS_FAULT: return "translation or permission fault (DFSC 0x04-0x0F)";
  case OTHER_FAULT_STATUS: return "other fault status";
  case FAULT: return "FAULT";
  case ILLEGAL: return "FAULT";
  case OTHER_SIGNAL: return "FAULT";
  case HANG: return "HANG (no progress in 5 seconds)";
  case REPORT_ONLY: return "report only";
  }
  return "?";
}

static void* MapJIT(void) {
  return mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
}

// One 32-bit store, as a single instruction so a fault handler can step over it.
static void Store32(void* P, uint32_t V) {
  __asm volatile("str %w[v], [%[p]]" : : [v] "r"(V), [p] "r"(P) : "memory");
}

// ---- Signal helpers ----

static volatile sig_atomic_t HandlerEntries;
static volatile sig_atomic_t RecordedFaults;
static void* HandlerTarget;

static void SkipFault(void* RawContext) {
  ucontext_t* Context = (ucontext_t*)RawContext;
  Context->uc_mcontext->__ss.__pc += 4;
}

static void InstallHandler(int Signal, void (*Handler)(int, siginfo_t*, void*)) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = Handler;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  sigaction(Signal, &Action, NULL);
}

static void RecordAndSkip(int Signal, siginfo_t* Info, void* RawContext) {
  (void)Signal;
  (void)Info;
  RecordedFaults++;
  SkipFault(RawContext);
}

// Returns 1 if a single store to P faults, without ending the process.
static int StoreFaults(void* P) {
  RecordedFaults = 0;
  InstallHandler(SIGBUS, RecordAndSkip);
  InstallHandler(SIGSEGV, RecordAndSkip);
  Store32(P, 0);
  signal(SIGBUS, SIG_DFL);
  signal(SIGSEGV, SIG_DFL);
  return RecordedFaults != 0;
}

// The trigger for the handler cases is a misaligned atomic load, which raises SIGBUS as
// the translator's misaligned guest accesses do. Apple silicon allows a misaligned atomic
// access that stays inside 16 bytes (see the two cases that check this), so this one
// crosses a 16-byte boundary.
static _Alignas(16) uint64_t MisalignBuffer[4];

static void MisalignedLoad(int Offset) {
  uint64_t V;
  char* Misaligned = (char*)MisalignBuffer + Offset;
  __asm volatile("ldar %[v], [%[p]]" : [v] "=r"(V) : [p] "r"(Misaligned) : "memory");
}

static void TriggerMisalignedFault(void) {
  MisalignedLoad(12);
}

// ---- Cases ----

// The sequence the port uses.
static int CorrectSequence(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  pthread_jit_write_protect_np(0);
  memcpy(P, Code, sizeof(Code));
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(P, sizeof(Code));
  return ((Fn)P)() == 42 ? OK : WRONG_RESULT;
}

// Write with no switch at all: the thread is in whatever state it started in.
static int WriteWithoutSwitch(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  memcpy(P, Code, sizeof(Code));
  return OK;
}

// Switch to executable explicitly, then write.
static int WriteWhileExecutable(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  pthread_jit_write_protect_np(1);
  memcpy(P, Code, sizeof(Code));
  return OK;
}

// Switch to writable, write, then run without switching back.
static int RunWhileWritable(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  pthread_jit_write_protect_np(0);
  memcpy(P, Code, sizeof(Code));
  sys_icache_invalidate(P, sizeof(Code));
  return ((Fn)P)() == 42 ? OK : WRONG_RESULT;
}

// An atomic compare-and-swap on MAP_JIT memory while switched to executable. The
// translator keeps a lock word in each block's tail, inside the code buffer.
static int CASWhileExecutable(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  pthread_jit_write_protect_np(1);
  uint32_t Expected = 0;
  __atomic_compare_exchange_n((uint32_t*)P, &Expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return OK;
}

// mprotect on one page inside a MAP_JIT mapping (what the code-buffer guard page did).
static int MprotectInsideJIT(void) {
  void* P = mmap(NULL, MAP_SIZE * 2, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  return mprotect((char*)P + MAP_SIZE, MAP_SIZE, PROT_NONE) == 0 ? OK : CALL_REFUSED;
}

// Writable and executable memory without MAP_JIT.
static int PlainRWX(void) {
  void* P = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  memcpy(P, Code, sizeof(Code));
  sys_icache_invalidate(P, sizeof(Code));
  return ((Fn)P)() == 42 ? OK : WRONG_RESULT;
}

// Rewrites the same code over and over with a different result each time, runs it, and
// checks the new code ran. Mode 0 flushes the instruction cache while the thread is still
// writable, mode 1 after switching back to executable (what the dispatcher does), mode 2
// never flushes, as a control that shows whether this test can see stale code at all.
static int CodeChangeLoop(int Mode) {
  uint32_t* P = (uint32_t*)MapJIT();
  if ((void*)P == MAP_FAILED) return MAPPING_REFUSED;
  for (uint32_t I = 0; I < 50000; ++I) {
    const uint32_t Value = I & 0xffff;
    pthread_jit_write_protect_np(0);
    P[0] = 0x52800000 | (Value << 5); // mov w0, #Value
    P[1] = 0xd65f03c0;                // ret
    if (Mode == 0) sys_icache_invalidate(P, 8);
    pthread_jit_write_protect_np(1);
    if (Mode == 1) sys_icache_invalidate(P, 8);
    if ((uint32_t)((Fn)P)() != Value) return STALE_CODE_RAN;
  }
  return OK;
}
static int FlushWhileWritable(void) {
  return CodeChangeLoop(0);
}
static int FlushAfterSwitch(void) {
  return CodeChangeLoop(1);
}
static int NoFlush(void) {
  return CodeChangeLoop(2);
}

static void WriteInHandlerNoSwitch(int Signal, siginfo_t* Info, void* RawContext) {
  (void)Signal;
  (void)Info;
  HandlerEntries++;
  // Faults again, while SIGBUS is blocked by this handler, if protection is enforced.
  Store32(HandlerTarget, Code[0]);
  SkipFault(RawContext);
}

// A write to write-protected code memory from inside a SIGBUS handler.
static int FaultInsideFaultHandler(void) {
  HandlerTarget = MapJIT();
  if (HandlerTarget == MAP_FAILED) return MAPPING_REFUSED;
  pthread_jit_write_protect_np(1);
  HandlerEntries = 0;
  InstallHandler(SIGBUS, WriteInHandlerNoSwitch);
  TriggerMisalignedFault();
  signal(SIGBUS, SIG_DFL);
  // Reaching here means the write inside the handler was allowed.
  return HandlerEntries ? NOT_ENFORCED : HANDLER_NOT_RUN;
}

static void WriteInHandlerSwitched(int Signal, siginfo_t* Info, void* RawContext) {
  (void)Signal;
  (void)Info;
  HandlerEntries++;
  pthread_jit_write_protect_np(0);
  memcpy(HandlerTarget, Code, sizeof(Code));
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(HandlerTarget, sizeof(Code));
  SkipFault(RawContext);
}

// The pattern the translator's fault-time patching needs: switch, write, switch back,
// all inside the handler, then run the written code.
static int SwitchInsideFaultHandler(void) {
  HandlerTarget = MapJIT();
  if (HandlerTarget == MAP_FAILED) return MAPPING_REFUSED;
  pthread_jit_write_protect_np(1);
  HandlerEntries = 0;
  InstallHandler(SIGBUS, WriteInHandlerSwitched);
  TriggerMisalignedFault();
  signal(SIGBUS, SIG_DFL);
  if (!HandlerEntries) return HANDLER_NOT_RUN;
  return ((Fn)HandlerTarget)() == 42 ? OK : WRONG_RESULT;
}

static void SwitchInHandlerOnly(int Signal, siginfo_t* Info, void* RawContext) {
  (void)Signal;
  (void)Info;
  HandlerEntries++;
  pthread_jit_write_protect_np(0);
  SkipFault(RawContext);
}

// A handler switches to writable and returns without switching back. Whether a store
// from the interrupted code then faults says whether returning from a signal restored the
// thread's setting.
static int SettingAfterSignalReturn(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return MAPPING_REFUSED;
  void* Target = (char*)P + 64;
  pthread_jit_write_protect_np(1);
  if (!StoreFaults(Target)) return NOT_ENFORCED;
  HandlerEntries = 0;
  InstallHandler(SIGBUS, SwitchInHandlerOnly);
  TriggerMisalignedFault();
  signal(SIGBUS, SIG_DFL);
  if (!HandlerEntries) return HANDLER_NOT_RUN;
  return StoreFaults(Target) ? SETTING_RESTORED : SETTING_NOT_RESTORED;
}

// Misaligned atomic loads: inside one 16-byte unit, and across two.
static int MisalignedAtomicWithin16(void) {
  MisalignedLoad(4);
  return OK;
}
static int MisalignedAtomicAcross16(void) {
  MisalignedLoad(12);
  return OK;
}

// System registers from user space (the port uses sysctl instead of all of these).
static int ReadCTR(void) {
  uint64_t V;
  __asm volatile("mrs %[v], ctr_el0" : [v] "=r"(V));
  return V ? OK : WRONG_RESULT;
}
static int ReadMIDR(void) {
  uint64_t V;
  __asm volatile("mrs %[v], midr_el1" : [v] "=r"(V));
  return V ? OK : WRONG_RESULT;
}
static int ReadISAR0(void) {
  uint64_t V;
  __asm volatile("mrs %[v], id_aa64isar0_el1" : [v] "=r"(V));
  return OK;
}

// Data cache clean by address from user space.
static int DCCVAU(void) {
  static char Buffer[64];
  __asm volatile("dc cvau, %[p]" : : [p] "r"(Buffer));
  return OK;
}

// ---- Fault status: what the kernel reports for each kind of data abort ----
//
// fexmac tells a misaligned atomic from a protection fault by the data abort's fault
// status code in ESR_EL1, which macOS passes in the signal context: both arrive as
// SIGBUS with si_code 1 (BUS_ADRALN), so si_code cannot tell them apart. Each case faults
// once; the handler prints what the kernel reported and ends the child with the kind.

static void ReportFaultStatus(int Signal, siginfo_t* Info, void* RawContext) {
  const ucontext_t* Context = (const ucontext_t*)RawContext;
  const uint32_t ESR = Context->uc_mcontext->__es.__esr;
  const uint32_t Class = ESR >> 26;
  const uint32_t DFSC = ESR & 0x3F;
  char Line[160];
  const int Length = snprintf(Line, sizeof(Line), "  [signal %d si_code %d ESR 0x%08x DFSC 0x%02x WnR %u] ", Signal, Info->si_code, ESR, DFSC,
                              (ESR >> 6) & 1);
  (void)!write(STDOUT_FILENO, Line, (size_t)Length);
  if (Class != 0x24 && Class != 0x25) {
    _exit(OTHER_FAULT_STATUS);
  }
  _exit(DFSC == 0x21 ? ALIGNMENT_FAULT : (DFSC >= 0x04 && DFSC <= 0x0F) ? ACCESS_FAULT : OTHER_FAULT_STATUS);
}

static void* FaultStatusSetup(int Protection) {
  InstallHandler(SIGBUS, ReportFaultStatus);
  InstallHandler(SIGSEGV, ReportFaultStatus);
  void* P = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  (void)*(volatile char*)P;
  mprotect(P, MAP_SIZE, Protection);
  return P;
}

static int StatusLoadProtNone(void) {
  volatile char* P = FaultStatusSetup(PROT_NONE);
  (void)*P;
  return OK;
}
static int StatusStoreReadOnly(void) {
  volatile char* P = FaultStatusSetup(PROT_READ);
  *P = 1;
  return OK;
}
static int StatusAtomicProtNone(void) {
  uint64_t* P = FaultStatusSetup(PROT_NONE);
  __atomic_fetch_add(P, 1, __ATOMIC_SEQ_CST);
  return OK;
}
static int StatusMisalignedAtomic(void) {
  char* P = FaultStatusSetup(PROT_READ | PROT_WRITE);
  uint32_t One = 1;
  __asm volatile("ldaddal %w[v], %w[v], [%[p]]" : [v] "+r"(One) : [p] "r"(P + 14) : "memory");
  return OK;
}
static int StatusMisalignedAtomicProtNone(void) {
  char* P = FaultStatusSetup(PROT_NONE);
  uint32_t One = 1;
  __asm volatile("ldaddal %w[v], %w[v], [%[p]]" : [v] "+r"(One) : [p] "r"(P + 14) : "memory");
  return OK;
}
static int StatusUnmapped(void) {
  FaultStatusSetup(PROT_READ);
  (void)*(volatile char*)0x7f0000000000ULL;
  return OK;
}

// ---- Driver ----

static enum Outcome Run(const char* Name, int (*Case)(void)) {
  fflush(stdout);
  pid_t Pid = fork();
  if (Pid == 0) {
    // A case that never finishes is reported as a hang.
    alarm(5);
    _exit(Case());
  }
  int Status = 0;
  waitpid(Pid, &Status, 0);
  enum Outcome O;
  if (WIFSIGNALED(Status)) {
    const int Signal = WTERMSIG(Status);
    O = Signal == SIGALRM ? HANG : (Signal == SIGBUS || Signal == SIGSEGV) ? FAULT : Signal == SIGILL ? ILLEGAL : OTHER_SIGNAL;
    if (O != HANG) {
      printf("%-52s FAULT (signal %d, %s)", Name, Signal, strsignal(Signal));
      return O;
    }
  } else {
    O = (enum Outcome)WEXITSTATUS(Status);
  }
  printf("%-52s %s", Name, OutcomeName(O));
  return O;
}

struct ProbeCase {
  const char* Name;
  int (*Case)(void);
  // The result the port relies on with write protection enforced, or REPORT_ONLY.
  enum Outcome Enforced;
};

static const struct ProbeCase Cases[] = {
  {"MAP_JIT: correct switch, write, switch, run", CorrectSequence, OK},
  {"MAP_JIT: write with no switch (thread default)", WriteWithoutSwitch, FAULT},
  {"MAP_JIT: write while switched to executable", WriteWhileExecutable, FAULT},
  {"MAP_JIT: run while still switched to writable", RunWhileWritable, FAULT},
  {"MAP_JIT: compare-and-swap while executable", CASWhileExecutable, FAULT},
  {"MAP_JIT: mprotect one page inside the mapping", MprotectInsideJIT, REPORT_ONLY},
  {"Plain writable+executable memory without MAP_JIT", PlainRWX, REPORT_ONLY},
  {"MAP_JIT: rewrite code, flush while writable", FlushWhileWritable, OK},
  {"MAP_JIT: rewrite code, flush after switching back", FlushAfterSwitch, OK},
  {"MAP_JIT: rewrite code, never flush (control)", NoFlush, REPORT_ONLY},
  {"MAP_JIT: write in a SIGBUS handler, no switch", FaultInsideFaultHandler, HANG},
  {"MAP_JIT: switch, write, switch in a SIGBUS handler", SwitchInsideFaultHandler, OK},
  {"MAP_JIT: handler left writable; setting on return", SettingAfterSignalReturn, SETTING_NOT_RESTORED},
  {"Misaligned atomic load inside 16 bytes", MisalignedAtomicWithin16, REPORT_ONLY},
  {"Misaligned atomic load across 16 bytes", MisalignedAtomicAcross16, FAULT},
  {"Read ctr_el0 from user space", ReadCTR, REPORT_ONLY},
  {"Read midr_el1 from user space", ReadMIDR, REPORT_ONLY},
  {"Read id_aa64isar0_el1 from user space", ReadISAR0, REPORT_ONLY},
  {"dc cvau from user space", DCCVAU, OK},
  {"Fault status: load from a PROT_NONE page", StatusLoadProtNone, ACCESS_FAULT},
  {"Fault status: store to a read-only page", StatusStoreReadOnly, ACCESS_FAULT},
  {"Fault status: atomic add on a PROT_NONE page", StatusAtomicProtNone, ACCESS_FAULT},
  {"Fault status: load from an unmapped address", StatusUnmapped, ACCESS_FAULT},
  {"Fault status: misaligned atomic across 16 bytes", StatusMisalignedAtomic, ALIGNMENT_FAULT},
  {"Fault status: misaligned atomic on a PROT_NONE page", StatusMisalignedAtomicProtNone, ALIGNMENT_FAULT},
};

int main(int argc, char** argv) {
  const int Expect = argc > 1 && strcmp(argv[1], "--expect-enforced") == 0;
  if (argc > 1 && !Expect) {
    fprintf(stderr, "usage: %s [--expect-enforced]\n", argv[0]);
    return 2;
  }

  char Model[128] = "unknown";
  size_t Size = sizeof(Model);
  sysctlbyname("hw.model", Model, &Size, NULL, 0);
  char Brand[128] = "unknown";
  Size = sizeof(Brand);
  sysctlbyname("machdep.cpu.brand_string", Brand, &Size, NULL, 0);
  int Hypervisor = -1;
  Size = sizeof(Hypervisor);
  sysctlbyname("kern.hv_vmm_present", &Hypervisor, &Size, NULL, 0);
  const int Supported = pthread_jit_write_protect_supported_np();

  printf("Model: %s\nCPU: %s\nRunning inside a virtual machine: %s\n", Model, Brand,
         Hypervisor == 1 ? "yes" : Hypervisor == 0 ? "no" : "unknown");
  printf("Page size: %ld\n", sysconf(_SC_PAGESIZE));
  printf("pthread_jit_write_protect_supported_np: %d\n\n", Supported);

  int Failures = 0;
  if (Expect && Supported != 1) {
    printf("EXPECTED pthread_jit_write_protect_supported_np to be 1\n");
    ++Failures;
  }
  for (size_t I = 0; I < sizeof(Cases) / sizeof(Cases[0]); ++I) {
    const enum Outcome O = Run(Cases[I].Name, Cases[I].Case);
    // A handler case whose handler never ran says nothing, in either mode.
    const int Wrong = O == HANDLER_NOT_RUN || (Cases[I].Enforced != REPORT_ONLY && O != Cases[I].Enforced);
    if (Expect && Wrong) {
      printf("   <-- EXPECTED: %s", OutcomeName(Cases[I].Enforced));
      ++Failures;
    }
    printf("\n");
  }

  if (Expect) {
    printf("\n%s: %d case(s) differ from enforced write protection\n", Failures ? "FAIL" : "PASS", Failures);
    return Failures ? 1 : 0;
  }
  return 0;
}
