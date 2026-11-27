/* SPDX-License-Identifier: MIT */
/*
 * The freestanding x86-64 guest's side of the benchmark (see bench.h). No libc: every
 * service is a `syscall` with the benchmark's private ABI, which FEXCoreBench
 * (Source/Tools/CoreBench/Main.cpp) answers. This is not the Linux or macOS system call
 * ABI; the numbers are chosen so they cannot be mistaken for either.
 *
 *   rax              arguments                         result in rax
 *   BENCH_SYS_EXIT   rdi = number of wrong checksums,  (does not return)
 *                    rsi = number of kernels run
 *   BENCH_SYS_WRITE  rdi = address, rsi = length       bytes written
 *   BENCH_SYS_TIME   -                                 nanoseconds, CLOCK_UPTIME_RAW
 *   BENCH_SYS_SPAWN  rdi = entry, rsi = stack top,     thread handle, or -1
 *                    rdx = first argument, r10 = second
 *   BENCH_SYS_JOIN   rdi = handle                      0, or -1
 *   BENCH_SYS_THREAD_EXIT                              (does not return)
 *
 * The loader starts the image at bench_start with rdi = flags (BENCH_QUICK,
 * BENCH_LITERAL), rsi = repetitions and rdx = the address of the kernel filter string
 * (0 for all) and rsp at the top of the stack it mapped. In the literal mode the guest
 * makes exactly one system call: BENCH_SYS_EXIT.
 */
#include "bench.h"

#define BENCH_SYS_EXIT 0xBE00
#define BENCH_SYS_WRITE 0xBE01
#define BENCH_SYS_TIME 0xBE02
#define BENCH_SYS_SPAWN 0xBE03
#define BENCH_SYS_JOIN 0xBE04
#define BENCH_SYS_THREAD_EXIT 0xBE05

static inline uint64_t sys1(uint64_t n, uint64_t a) {
  uint64_t r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory");
  return r;
}

static inline uint64_t sys4(uint64_t n, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
  uint64_t r;
  register uint64_t r10 __asm__("r10") = d;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
  return r;
}

uint64_t bench_time_ns(void) {
  return sys1(BENCH_SYS_TIME, 0);
}

void bench_write(const char* text, size_t len) {
  sys4(BENCH_SYS_WRITE, (uint64_t)text, len, 0, 0);
}

const char* bench_platform(void) {
  return "guest-x86_64";
}

/* Guest thread stacks are static: the guest allocates nothing. One per live thread. */
#define THREAD_STACK (1u << 20)
static uint8_t g_stacks[BENCH_MAX_THREADS][THREAD_STACK] __attribute__((aligned(64)));
static int g_stack_owner[BENCH_MAX_THREADS] = {-1, -1, -1, -1, -1, -1, -1, -1};

/* A new thread starts here with rdi = fn and rsi = arg and a 16-byte aligned stack. */
void bench_thread_entry(void);
__asm__(".text\n"
        ".globl _bench_thread_entry\n"
        "_bench_thread_entry:\n"
        "  movq %rdi, %rax\n"
        "  movq %rsi, %rdi\n"
        "  callq *%rax\n"
        "  movl $0xBE05, %eax\n"
        "  syscall\n"
        "  hlt\n");

int bench_thread_start(void (*fn)(void*), void* arg) {
  /* Only the main thread starts and joins threads, so no lock is needed here. */
  for (int i = 0; i < BENCH_MAX_THREADS; ++i) {
    if (g_stack_owner[i] < 0) {
      const uint64_t top = (uint64_t)(g_stacks[i] + THREAD_STACK);
      const int64_t h = (int64_t)sys4(BENCH_SYS_SPAWN, (uint64_t)bench_thread_entry, top, (uint64_t)fn, (uint64_t)arg);
      if (h < 0) {
        return -1;
      }
      g_stack_owner[i] = (int)h;
      return (int)h;
    }
  }
  return -1;
}

void bench_thread_join(int handle) {
  sys1(BENCH_SYS_JOIN, (uint64_t)(int64_t)handle);
  for (int i = 0; i < BENCH_MAX_THREADS; ++i) {
    if (g_stack_owner[i] == handle) {
      g_stack_owner[i] = -1;
    }
  }
}

/* The compiler may call these for structure copies and initialisation even in a
 * freestanding build. They use the string instructions, as a C library would. */
void* memcpy(void* d, const void* s, size_t n) {
  void* r = d;
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  return r;
}

void* memmove(void* d, const void* s, size_t n) {
  if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) {
    return memcpy(d, s, n);
  }
  uint8_t* dd = (uint8_t*)d + n;
  const uint8_t* ss = (const uint8_t*)s + n;
  while (n--) {
    *--dd = *--ss;
  }
  return d;
}

void* memset(void* d, int v, size_t n) {
  void* r = d;
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
  return r;
}

void __bzero(void* d, size_t n) {
  memset(d, 0, n);
}

void bench_start(uint64_t flags, uint64_t reps, const char* only) __attribute__((noreturn));
void bench_start(uint64_t flags, uint64_t reps, const char* only) {
  int ran = 0;
  const int failures = bench_main((int)flags, (int)reps, only, &ran);
  sys4(BENCH_SYS_EXIT, (uint64_t)failures, (uint64_t)ran, 0, 0);
  for (;;) {
    __asm__ volatile("hlt");
  }
}

/* The image's entry point (named with -e): align the stack and call bench_start. */
__asm__(".text\n"
        ".globl _bench_entry\n"
        "_bench_entry:\n"
        "  andq $-16, %rsp\n"
        "  callq _bench_start\n"
        "  hlt\n");
