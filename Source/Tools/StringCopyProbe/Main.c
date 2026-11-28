// SPDX-License-Identifier: MIT
//
// Native arm64 timings of the instruction sequences a translator can use for x86
// `rep movs` / `rep stos` with memory-ordering emulation on. Standalone (no FEX code):
//
//   clang -O2 -o StringCopyProbe Source/Tools/StringCopyProbe/Main.c
//   ./StringCopyProbe                 # the default quality of service (performance cores)
//   taskpolicy -b ./StringCopyProbe   # background: efficiency cores
//
// Each variant copies (or fills) N bytes, N = 16, 64, 4096 and 1 MiB, from an 8-aligned
// source to an 8-aligned destination 64 KiB further on (no overlap), many times; the
// report is nanoseconds per call (minimum of 7 timed batches) and bytes per nanosecond.
//
//   movsb-elem     ldaprb + stlrb per byte (the current rep movsb with ordering on)
//   movsq-elem     ldapr + stlr per 8 bytes (the current rep movsq with ordering on)
//   wide-acqrel    4 x ldapur + 4 x stlur per 32 bytes: 8-byte acquire loads and release
//                  stores, every store still ordered after all earlier accesses
//   wide-window    as wide-acqrel with a source 3 bytes off 8-byte alignment: aligned
//                  8-byte acquire loads, two shifts and an or per word, release stores
//   wide-dmbst-stp 2 x ldapur, dmb ishst, stp per 16 bytes (16-byte single-copy atomic
//                  stores, one barrier each)
//   plain          ldp/stp of q registers per 32 bytes (string ordering off)
//   bracketed      dmb ish, plain, dmb ish (what the Intel-only relaxation would cost)
//   stosb-elem     stlrb per byte;  stos-wide  stlur 8 bytes x 4;  stos-plain  stp q
//   barrier pair   dmb ishst + dmb ishld with nothing between (the fixed cost of brackets)
//
// Also the processor each batch ran on, from the cluster id in TPIDRRO_EL0 as macOS
// reports it is not available to user code, so the tool prints the QoS it ran under.
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>

typedef void (*CopyFn)(uint8_t* d, const uint8_t* s, uint64_t n);

static void movsb_elem(uint8_t* d, const uint8_t* s, uint64_t n) {
  __asm__ volatile("cbz %2, 2f\n"
                   "1: ldaprb w9, [%1]\n"
                   "stlrb w9, [%0]\n"
                   "add %0, %0, #1\n"
                   "add %1, %1, #1\n"
                   "sub %2, %2, #1\n"
                   "cbnz %2, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(s), "+r"(n)
                   :
                   : "x9", "memory");
}

static void movsq_elem(uint8_t* d, const uint8_t* s, uint64_t n) {
  n /= 8;
  __asm__ volatile("cbz %2, 2f\n"
                   "1: ldapr x9, [%1]\n"
                   "stlr x9, [%0]\n"
                   "add %0, %0, #8\n"
                   "add %1, %1, #8\n"
                   "sub %2, %2, #1\n"
                   "cbnz %2, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(s), "+r"(n)
                   :
                   : "x9", "memory");
}

static void wide_acqrel(uint8_t* d, const uint8_t* s, uint64_t n) {
  uint64_t c = n / 32;
  __asm__ volatile("cbz %2, 2f\n"
                   "1: ldapur x9, [%1, #0]\n"
                   "ldapur x10, [%1, #8]\n"
                   "ldapur x11, [%1, #16]\n"
                   "ldapur x12, [%1, #24]\n"
                   "stlur x9, [%0, #0]\n"
                   "stlur x10, [%0, #8]\n"
                   "stlur x11, [%0, #16]\n"
                   "stlur x12, [%0, #24]\n"
                   "add %0, %0, #32\n"
                   "add %1, %1, #32\n"
                   "sub %2, %2, #1\n"
                   "cbnz %2, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(s), "+r"(c)
                   :
                   : "x9", "x10", "x11", "x12", "memory");
  for (uint64_t i = n & ~31ull; i < n; ++i) {
    __atomic_store_n(d + (i - (n & ~31ull)), __atomic_load_n(s + (i - (n & ~31ull)), __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
  }
}

// Source s is 3 bytes past an 8-aligned address. Aligned words are read once each, in
// ascending order; each destination word is (prev >> 24) | (next << 40).
static void wide_window(uint8_t* d, const uint8_t* s, uint64_t n) {
  uint64_t c = n / 32;
  const uint8_t* a = (const uint8_t*)((uintptr_t)s & ~7ull);
  uint64_t sh = ((uintptr_t)s & 7) * 8;
  __asm__ volatile("cbz %2, 2f\n"
                   "ldapr x9, [%1]\n"
                   "add %1, %1, #8\n"
                   "neg x15, %3\n"
                   "1: ldapur x10, [%1, #0]\n"
                   "ldapur x11, [%1, #8]\n"
                   "ldapur x12, [%1, #16]\n"
                   "ldapur x13, [%1, #24]\n"
                   "lsr x14, x9, %3\n"
                   "lsl x16, x10, x15\n"
                   "orr x14, x14, x16\n"
                   "stlur x14, [%0, #0]\n"
                   "lsr x14, x10, %3\n"
                   "lsl x16, x11, x15\n"
                   "orr x14, x14, x16\n"
                   "stlur x14, [%0, #8]\n"
                   "lsr x14, x11, %3\n"
                   "lsl x16, x12, x15\n"
                   "orr x14, x14, x16\n"
                   "stlur x14, [%0, #16]\n"
                   "lsr x14, x12, %3\n"
                   "lsl x16, x13, x15\n"
                   "orr x14, x14, x16\n"
                   "stlur x14, [%0, #24]\n"
                   "mov x9, x13\n"
                   "add %0, %0, #32\n"
                   "add %1, %1, #32\n"
                   "sub %2, %2, #1\n"
                   "cbnz %2, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(a), "+r"(c)
                   : "r"(sh)
                   : "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "memory");
}

static void wide_dmbst_stp(uint8_t* d, const uint8_t* s, uint64_t n) {
  uint64_t c = n / 16;
  __asm__ volatile("cbz %2, 2f\n"
                   "1: ldapur x9, [%1, #0]\n"
                   "ldapur x10, [%1, #8]\n"
                   "dmb ishst\n"
                   "stp x9, x10, [%0]\n"
                   "add %0, %0, #16\n"
                   "add %1, %1, #16\n"
                   "sub %2, %2, #1\n"
                   "cbnz %2, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(s), "+r"(c)
                   :
                   : "x9", "x10", "memory");
}

static void plain(uint8_t* d, const uint8_t* s, uint64_t n) {
  uint64_t c = n / 32;
  __asm__ volatile("cbz %2, 2f\n"
                   "1: ldp q0, q1, [%1], #32\n"
                   "stp q0, q1, [%0], #32\n"
                   "sub %2, %2, #1\n"
                   "cbnz %2, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(s), "+r"(c)
                   :
                   : "v0", "v1", "memory");
  if (n < 32) {
    for (uint64_t i = 0; i < n; ++i) {
      ((volatile uint8_t*)d)[i] = s[i];
    }
  }
}

static void bracketed(uint8_t* d, const uint8_t* s, uint64_t n) {
  __asm__ volatile("dmb ish" ::: "memory");
  plain(d, s, n);
  __asm__ volatile("dmb ish" ::: "memory");
}

static void stosb_elem(uint8_t* d, const uint8_t* s, uint64_t n) {
  (void)s;
  __asm__ volatile("cbz %1, 2f\n"
                   "mov w9, #0x5a\n"
                   "1: stlrb w9, [%0]\n"
                   "add %0, %0, #1\n"
                   "sub %1, %1, #1\n"
                   "cbnz %1, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(n)
                   :
                   : "x9", "memory");
}

static void stos_wide(uint8_t* d, const uint8_t* s, uint64_t n) {
  (void)s;
  uint64_t c = n / 32;
  __asm__ volatile("cbz %1, 2f\n"
                   "mov x9, #0x5a5a\n"
                   "1: stlur x9, [%0, #0]\n"
                   "stlur x9, [%0, #8]\n"
                   "stlur x9, [%0, #16]\n"
                   "stlur x9, [%0, #24]\n"
                   "add %0, %0, #32\n"
                   "sub %1, %1, #1\n"
                   "cbnz %1, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(c)
                   :
                   : "x9", "memory");
  for (uint64_t i = n & ~31ull; i < n; ++i) {
    __atomic_store_n(d + i - (n & ~31ull), 0x5a, __ATOMIC_RELEASE);
  }
}

static void stos_plain(uint8_t* d, const uint8_t* s, uint64_t n) {
  (void)s;
  uint64_t c = n / 32;
  __asm__ volatile("cbz %1, 2f\n"
                   "movi v0.16b, #0x5a\n"
                   "1: stp q0, q0, [%0], #32\n"
                   "sub %1, %1, #1\n"
                   "cbnz %1, 1b\n"
                   "2:\n"
                   : "+r"(d), "+r"(c)
                   :
                   : "v0", "memory");
  if (n < 32) {
    for (uint64_t i = 0; i < n; ++i) {
      ((volatile uint8_t*)d)[i] = 0x5a;
    }
  }
}

static void barrier_pair(uint8_t* d, const uint8_t* s, uint64_t n) {
  (void)d;
  (void)s;
  (void)n;
  __asm__ volatile("dmb ishst\ndmb ishld" ::: "memory");
}

static double Now(void) {
  static mach_timebase_info_data_t TB;
  if (!TB.denom) {
    mach_timebase_info(&TB);
  }
  return (double)mach_absolute_time() * TB.numer / TB.denom;
}

struct Variant {
  const char* Name;
  CopyFn Fn;
  int SrcOff;
};

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  qos_class_t Q;
  int Rel;
  pthread_get_qos_class_np(pthread_self(), &Q, &Rel);
  printf("qos class 0x%x (0x21 user-interactive, 0x19 user-initiated, 0x15 default, 0x11 utility, 0x09 background)\n", Q);
  const uint64_t Region = 1 << 21;
  uint8_t* Buf = aligned_alloc(16384, 3 * Region);
  memset(Buf, 1, 3 * Region);
  uint8_t* Src = Buf;
  uint8_t* Dst = Buf + Region + 65536;
  const struct Variant V[] = {
    {"movsb-elem", movsb_elem, 0},    {"movsq-elem", movsq_elem, 0},     {"wide-acqrel", wide_acqrel, 0},
    {"wide-window", wide_window, 3},  {"wide-dmbst-stp", wide_dmbst_stp, 0}, {"plain", plain, 0},
    {"bracketed", bracketed, 0},      {"stosb-elem", stosb_elem, 0},     {"stos-wide", stos_wide, 0},
    {"stos-plain", stos_plain, 0},    {"barrier-pair", barrier_pair, 0},
  };
  const uint64_t Sizes[] = {16, 64, 4096, 1 << 20};
  printf("%-15s", "variant");
  for (unsigned k = 0; k < 4; ++k) {
    printf(" %10llu B ns  B/ns ", (unsigned long long)Sizes[k]);
  }
  printf("\n");
  for (unsigned v = 0; v < sizeof(V) / sizeof(V[0]); ++v) {
    printf("%-15s", V[v].Name);
    for (unsigned k = 0; k < 4; ++k) {
      const uint64_t N = Sizes[k];
      const uint64_t Calls = N >= (1 << 20) ? 20 : (N >= 4096 ? 4000 : 200000);
      double Best = 1e30;
      for (int b = 0; b < 7; ++b) {
        double T0 = Now();
        for (uint64_t c = 0; c < Calls; ++c) {
          V[v].Fn(Dst, Src + V[v].SrcOff, N);
        }
        double T = (Now() - T0) / Calls;
        if (T < Best) {
          Best = T;
        }
      }
      printf(" %12.1f %6.2f", Best, N / Best);
    }
    printf("\n");
  }
  // Check that the window copy is right.
  memset(Dst, 0, 4096);
  for (int i = 0; i < 4200; ++i) {
    Src[i] = (uint8_t)(i * 7 + 1);
  }
  wide_window(Dst, Src + 3, 4096);
  if (memcmp(Dst, Src + 3, 4096) != 0) {
    printf("wide-window copy WRONG\n");
    return 1;
  }
  printf("wide-window copy checked\n");
  return 0;
}
