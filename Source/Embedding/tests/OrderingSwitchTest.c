/* SPDX-License-Identifier: MIT */
/*
 * Ordering deferred while single-threaded (option TSODEFERRED): subtests compiled into
 * fexmac_c_test (the production archive, public interface only) and into fexmac_hook_test
 * (with ORDERING_SWITCH_HOOKS: the test-only hooks, which read the core's effective
 * ordering and break the switch on purpose to show that the subtests can fail).
 */
#include "TestSupport.h"
#ifdef ORDERING_SWITCH_HOOKS
#include "fexmac_testing.h"
#endif

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const fexmac_option DeferredOptions[] = {
  {"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"MEMCPYSETTSOENABLED", "1"}, {"MULTIBLOCK", "1"}, {"TSODEFERRED", "1"}};
static const fexmac_option OrderedOptions[] = {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"MEMCPYSETTSOENABLED", "1"}, {"MULTIBLOCK", "1"}};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint64_t Load64(const uint8_t* Base, size_t Offset) {
  return atomic_load_explicit((_Atomic uint64_t*)(uintptr_t)(Base + Offset), memory_order_acquire);
}
static void Store64(uint8_t* Base, size_t Offset, uint64_t Value) {
  atomic_store_explicit((_Atomic uint64_t*)(uintptr_t)(Base + Offset), Value, memory_order_release);
}

static void InstallHandlers(void) {
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers");
}

static uint64_t CompiledBlocks(fexmac_context* Context) {
  fexmac_translation_counters C;
  fexmac_translation_counters_init(&C);
  CheckResult(fexmac_context_get_counters(Context, &C), FEXMAC_OK, "fexmac_context_get_counters");
  return C.blocks_compiled;
}

struct RunArgs {
  fexmac_thread* Thread;
  fexmac_stop Stop;
  atomic_int Ended;
};

static void* RunWorker(void* Arg) {
  struct RunArgs* A = (struct RunArgs*)Arg;
  A->Stop = Run(A->Thread);
  atomic_store(&A->Ended, 1);
  return NULL;
}

static fexmac_thread* ThreadAt(fexmac_context* Context, uint64_t RIP, uint64_t RBXValue) {
  fexmac_thread* Thread = CreateThreadAt(Context, RIP, 0);
  fexmac_x86_state S = GetState(Thread);
  S.gpr[RBX] = RBXValue;
  SetState(Thread, &S);
  return Thread;
}

/* clang-format off */
/* A memory-heavy loop that runs until [rbx+16] is set: [rbx] and rax count iterations. */
static const uint8_t CountLoop[] = {
  0x48, 0x8B, 0x03,             /* loop: mov rax, [rbx] */
  0x48, 0x83, 0xC0, 0x01,       /*   add rax, 1 */
  0x48, 0x89, 0x03,             /*   mov [rbx], rax */
  0x48, 0x8B, 0x4B, 0x10,       /*   mov rcx, [rbx+16] */
  0x48, 0x85, 0xC9,             /*   test rcx, rcx */
  0x74, 0xED,                   /*   jz loop */
  0xF4,                         /*   hlt */
};
/* Sets [rbx+16]. */
static const uint8_t SetStop[] = {
  0x48, 0xC7, 0x43, 0x10, 0x01, 0x00, 0x00, 0x00, /* mov qword [rbx+16], 1 */
  0xF4,
};
/* clang-format on */

/* ---- the option off: ordering as configured, always ---- */
static void TestDeferredOptionOff(void) {
  InstallHandlers();
  uint8_t* Mem = MapGuestMemory(0x8000);
  memcpy(Mem, CountLoop, sizeof(CountLoop));
  memcpy(Mem + 0x100, SetStop, sizeof(SetStop));
  uint8_t* Data = Mem + 0x4000;
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  fexmac_context* Context = CreateContextWithOptions(OrderedOptions, COUNT(OrderedOptions), NULL, NULL);
  Check(fexmac_context_ordering_deferred(Context) == 0, "option off: not deferred");
#ifdef ORDERING_SWITCH_HOOKS
  uint32_t Effective = 0;
  uint64_t Stats[5];
  fexmac_testing_ordering_state(Context, &Effective, Stats);
  Check(Effective == 7, "option off: the core translates with all three orderings (%u)", Effective);
#endif
  struct RunArgs A = {.Thread = ThreadAt(Context, Base, Base + 0x4000)};
  pthread_t Host;
  pthread_create(&Host, NULL, RunWorker, &A);
  while (Load64(Data, 0) < 10000) {
  }
  const uint64_t Before = CompiledBlocks(Context);
  fexmac_thread* Second = ThreadAt(Context, Base + 0x100, Base + 0x4000);
  const fexmac_stop Stop2 = Run(Second);
  pthread_join(Host, NULL);
  const fexmac_x86_state S = GetState(A.Thread);
  Check(Stop2.reason == FEXMAC_STOP_HALT && A.Stop.reason == FEXMAC_STOP_HALT && S.gpr[RAX] == Load64(Data, 0),
        "option off: both threads halt, the loop counted right (%" PRIu64 ")", S.gpr[RAX]);
  Check(CompiledBlocks(Context) - Before == 1, "option off: a second thread discards nothing (1 new block: the second thread's)");
  CheckResult(fexmac_context_mark_memory_shared(Context), FEXMAC_OK, "mark_memory_shared with the option off (no-op)");
  Check(fexmac_context_ordering_deferred(Context) == 0, "option off: still not deferred");
#ifdef ORDERING_SWITCH_HOOKS
  fexmac_testing_ordering_state(Context, &Effective, Stats);
  Check(Stats[0] == 0 && Effective == 7, "option off: no switch made");
#endif
  fexmac_thread_destroy(Second);
  fexmac_thread_destroy(A.Thread);
  fexmac_context_destroy(Context);

  /* TSODEFERRED with no ordering configured: nothing to defer. */
  const fexmac_option NoOrdering[] = {{"TSOENABLED", "0"}, {"TSODEFERRED", "1"}};
  Context = CreateContextWithOptions(NoOrdering, COUNT(NoOrdering), NULL, NULL);
  Check(fexmac_context_ordering_deferred(Context) == 0, "TSODEFERRED with TSOENABLED=0: nothing deferred");
  fexmac_context_destroy(Context);
  munmap(Mem, 0x8000);
}

/* ---- one thread: deferred; a second live thread switches, for good ---- */
static void TestDeferredLifecycle(void) {
  InstallHandlers();
  uint8_t* Mem = MapGuestMemory(0x8000);
  memcpy(Mem, SetStop, sizeof(SetStop));
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  fexmac_context* Context = CreateContextWithOptions(DeferredOptions, COUNT(DeferredOptions), NULL, NULL);
  Check(fexmac_context_ordering_deferred(Context) == 1, "TSODEFERRED: deferred at creation");
#ifdef ORDERING_SWITCH_HOOKS
  uint32_t Effective = 9;
  uint64_t Stats[5];
  fexmac_testing_ordering_state(Context, &Effective, Stats);
  Check(Effective == 0, "deferred: the core translates without ordering (%u)", Effective);
#endif
  /* One thread at a time is one guest thread: create, run, destroy, create again. */
  fexmac_thread* T = ThreadAt(Context, Base, Base + 0x4000);
  Check(Run(T).reason == FEXMAC_STOP_HALT, "first thread runs");
  fexmac_thread_destroy(T);
  T = ThreadAt(Context, Base, Base + 0x4000);
  Check(fexmac_context_ordering_deferred(Context) == 1, "a thread created after the first was destroyed: still deferred");
  fexmac_thread* Second = ThreadAt(Context, Base, Base + 0x4000);
  Check(fexmac_context_ordering_deferred(Context) == 0, "a second live thread: ordering on");
#ifdef ORDERING_SWITCH_HOOKS
  fexmac_testing_ordering_state(Context, &Effective, Stats);
  Check(Effective == 7 && Stats[0] == 1, "after the switch the core translates with all three orderings (%u), one switch", Effective);
#endif
  fexmac_thread_destroy(Second);
  Check(Run(T).reason == FEXMAC_STOP_HALT, "back to one thread: runs");
  Check(fexmac_context_ordering_deferred(Context) == 0, "back to one thread: never switched back");
  fexmac_thread_destroy(T);
  fexmac_context_destroy(Context);

  /* Only the scalar ordering configured: deferred, then the scalar one only. */
  const fexmac_option Scalar[] = {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "0"}, {"MEMCPYSETTSOENABLED", "0"}, {"TSODEFERRED", "1"}};
  Context = CreateContextWithOptions(Scalar, COUNT(Scalar), NULL, NULL);
  CheckResult(fexmac_context_mark_memory_shared(Context), FEXMAC_OK, "mark_memory_shared with no thread");
  Check(fexmac_context_ordering_deferred(Context) == 0, "mark_memory_shared with no thread: ordering on");
#ifdef ORDERING_SWITCH_HOOKS
  fexmac_testing_ordering_state(Context, &Effective, Stats);
  Check(Effective == 1, "scalar configuration: scalar ordering after the switch (%u)", Effective);
#endif
  fexmac_context_destroy(Context);
  munmap(Mem, 0x8000);
}

/* ---- CPUID does not change at the switch ---- */
static void TestDeferredCPUID(void) {
  InstallHandlers();
  uint32_t Off[4], Before[4], After[4];
  fexmac_context* Context = CreateContextWithOptions(OrderedOptions, COUNT(OrderedOptions), NULL, NULL);
  fexmac_cpuid(Context, 7, 0, Off);
  fexmac_context_destroy(Context);
  Context = CreateContextWithOptions(DeferredOptions, COUNT(DeferredOptions), NULL, NULL);
  fexmac_cpuid(Context, 7, 0, Before);
  fexmac_context_mark_memory_shared(Context);
  fexmac_cpuid(Context, 7, 0, After);
  fexmac_context_destroy(Context);
  Check(memcmp(Off, Before, sizeof(Off)) == 0 && memcmp(Before, After, sizeof(Off)) == 0 && !(Before[1] & (1u << 9)),
        "CPUID leaf 7 the same deferred, after the switch and with the option off (ERMS clear with string ordering configured)");
}

/* ---- the switch while thread 1 is in a tight loop of memory accesses ---- */
static void SwitchMidLoop(int ExpectDiscard) {
  InstallHandlers();
  uint8_t* Mem = MapGuestMemory(0x8000);
  memcpy(Mem, CountLoop, sizeof(CountLoop));
  memcpy(Mem + 0x100, SetStop, sizeof(SetStop));
  uint8_t* Data = Mem + 0x4000;
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  fexmac_context* Context = CreateContextWithOptions(DeferredOptions, COUNT(DeferredOptions), NULL, NULL);
  struct RunArgs A = {.Thread = ThreadAt(Context, Base, Base + 0x4000)};
  pthread_t Host;
  pthread_create(&Host, NULL, RunWorker, &A);
  while (Load64(Data, 0) < 100000) {
  }
  const uint64_t Before = CompiledBlocks(Context);
  const uint64_t CountAtSwitch = Load64(Data, 0);
  fexmac_thread* Second = ThreadAt(Context, Base + 0x100, Base + 0x4000);
  Check(!atomic_load(&A.Ended) && fexmac_context_ordering_deferred(Context) == 0, "switched while thread 1 was in its loop");
  const fexmac_stop Stop2 = Run(Second);
  pthread_join(Host, NULL);
  const fexmac_x86_state S = GetState(A.Thread);
  Check(Stop2.reason == FEXMAC_STOP_HALT && A.Stop.reason == FEXMAC_STOP_HALT, "both threads halt");
  Check(S.gpr[RAX] == Load64(Data, 0) && S.gpr[RAX] > CountAtSwitch,
        "thread 1 continued its loop correctly across the switch (%" PRIu64 " iterations, %" PRIu64 " at the switch)", S.gpr[RAX],
        CountAtSwitch);
  const uint64_t New = CompiledBlocks(Context) - Before;
  if (ExpectDiscard) {
    Check(New >= 2, "translations discarded: %" PRIu64 " blocks compiled after the switch (thread 2's and thread 1's loop again)", New);
  } else {
    Check(New == 1, "broken switch: only thread 2's block compiled after it (%" PRIu64 "): the counter check can fail", New);
  }
#ifdef ORDERING_SWITCH_HOOKS
  uint32_t Effective;
  uint64_t Stats[5];
  fexmac_testing_ordering_state(Context, &Effective, Stats);
  printf("    acknowledged at a resync %" PRIu64 ", at a system call or end of run %" PRIu64 ", at a run's start %" PRIu64 "; waits %" PRIu64 "\n",
         Stats[1], Stats[2], Stats[3], Stats[4]);
  if (ExpectDiscard) {
    Check(Stats[1] + Stats[2] == 1, "thread 1 acknowledged once");
  }
#endif
  fexmac_thread_destroy(Second);
  fexmac_thread_destroy(A.Thread);
  fexmac_context_destroy(Context);
  munmap(Mem, 0x8000);
}
static void TestDeferredSwitchMidLoop(void) {
  SwitchMidLoop(1);
}

/* ---- the switch while thread 1 is in a long rep movsb ----
 * The core's rep loops check the interrupt fault page every 64 elements (or 32-byte
 * run), with rcx/rsi/rdi showing exact progress, as x86 lets a rep be interrupted. So the
 * switch catches thread 1 inside the copy, it acknowledges there and finishes the copy
 * with a new translation made with ordering. x86 does not order the stores within one
 * fast string operation, so thread 2 may see the copy incomplete; what must hold is that
 * the copy completes exactly, and (hook build) that thread 1 acknowledged before thread 2
 * ran. Thread 2 reads the copy's last byte and thread 1's "copied" flag. */
/* clang-format off */
static const uint8_t LongCopy[] = {
  0x48, 0xC7, 0x43, 0x10, 0x01, 0x00, 0x00, 0x00, /* mov qword [rbx+16], 1   (started) */
  0xF3, 0xA4,                                     /* rep movsb */
  0x48, 0xC7, 0x43, 0x18, 0x01, 0x00, 0x00, 0x00, /* mov qword [rbx+24], 1   (copied) */
  0xF4,
};
static const uint8_t ReadCopy[] = {
  0x0F, 0xB6, 0x02,             /* movzx eax, byte [rdx] (the copy's last byte) */
  0x48, 0x8B, 0x4B, 0x18,       /* mov rcx, [rbx+24] */
  0xF4,
};
/* clang-format on */
static void SwitchInRepMovs(int ExpectOrdered) {
  InstallHandlers();
  const size_t CopySize = 256u << 20;
  uint8_t* Mem = MapGuestMemory(0x8000);
  uint8_t* Source = MapGuestMemory(CopySize);
  uint8_t* Dest = MapGuestMemory(CopySize);
  memset(Source, 0x5A, CopySize);
  /* Touch the destination so the copy is not slowed by first-touch faults alone. */
  memset(Dest, 0, CopySize);
  memcpy(Mem, LongCopy, sizeof(LongCopy));
  memcpy(Mem + 0x100, ReadCopy, sizeof(ReadCopy));
  uint8_t* Data = Mem + 0x4000;
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  fexmac_context* Context = CreateContextWithOptions(DeferredOptions, COUNT(DeferredOptions), NULL, NULL);
  struct RunArgs A = {.Thread = ThreadAt(Context, Base, Base + 0x4000)};
  fexmac_x86_state S = GetState(A.Thread);
  S.gpr[RSI] = (uint64_t)(uintptr_t)Source;
  S.gpr[RDI] = (uint64_t)(uintptr_t)Dest;
  S.gpr[RCX] = CopySize;
  SetState(A.Thread, &S);
  pthread_t Host;
  pthread_create(&Host, NULL, RunWorker, &A);
  while (Load64(Data, 16) == 0) {
  }
  fexmac_thread* Second = ThreadAt(Context, Base + 0x100, Base + 0x4000);
  const int InCopy = Load64(Data, 24) == 0 && Dest[CopySize - 1] == 0;
  Check(InCopy, "thread 2 created while thread 1's copy was in progress");
  S = GetState(Second);
  S.gpr[RDX] = (uint64_t)(uintptr_t)(Dest + CopySize - 1);
  SetState(Second, &S);
  const fexmac_stop Stop2 = Run(Second);
  pthread_join(Host, NULL);
  S = GetState(Second);
  Check(Stop2.reason == FEXMAC_STOP_HALT && A.Stop.reason == FEXMAC_STOP_HALT, "both threads halt");
  (void)ExpectOrdered;
  printf("    thread 2 read the copy's last byte 0x%" PRIx64 " and the copied flag %" PRIu64 "\n", S.gpr[RAX], S.gpr[RCX]);
#ifdef ORDERING_SWITCH_HOOKS
  uint32_t Effective;
  uint64_t Stats[5];
  fexmac_testing_ordering_state(Context, &Effective, Stats);
  printf("    acknowledged at a resync %" PRIu64 ", at a system call or end of run %" PRIu64 "; waits %" PRIu64 "\n", Stats[1], Stats[2], Stats[4]);
  Check(Stats[1] + Stats[2] == 1 && Stats[4] <= 1, "thread 1 acknowledged once (mid-copy at a rep loop check, or at its end)");
  if (S.gpr[RCX] == 0) {
    Check(Stats[1] == 1, "thread 2 ran before the copy ended: thread 1 had acknowledged at a check inside the rep loop");
  }
#endif
  Check(memcmp(Source, Dest, CopySize) == 0, "the copy is complete and exact (resumed part-way with the new translation)");
  fexmac_thread_destroy(Second);
  fexmac_thread_destroy(A.Thread);
  fexmac_context_destroy(Context);
  munmap(Source, CopySize);
  munmap(Dest, CopySize);
  munmap(Mem, 0x8000);
}
static void TestDeferredSwitchInRepMovs(void) {
  SwitchInRepMovs(1);
}

/* ---- the switch from thread 1's own system call callback ----
 * The callback creates thread 2, runs it on another host thread and waits for it to end
 * before returning, as a blocking clone or a spawn-and-join would. Thread 1 is in host
 * code there (the system call ended its block), so thread 2 must not wait for it: this
 * deadlocks (and the harness's alarm fires) if the system call were not a safe point. */
/* clang-format off */
static const uint8_t SyscallCreates[] = {
  0x48, 0xC7, 0x03, 0x01, 0x00, 0x00, 0x00,       /* mov qword [rbx], 1 */
  0xB8, 0x01, 0x00, 0x00, 0x00,                   /* mov eax, 1 */
  0x0F, 0x05,                                     /* syscall (the callback creates and joins thread 2) */
  0x48, 0x8B, 0x43, 0x20,                         /* mov rax, [rbx+32] (thread 2's value) */
  0xF4,
};
static const uint8_t WriteValue[] = {
  0x48, 0xC7, 0x43, 0x20, 0x4D, 0x00, 0x00, 0x00, /* mov qword [rbx+32], 77 */
  0xF4,
};
/* clang-format on */
struct CallbackRecord {
  fexmac_context* Context;
  uint64_t Base;
  int DeferredInCallback;
  int DeferredAfterCreate;
  fexmac_stop SecondStop;
};
static fexmac_syscall_action CreatingSyscall(fexmac_thread* Thread, void* UserData) {
  struct CallbackRecord* R = (struct CallbackRecord*)UserData;
  (void)Thread;
  R->DeferredInCallback = fexmac_context_ordering_deferred(R->Context);
  struct RunArgs A = {.Thread = ThreadAt(R->Context, R->Base + 0x100, R->Base + 0x4000)};
  R->DeferredAfterCreate = fexmac_context_ordering_deferred(R->Context);
  pthread_t Host;
  pthread_create(&Host, NULL, RunWorker, &A);
  pthread_join(Host, NULL);
  R->SecondStop = A.Stop;
  fexmac_thread_destroy(A.Thread);
  return FEXMAC_SYSCALL_RESUME;
}
static void TestDeferredSwitchFromCallback(void) {
  InstallHandlers();
  uint8_t* Mem = MapGuestMemory(0x8000);
  memcpy(Mem, SyscallCreates, sizeof(SyscallCreates));
  memcpy(Mem + 0x100, WriteValue, sizeof(WriteValue));
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  struct CallbackRecord R = {.Base = Base};
  fexmac_context* Context = CreateContextWithOptions(DeferredOptions, COUNT(DeferredOptions), CreatingSyscall, &R);
  R.Context = Context;
  fexmac_thread* T = ThreadAt(Context, Base, Base + 0x4000);
  const fexmac_stop Stop = Run(T);
  const fexmac_x86_state S = GetState(T);
  Check(R.DeferredInCallback == 1 && R.DeferredAfterCreate == 0, "deferred in the callback, ordering on once it created thread 2");
  Check(R.SecondStop.reason == FEXMAC_STOP_HALT && Stop.reason == FEXMAC_STOP_HALT && S.gpr[RAX] == 77,
        "thread 2 ran to its end inside thread 1's callback; thread 1 then read its value (%" PRIu64 ")", S.gpr[RAX]);
  fexmac_thread_destroy(T);
  fexmac_context_destroy(Context);
  munmap(Mem, 0x8000);
}

/* ---- fexmac_context_mark_memory_shared, seen by the embedder's own host thread ----
 * The guest writes X then Y with the same increasing value in a loop; a host thread reads
 * Y then X with acquire loads, so only reordered guest stores can show X < Y. Deferred,
 * the translation is plain stores and that outcome appears: the translation is observably
 * unordered, which is also why an embedder that touches guest memory from its own threads
 * must declare it. After fexmac_context_mark_memory_shared it must never appear. */
/* clang-format off */
static const uint8_t MPWriter[] = {
  0x48, 0xFF, 0xC0,                         /* loop: inc rax */
  0x48, 0x89, 0x03,                         /*   mov [rbx], rax (X) */
  0x48, 0x89, 0x83, 0x80, 0x00, 0x00, 0x00, /*   mov [rbx+0x80], rax (Y) */
  0x48, 0x8B, 0x8B, 0x00, 0x01, 0x00, 0x00, /*   mov rcx, [rbx+0x100] (done) */
  0x48, 0x85, 0xC9,                         /*   test rcx, rcx */
  0x74, 0xE7,                               /*   jz loop */
  0xF4,
};
/* clang-format on */
static uint64_t HostReads(const uint8_t* Data, uint64_t Reads, uint64_t StopAfter) {
  uint64_t Bad = 0;
  for (uint64_t i = 0; i < Reads && Bad < StopAfter; ++i) {
    const uint64_t Y = Load64(Data, 0x80);
    const uint64_t X = Load64(Data, 0);
    Bad += X < Y;
  }
  return Bad;
}
static void MarkSharedObserved(const fexmac_option* Options, size_t Count, int Deferred) {
  uint8_t* Mem = MapGuestMemory(0x8000);
  memcpy(Mem, MPWriter, sizeof(MPWriter));
  uint8_t* Data = Mem + 0x4000;
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  fexmac_context* Context = CreateContextWithOptions(Options, Count, NULL, NULL);
  struct RunArgs A = {.Thread = ThreadAt(Context, Base, Base + 0x4000)};
  pthread_t Host;
  pthread_create(&Host, NULL, RunWorker, &A);
  while (Load64(Data, 0) < 10000) {
  }
  const uint64_t Before = HostReads(Data, 200000000, 1);
  if (Deferred) {
    Check(Before > 0, "deferred: the host saw the guest's stores out of order (translation observably unordered)");
  } else {
    Check(Before == 0, "ordering on from the start: the host never saw the stores out of order");
  }
  CheckResult(fexmac_context_mark_memory_shared(Context), FEXMAC_OK, "fexmac_context_mark_memory_shared while the guest runs");
  Check(fexmac_context_ordering_deferred(Context) == 0, "ordering on after mark_memory_shared");
  const uint64_t After = HostReads(Data, 50000000, ~0ull);
  Check(After == 0, "after mark_memory_shared: 0 of 50M host reads saw the stores out of order (%" PRIu64 ")", After);
  Store64(Data, 0x100, 1);
  pthread_join(Host, NULL);
  Check(A.Stop.reason == FEXMAC_STOP_HALT, "the guest halts");
  fexmac_thread_destroy(A.Thread);
  fexmac_context_destroy(Context);
  munmap(Mem, 0x8000);
}
static void TestDeferredMarkShared(void) {
  InstallHandlers();
  MarkSharedObserved(DeferredOptions, COUNT(DeferredOptions), 1);
  MarkSharedObserved(OrderedOptions, COUNT(OrderedOptions), 0);
}

/* ---- the code cache: unordered and ordered translations never mix ---- */
static void CountersOf(fexmac_context* Context, uint64_t* Compiled, uint64_t* Loaded) {
  fexmac_translation_counters C;
  fexmac_translation_counters_init(&C);
  fexmac_context_get_counters(Context, &C);
  *Compiled = C.blocks_compiled;
  *Loaded = C.blocks_loaded;
}
static fexmac_context* CacheContext(const char* Directory, int Deferred) {
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = Deferred ? DeferredOptions : OrderedOptions;
  Config.option_count = Deferred ? COUNT(DeferredOptions) : COUNT(OrderedOptions);
  Config.code_cache_enabled = 1;
  Config.code_cache_directory = Directory;
  fexmac_context* Context = NULL;
  if (fexmac_context_create(&Config, &Context) != FEXMAC_OK) {
    fprintf(stderr, "cache context\n");
    exit(2);
  }
  return Context;
}
/* clang-format off */
static const uint8_t CacheSum[] = {
  0x31, 0xC0,                   /* xor eax, eax */
  0xB9, 0x64, 0x00, 0x00, 0x00, /* mov ecx, 100 */
  0x01, 0xC8,                   /* add eax, ecx */
  0x89, 0x03,                   /* mov [rbx], eax */
  0xFF, 0xC9,                   /* dec ecx */
  0x75, 0xF8,                   /* jnz -8 */
  0xF4,
};
/* clang-format on */
/* Runs the program at a fresh address in the context; returns blocks compiled and loaded
 * by that run. */
static void CacheRun(fexmac_context* Context, uint64_t* Compiled, uint64_t* Loaded, const char* What) {
  uint64_t C0, L0;
  CountersOf(Context, &C0, &L0);
  uint8_t* Mem = MapGuestMemory(0x8000);
  memcpy(Mem, CacheSum, sizeof(CacheSum));
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  CheckResult(fexmac_memory_map(Context, Base, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_EXEC), FEXMAC_OK, "register code");
  CheckResult(fexmac_memory_map(Context, Base + 0x4000, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE), FEXMAC_OK, "register data");
  fexmac_thread* T = ThreadAt(Context, Base, Base + 0x4000);
  const fexmac_stop Stop = Run(T);
  Check(Stop.reason == FEXMAC_STOP_HALT && GetState(T).gpr[RAX] == 5050, "%s: right answer", What);
  fexmac_thread_destroy(T);
  fexmac_memory_unmap(Context, Base, 0x8000);
  munmap(Mem, 0x8000);
  CountersOf(Context, Compiled, Loaded);
  *Compiled -= C0;
  *Loaded -= L0;
}
static void TestDeferredCache(void) {
  InstallHandlers();
  char Dir[128], Other[128];
  MakeTestTempDir(Dir, sizeof(Dir), "deferred-cache");
  MakeTestTempDir(Other, sizeof(Other), "deferred-cache-2");
  uint64_t C, L;
  /* 1. Cold, deferred, single-threaded: stores translations made without ordering. */
  fexmac_context* Context = CacheContext(Dir, 1);
  CacheRun(Context, &C, &L, "cold, deferred");
  Check(C > 0 && L == 0, "cold, deferred: %" PRIu64 " compiled, nothing loaded", C);
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_OK, "flush");
  fexmac_context_destroy(Context);
  /* 2. Deferred again: a single-threaded warm start loads the unordered entries. Then a
   * second thread switches and the same program runs again: the cache has only unordered
   * entries for it, so nothing may be loaded and everything is compiled, with ordering. */
  Context = CacheContext(Dir, 1);
  CacheRun(Context, &C, &L, "warm, deferred");
  Check(C == 0 && L > 0, "warm, deferred, single-threaded: %" PRIu64 " loaded, nothing compiled", L);
  fexmac_thread* Keep = CreateThreadAt(Context, 0, 0);
  Check(fexmac_context_ordering_deferred(Context) == 1, "one live thread: still deferred");
  fexmac_thread* Second = CreateThreadAt(Context, 0, 0);
  Check(fexmac_context_ordering_deferred(Context) == 0, "two live threads: switched");
  fexmac_thread_destroy(Second);
  CacheRun(Context, &C, &L, "after the switch");
  Check(C > 0 && L == 0, "after the switch: no unordered entry loaded (%" PRIu64 " compiled, %" PRIu64 " loaded)", C, L);
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_OK, "flush");
  fexmac_thread_destroy(Keep);
  fexmac_context_destroy(Context);
  /* 3. Ordering on (option off): loads the ordered entries the switched run stored. */
  Context = CacheContext(Dir, 0);
  CacheRun(Context, &C, &L, "ordered, warm");
  Check(C == 0 && L > 0, "option off: the entries stored after the switch are loaded (%" PRIu64 " loaded)", L);
  fexmac_context_destroy(Context);
  /* 4. Another directory: after only a deferred run, ordering on loads nothing. */
  Context = CacheContext(Other, 1);
  CacheRun(Context, &C, &L, "cold, deferred (second directory)");
  CheckResult(fexmac_context_flush_cache(Context), FEXMAC_OK, "flush");
  fexmac_context_destroy(Context);
  Context = CacheContext(Other, 0);
  CacheRun(Context, &C, &L, "ordered after a deferred run");
  Check(C > 0 && L == 0, "option off: no unordered entry loaded (%" PRIu64 " compiled, %" PRIu64 " loaded)", C, L);
  fexmac_context_destroy(Context);
}

/* ---- races of the switch with a run starting and with a system call ----
 * (Found by review, round 2.) A switch made between a run marking its thread Running
 * and its wait for acknowledgements must not make it wait for itself; a switch made
 * while a thread enters its system call callback must not count it while the callback
 * waits for another guest thread. Each case races 4,000 times in fresh contexts; a hang
 * is caught by the harness's alarm. */
/* clang-format off */
static const uint8_t SyscallLoop[] = {
  0xB8, 0x01, 0x00, 0x00, 0x00, /* loop: mov eax, 1 */
  0x0F, 0x05,                   /*   syscall (the callback waits until thread 2 has run) */
  0x48, 0x8B, 0x4B, 0x10,       /*   mov rcx, [rbx+16] */
  0x48, 0x85, 0xC9,             /*   test rcx, rcx */
  0x74, 0xF0,                   /*   jz loop */
  0xF4,
};
/* clang-format on */
struct RaceRecord {
  uint8_t* Data;
  atomic_int Calls;
};
static fexmac_syscall_action WaitingSyscall(fexmac_thread* Thread, void* UserData) {
  struct RaceRecord* R = (struct RaceRecord*)UserData;
  (void)Thread;
  /* After the first call, wait for thread 2's store, as a callback blocked on another
   * guest thread would. */
  if (atomic_fetch_add(&R->Calls, 1) > 0) {
    while (Load64(R->Data, 16) == 0) {
    }
  }
  return FEXMAC_SYSCALL_RESUME;
}
static void TestDeferredSwitchRaces(void) {
  InstallHandlers();
  uint8_t* Mem = MapGuestMemory(0x8000);
  memcpy(Mem, SetStop, sizeof(SetStop));
  memcpy(Mem + 0x100, SyscallLoop, sizeof(SyscallLoop));
  uint8_t* Data = Mem + 0x4000;
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  int Bad = 0;
  for (int i = 0; i < 4000; ++i) {
    /* A run of thread 1 starting while thread 2 is created. */
    Store64(Data, 16, 0);
    fexmac_context* Context = CreateContextWithOptions(DeferredOptions, COUNT(DeferredOptions), NULL, NULL);
    struct RunArgs A = {.Thread = ThreadAt(Context, Base, Base + 0x4000)};
    pthread_t Host;
    pthread_create(&Host, NULL, RunWorker, &A);
    for (volatile int Spin = 0; Spin < (i % 64) * 20; ++Spin) {
    }
    fexmac_thread* Second = ThreadAt(Context, Base, Base + 0x4000);
    Bad += Run(Second).reason != FEXMAC_STOP_HALT;
    pthread_join(Host, NULL);
    Bad += A.Stop.reason != FEXMAC_STOP_HALT;
    fexmac_thread_destroy(Second);
    fexmac_thread_destroy(A.Thread);
    fexmac_context_destroy(Context);

    /* Thread 1 in a loop of system calls whose callback waits for thread 2. */
    Store64(Data, 16, 0);
    struct RaceRecord R = {.Data = Data};
    Context = CreateContextWithOptions(DeferredOptions, COUNT(DeferredOptions), WaitingSyscall, &R);
    struct RunArgs B = {.Thread = ThreadAt(Context, Base + 0x100, Base + 0x4000)};
    pthread_create(&Host, NULL, RunWorker, &B);
    while (atomic_load(&R.Calls) == 0) {
    }
    for (volatile int Spin = 0; Spin < (i % 64) * 20; ++Spin) {
    }
    Second = ThreadAt(Context, Base, Base + 0x4000);
    Bad += Run(Second).reason != FEXMAC_STOP_HALT;
    pthread_join(Host, NULL);
    Bad += B.Stop.reason != FEXMAC_STOP_HALT;
    fexmac_thread_destroy(Second);
    fexmac_thread_destroy(B.Thread);
    fexmac_context_destroy(Context);
  }
  Check(Bad == 0, "4,000 switches racing a run's start and 4,000 racing a system call whose callback waits for thread 2: no hang, all halt");
  munmap(Mem, 0x8000);
}

#ifdef ORDERING_SWITCH_HOOKS
/* The broken switches, to show the subtests above can fail. */
static void TestBrokenNoDiscardMidLoop(void) {
  fexmac_testing_break_ordering_switch(FEXMAC_TESTING_ORDERING_NO_DISCARD);
  SwitchMidLoop(0);
  fexmac_testing_break_ordering_switch(0);
}
#endif

const struct Subtest OrderingSubtests[] = {
  {"deferred-option-off", TestDeferredOptionOff, 0, NULL, NULL, 0},
  {"deferred-lifecycle", TestDeferredLifecycle, 0, NULL, NULL, 0},
  {"deferred-cpuid", TestDeferredCPUID, 0, NULL, NULL, 0},
  {"deferred-switch-mid-loop", TestDeferredSwitchMidLoop, 0, NULL, NULL, 0},
  {"deferred-switch-rep-movs", TestDeferredSwitchInRepMovs, 0, NULL, NULL, 0},
  {"deferred-switch-from-callback", TestDeferredSwitchFromCallback, 0, NULL, NULL, 0},
  {"deferred-mark-shared", TestDeferredMarkShared, 0, NULL, NULL, 0},
  {"deferred-cache", TestDeferredCache, 0, NULL, NULL, 0},
  {"deferred-switch-races", TestDeferredSwitchRaces, 0, NULL, NULL, 0},
#ifdef ORDERING_SWITCH_HOOKS
  {"deferred-broken-no-discard-mid-loop", TestBrokenNoDiscardMidLoop, 0, NULL, NULL, 0},
#endif
};
const size_t OrderingSubtestCount = sizeof(OrderingSubtests) / sizeof(OrderingSubtests[0]);
