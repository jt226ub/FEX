// SPDX-License-Identifier: MIT
//
// Reports how this machine treats executable memory, so the port's assumptions can be
// checked on any Mac (hosted virtual runner or physical hardware).
//
// Each case runs in a child process so a fault is reported instead of ending the probe.
// It prints facts and always exits 0; it does not judge the result.

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

static void* MapJIT(void) {
  return mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
}

// Write with no switch at all: the thread is in whatever state it started in.
static int WriteWithoutSwitch(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return 100;
  memcpy(P, Code, sizeof(Code));
  return 0;
}

// Switch to writable, write, then run without switching back.
static int RunWhileWritable(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return 100;
  pthread_jit_write_protect_np(0);
  memcpy(P, Code, sizeof(Code));
  sys_icache_invalidate(P, sizeof(Code));
  return ((Fn)P)() == 42 ? 0 : 1;
}

// Switch to executable explicitly, then write.
static int WriteWhileExecutable(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return 100;
  pthread_jit_write_protect_np(1);
  memcpy(P, Code, sizeof(Code));
  return 0;
}

// The sequence the port uses.
static int CorrectSequence(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return 100;
  pthread_jit_write_protect_np(0);
  memcpy(P, Code, sizeof(Code));
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(P, sizeof(Code));
  return ((Fn)P)() == 42 ? 0 : 1;
}

// mprotect on one page inside a MAP_JIT mapping (what the code-buffer guard page did).
static int MprotectInsideJIT(void) {
  void* P = mmap(NULL, MAP_SIZE * 2, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
  if (P == MAP_FAILED) return 100;
  return mprotect((char*)P + MAP_SIZE, MAP_SIZE, PROT_NONE) == 0 ? 0 : 2;
}

// Writable and executable memory without MAP_JIT.
static int PlainRWX(void) {
  void* P = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (P == MAP_FAILED) return 100;
  memcpy(P, Code, sizeof(Code));
  sys_icache_invalidate(P, sizeof(Code));
  return ((Fn)P)() == 42 ? 0 : 1;
}

// Read the cache type register from user space (the port uses sysctl instead).
static int ReadCTR(void) {
  uint64_t V;
  __asm volatile("mrs %[v], ctr_el0" : [v] "=r"(V));
  return V ? 0 : 1;
}

// Data cache clean by address from user space.
static int DCCVAU(void) {
  static char Buffer[64];
  __asm volatile("dc cvau, %[p]" : : [p] "r"(Buffer));
  return 0;
}

// An atomic compare-and-swap on MAP_JIT memory while switched to executable. The
// translator keeps a lock word in each block's tail, inside the code buffer.
static int CASWhileExecutable(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return 100;
  pthread_jit_write_protect_np(1);
  uint32_t Expected = 0;
  __atomic_compare_exchange_n((uint32_t*)P, &Expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return 0;
}

// Fault handlers for the cases below. The trigger is a misaligned atomic load, which
// raises SIGBUS as the translator's misaligned guest accesses do. Apple silicon allows
// a misaligned atomic access that stays inside 16 bytes, so this one crosses a 16-byte
// boundary.
static void* HandlerTarget;
static volatile int HandlerDone;

static void TriggerMisalignedFault(void) {
  static _Alignas(16) uint64_t Buffer[4];
  uint64_t V;
  char* Misaligned = (char*)Buffer + 12;
  __asm volatile("ldar %[v], [%[p]]" : [v] "=r"(V) : [p] "r"(Misaligned) : "memory");
}

// Skips the faulting instruction so the program continues.
static void SkipFault(void* RawContext) {
  ucontext_t* Context = (ucontext_t*)RawContext;
  Context->uc_mcontext->__ss.__pc += 4;
}

static void WriteInHandlerNoSwitch(int Signal, siginfo_t* Info, void* RawContext) {
  (void)Signal;
  (void)Info;
  if (!HandlerDone) {
    HandlerDone = 1;
    // Faults again while SIGBUS is blocked by this handler.
    memcpy(HandlerTarget, Code, sizeof(Code));
  }
  SkipFault(RawContext);
}

static void WriteInHandlerSwitched(int Signal, siginfo_t* Info, void* RawContext) {
  (void)Signal;
  (void)Info;
  pthread_jit_write_protect_np(0);
  memcpy(HandlerTarget, Code, sizeof(Code));
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(HandlerTarget, sizeof(Code));
  SkipFault(RawContext);
}

static void SwitchInHandlerOnly(int Signal, siginfo_t* Info, void* RawContext) {
  (void)Signal;
  (void)Info;
  pthread_jit_write_protect_np(0);
  SkipFault(RawContext);
}

static int InstallBusHandler(void (*Handler)(int, siginfo_t*, void*)) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = Handler;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  return sigaction(SIGBUS, &Action, NULL);
}

// A write to write-protected code memory from inside a SIGBUS handler: the second
// fault arrives while SIGBUS is blocked.
static int FaultInsideFaultHandler(void) {
  HandlerTarget = MapJIT();
  if (HandlerTarget == MAP_FAILED) return 100;
  pthread_jit_write_protect_np(1);
  InstallBusHandler(WriteInHandlerNoSwitch);
  TriggerMisalignedFault();
  return 0;
}

// The pattern the translator's fault-time patching needs: switch, write, switch back,
// all inside the handler, then run the written code.
static int SwitchInsideFaultHandler(void) {
  HandlerTarget = MapJIT();
  if (HandlerTarget == MAP_FAILED) return 100;
  pthread_jit_write_protect_np(1);
  InstallBusHandler(WriteInHandlerSwitched);
  TriggerMisalignedFault();
  return ((Fn)HandlerTarget)() == 42 ? 0 : 1;
}

// A handler switches to writable and returns without switching back. Whether the
// interrupted code then runs writable or executable says whether returning from a
// signal restores the thread's setting. "ok": restored (the code still runs). "FAULT":
// not restored (the handler's setting carried over, so running the code faults).
static int SettingAfterSignalReturn(void) {
  void* P = MapJIT();
  if (P == MAP_FAILED) return 100;
  pthread_jit_write_protect_np(0);
  memcpy(P, Code, sizeof(Code));
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(P, sizeof(Code));
  InstallBusHandler(SwitchInHandlerOnly);
  TriggerMisalignedFault();
  // Executable: this runs. Writable: this faults (SIGBUS, which the handler would
  // skip, so reset to the default first).
  signal(SIGBUS, SIG_DFL);
  if (((Fn)P)() != 42) return 1;
  return 0;
}

static void Run(const char* Name, int (*Case)(void)) {
  fflush(stdout);
  pid_t Pid = fork();
  if (Pid == 0) {
    // A case that never finishes is reported as a hang.
    alarm(5);
    _exit(Case());
  }
  int Status = 0;
  waitpid(Pid, &Status, 0);
  if (WIFSIGNALED(Status) && WTERMSIG(Status) == SIGALRM) {
    printf("%-52s HANG (no progress in 5 seconds)\n", Name);
  } else if (WIFSIGNALED(Status)) {
    printf("%-52s FAULT (signal %d, %s)\n", Name, WTERMSIG(Status), strsignal(WTERMSIG(Status)));
  } else if (WEXITSTATUS(Status) == 0) {
    printf("%-52s ok\n", Name);
  } else if (WEXITSTATUS(Status) == 100) {
    printf("%-52s mapping refused\n", Name);
  } else if (WEXITSTATUS(Status) == 2) {
    printf("%-52s call refused\n", Name);
  } else {
    printf("%-52s wrong result\n", Name);
  }
}

int main(void) {
  char Model[128] = "unknown";
  size_t Size = sizeof(Model);
  sysctlbyname("hw.model", Model, &Size, NULL, 0);
  char Brand[128] = "unknown";
  Size = sizeof(Brand);
  sysctlbyname("machdep.cpu.brand_string", Brand, &Size, NULL, 0);
  int Hypervisor = -1;
  Size = sizeof(Hypervisor);
  sysctlbyname("kern.hv_vmm_present", &Hypervisor, &Size, NULL, 0);

  printf("Model: %s\nCPU: %s\nRunning inside a virtual machine: %s\n", Model, Brand,
         Hypervisor == 1 ? "yes" : Hypervisor == 0 ? "no" : "unknown");
  printf("Page size: %ld\n", sysconf(_SC_PAGESIZE));
  printf("pthread_jit_write_protect_supported_np: %d\n\n", pthread_jit_write_protect_supported_np());

  Run("MAP_JIT: correct switch, write, switch, run", CorrectSequence);
  Run("MAP_JIT: write with no switch (thread default)", WriteWithoutSwitch);
  Run("MAP_JIT: write while switched to executable", WriteWhileExecutable);
  Run("MAP_JIT: run while still switched to writable", RunWhileWritable);
  Run("MAP_JIT: mprotect one page inside the mapping", MprotectInsideJIT);
  Run("Plain writable+executable memory without MAP_JIT", PlainRWX);
  Run("MAP_JIT: compare-and-swap while executable", CASWhileExecutable);
  Run("MAP_JIT: write in a SIGBUS handler, no switch", FaultInsideFaultHandler);
  Run("MAP_JIT: switch, write, switch in a SIGBUS handler", SwitchInsideFaultHandler);
  Run("MAP_JIT: handler left writable, then run after it", SettingAfterSignalReturn);
  Run("Read ctr_el0 from user space", ReadCTR);
  Run("dc cvau from user space", DCCVAU);
  return 0;
}
