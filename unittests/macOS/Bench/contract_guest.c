/* SPDX-License-Identifier: MIT */
/*
 * A guest that breaks FEXCoreBench's contract on purpose, one way per run, to test that
 * the loader reports each as a failure. Linked with platform_guest.c in place of
 * kernels.c; the case is chosen with --only. Run by Scripts/macos_bench.py --quick.
 *
 *   ok             exits normally, 0 wrong, 1 kernel run
 *   unjoined       starts a thread that never ends and exits without joining it
 *   worker-exit    a started thread calls exit (only the main thread may)
 *   main-thread-exit  the main thread calls thread exit
 *   bad-syscall    an unknown system call number
 *   write          writes output (refused in the literal mode)
 *   ud2            an invalid instruction: the process must die with SIGILL
 */
#include "bench.h"

static int eq(const char* a, const char* b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

static volatile int g_forever = 1;

static void spin(void* arg) {
  (void)arg;
  while (g_forever) {}
}

static void call_exit(void* arg) {
  (void)arg;
  __asm__ volatile("syscall" : : "a"(0xBE00), "D"(0), "S"(1) : "rcx", "r11", "memory");
}

int bench_main(int flags, int reps, const char* only, int* ran) {
  (void)flags;
  (void)reps;
  *ran = 1;
  if (!only || eq(only, "ok")) {
    return 0;
  }
  if (eq(only, "unjoined")) {
    bench_thread_start(spin, 0);
    return 0;
  }
  if (eq(only, "worker-exit")) {
    bench_thread_join(bench_thread_start(call_exit, 0));
    return 0;
  }
  if (eq(only, "main-thread-exit")) {
    __asm__ volatile("syscall" : : "a"(0xBE05) : "rcx", "r11", "memory");
    return 0;
  }
  if (eq(only, "bad-syscall")) {
    __asm__ volatile("syscall" : : "a"(60), "D"(0) : "rcx", "r11", "memory");
    return 0;
  }
  if (eq(only, "write")) {
    bench_write("hello\n", 6);
    return 0;
  }
  if (eq(only, "ud2")) {
    __asm__ volatile("ud2");
  }
  return 1;
}
