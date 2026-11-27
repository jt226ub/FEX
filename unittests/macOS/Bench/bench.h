/* SPDX-License-Identifier: MIT */
/*
 * The benchmark's interface to whatever runs it.
 *
 * The kernels (kernels.c) are the same C source in every build. They see the outside
 * world only through the functions below, which each platform file provides:
 *
 *   platform_guest.c   the freestanding x86-64 guest image run by FEXCoreBench. No libc;
 *                      each function is one `syscall` with the benchmark's private ABI
 *                      (see Source/Tools/CoreBench/Main.cpp), answered by the loader.
 *   platform_native.c  an ordinary macOS program (x86-64 under Rosetta 2, or arm64) using
 *                      libc and pthreads.
 *
 * In the timed mode the guest's host surface is: exit, write bytes to standard output,
 * read a monotonic clock, start a guest thread and wait for it. In the literal mode
 * (BENCH_LITERAL) it is exit only. Nothing else in either (no files, no memory
 * allocation: all memory is static).
 */
#ifndef BENCH_H
#define BENCH_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic time in nanoseconds (the host's CLOCK_UPTIME_RAW in every build). */
uint64_t bench_time_ns(void);

/* Writes len bytes to standard output. */
void bench_write(const char* text, size_t len);

/* Starts fn(arg) on a new thread; returns a handle for bench_thread_join, or -1. At most
 * BENCH_MAX_THREADS threads exist at a time. */
#define BENCH_MAX_THREADS 8
int bench_thread_start(void (*fn)(void*), void* arg);
void bench_thread_join(int handle);

/* Short name of the build, printed in the START line. */
const char* bench_platform(void);

/* The driver (kernels.c).
 *
 * flags: BENCH_QUICK selects the small sizes (the correctness check). BENCH_LITERAL is
 * the mode that meets exit criterion 7 to the letter: one run of every single-threaded
 * kernel, and no host service at all (no output, no clock, no threads); the result is
 * only the return value. Without it, the timed mode writes its results and reads the
 * clock, and the thread kernels start guest threads.
 *
 * reps: timed repetitions after one warm-up run (ignored in the literal mode). only:
 * NULL or a comma-separated list of kernel names; an unknown name is an error (returns
 * 1, runs nothing). Returns the number of kernels whose checksum is not the recorded
 * one; *ran is the number of kernels run. */
#define BENCH_QUICK 1
#define BENCH_LITERAL 2
int bench_main(int flags, int reps, const char* only, int* ran);

#endif
