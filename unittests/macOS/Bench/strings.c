/* SPDX-License-Identifier: MIT */
/*
 * Microbenchmark of x86 string instructions (round 2, task E2): rep movsb, rep movsq,
 * rep stosb and rep stosq at 16 B, 64 B, 4 KB and 1 MB, plus rep movsb with the source
 * 3 bytes off the destination's 8-byte alignment. x86-64 only (the instructions are
 * written as inline assembly); built by Scripts/macos_string_bench.py as a guest image
 * for FEXCoreBench (with platform_guest.c) and as a macOS program for Rosetta 2 (with
 * platform_native.c).
 *
 * Output, one line per case: "S <name> <bytes> <ns per instruction> <checksum>", the
 * time the best of `reps` timed batches (after one warm-up batch); then "DONE". The
 * checksum covers the destination after the last batch and must be the same in every
 * build and configuration. Returns the number of wrong destination checks.
 */
#include "bench.h"

#define BUF (2u << 20)
static uint8_t g_src[BUF + 64] __attribute__((aligned(64)));
static uint8_t g_dst[BUF + 64] __attribute__((aligned(64)));

typedef struct {
  char data[160];
  size_t len;
} line_t;

static void put_str(line_t* l, const char* s) {
  while (*s && l->len < sizeof(l->data) - 1) {
    l->data[l->len++] = *s++;
  }
}
static void put_u64(line_t* l, uint64_t v) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n && l->len < sizeof(l->data) - 1) {
    l->data[l->len++] = tmp[--n];
  }
}
static void put_hex(line_t* l, uint64_t v) {
  for (int i = 60; i >= 0; i -= 4) {
    l->data[l->len++] = "0123456789abcdef"[(v >> i) & 15];
  }
}
static void put_end(line_t* l) {
  l->data[l->len++] = '\n';
  bench_write(l->data, l->len);
  l->len = 0;
}

static void movsb(uint8_t* d, const uint8_t* s, uint64_t n) {
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static void movsq(uint8_t* d, const uint8_t* s, uint64_t n) {
  n /= 8;
  __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static void stosb(uint8_t* d, const uint8_t* s, uint64_t n) {
  (void)s;
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(0x5a) : "memory");
}
static void stosq(uint8_t* d, const uint8_t* s, uint64_t n) {
  (void)s;
  n /= 8;
  __asm__ volatile("rep stosq" : "+D"(d), "+c"(n) : "a"(0x5a5a5a5a5a5a5a5aull) : "memory");
}

static void movsd(uint8_t* d, const uint8_t* s, uint64_t n) {
  n /= 4;
  __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}
static void movsb_back(uint8_t* d, const uint8_t* s, uint64_t n) {
  d += n - 1;
  s += n - 1;
  __asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

typedef struct {
  const char* name;
  void (*fn)(uint8_t*, const uint8_t*, uint64_t);
  uint32_t src_off;
  uint32_t dst_off;
  int overlap; /* source is the destination buffer, dst_off bytes below the destination */
} op_t;

static uint64_t fnv(const uint8_t* p, uint64_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint64_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
}

int bench_main(int flags, int reps, const char* only, int* ran) {
  (void)only;
  const int quick = (flags & BENCH_QUICK) != 0;
  line_t l = {{0}, 0};
  int bad = 0;
  *ran = 0;
  if (reps < 1) {
    reps = 1;
  }
  put_str(&l, "START ");
  put_str(&l, bench_platform());
  put_end(&l);
  for (uint32_t i = 0; i < BUF + 64; ++i) {
    g_src[i] = (uint8_t)(i * 131 + 7);
  }
  static const op_t ops[] = {
    {"movsb", movsb, 0, 0, 0},
    {"movsq", movsq, 0, 0, 0},
    {"stosb", stosb, 0, 0, 0},
    {"stosq", stosq, 0, 0, 0},
    /* Shapes of the benchmark's rep_movs kernel: a source off the destination's
     * alignment, misaligned elements, overlapping copies forward and backward. */
    {"movsb_src+3", movsb, 3, 0, 0},
    {"movsq_dst+3", movsq, 0, 3, 0},
    {"movsd_dst+2", movsd, 0, 2, 0},
    {"movsb_overlap+3", movsb, 0, 3, 1},
    {"movsb_back_overlap+5", movsb_back, 0, 5, 1},
  };
  static const uint64_t sizes[] = {16, 64, 4096, 1u << 20};
  for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o) {
    for (unsigned z = 0; z < 4; ++z) {
      const uint64_t n = sizes[z];
      if ((ops[o].src_off || ops[o].dst_off) && n < 4096) {
        continue;
      }
      uint8_t* const d = g_dst + ops[o].dst_off;
      const uint8_t* const src = ops[o].overlap ? g_dst : g_src + ops[o].src_off;
      /* About 8 MB per batch (2 MB in the quick mode), at least 64 instructions. */
      uint64_t calls = ((quick ? 2u : 8u) << 20) / n;
      if (calls > (quick ? 20000u : 200000u)) {
        calls = quick ? 20000u : 200000u;
      }
      uint64_t best = ~0ull;
      for (int r = 0; r <= reps; ++r) {
        for (uint64_t i = 0; i < n + 64; ++i) {
          g_dst[i] = ops[o].overlap ? (uint8_t)(i * 7 + 1) : 0;
        }
        const uint64_t t0 = bench_time_ns();
        for (uint64_t c = 0; c < calls; ++c) {
          ops[o].fn(d, src, n);
        }
        const uint64_t t = bench_time_ns() - t0;
        if (r > 0 && t < best) {
          best = t;
        }
      }
      /* The x86 result: the copy or fill, nothing past it (overlapping copies are
       * checked through the checksum, which every build must agree on). */
      for (uint64_t i = 0; !ops[o].overlap && i < n + 64; ++i) {
        const uint64_t j = i - ops[o].dst_off;
        const uint8_t want = i < ops[o].dst_off || j >= n ? 0 : (ops[o].fn == stosb || ops[o].fn == stosq) ? 0x5a : g_src[j + ops[o].src_off];
        if (g_dst[i] != want) {
          ++bad;
          break;
        }
      }
      ++*ran;
      put_str(&l, "S ");
      put_str(&l, ops[o].name);
      put_str(&l, " ");
      put_u64(&l, n);
      put_str(&l, " ");
      /* ns per instruction, in hundredths. */
      put_u64(&l, best * 100 / calls);
      put_str(&l, " ");
      put_hex(&l, fnv(g_dst, n + 64));
      put_end(&l);
    }
  }
  put_str(&l, "DONE ");
  put_u64(&l, (uint64_t)bad);
  put_end(&l);
  return bad;
}
