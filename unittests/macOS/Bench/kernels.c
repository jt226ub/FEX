/* SPDX-License-Identifier: MIT */
/*
 * Benchmark kernels and driver. Written for this port; no code here is taken from
 * another benchmark suite. The same file is compiled for the x86-64 guest image, for
 * x86-64 macOS (run under Rosetta 2) and for arm64 macOS, with the same flags apart from
 * the target (see Scripts/macos_bench.py).
 *
 * Every kernel returns a 64-bit checksum of everything it computed, which must be equal
 * in all three builds. Floating point is kept bit-exact: only IEEE operations that are
 * correctly rounded on both SSE2 and NEON (+ - * / sqrt, conversions), no library
 * functions (sin and cos are our own series), and -ffp-contract=off so that no build
 * fuses a multiply and an add. The one exception is the x87 kernel: `long double` is the
 * 80-bit x87 format on x86-64 but plain double on arm64, so its checksum is compared
 * between the two x86-64 builds only, and arm64 has its own expected value.
 *
 * The random numbers come from splitmix64 (Steele, Lea and Flood, 2014), re-implemented
 * here; the algorithm's constants are in the public domain.
 */
#include "bench.h"

#define NOINLINE __attribute__((noinline))

/* ------------------------------------------------------------------------------- */
/* Helpers                                                                          */
/* ------------------------------------------------------------------------------- */

static uint64_t splitmix(uint64_t* state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static uint64_t mix(uint64_t h, uint64_t v) {
  h ^= v;
  h *= 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}

static uint64_t bits_of_double(double d) {
  uint64_t u;
  __builtin_memcpy(&u, &d, sizeof(u));
  return u;
}

static uint64_t bits_of_float(float f) {
  uint32_t u;
  __builtin_memcpy(&u, &f, sizeof(u));
  return u;
}

static void cpu_relax(void) {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ volatile("yield");
#endif
}

/* Output without libc. */
typedef struct {
  char data[256];
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
  static const char digits[] = "0123456789abcdef";
  for (int i = 60; i >= 0; i -= 4) {
    if (l->len < sizeof(l->data) - 1) {
      l->data[l->len++] = digits[(v >> i) & 15];
    }
  }
}

static void put_end(line_t* l) {
  l->data[l->len++] = '\n';
  bench_write(l->data, l->len);
  l->len = 0;
}

static int str_eq_n(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      return 0;
    }
  }
  return b[n] == 0;
}

/* ------------------------------------------------------------------------------- */
/* Integer: CRC-32 and a multiply-xorshift hash                                     */
/* ------------------------------------------------------------------------------- */

#define BYTES_MAX (1u << 20)
static uint8_t g_bytes[BYTES_MAX];
static uint32_t g_crc_table[256];

static void setup_bytes(int quick) {
  (void)quick;
  uint64_t s = 1;
  for (uint32_t i = 0; i < BYTES_MAX; i += 8) {
    uint64_t v = splitmix(&s);
    for (int j = 0; j < 8; ++j) {
      g_bytes[i + j] = (uint8_t)(v >> (8 * j));
    }
  }
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    g_crc_table[i] = c;
  }
}

static uint64_t run_crc32(int quick) {
  const int passes = quick ? 2 : 24;
  uint64_t h = 0;
  for (int p = 0; p < passes; ++p) {
    uint32_t crc = 0xFFFFFFFFu ^ (uint32_t)p;
    for (uint32_t i = 0; i < BYTES_MAX; ++i) {
      crc = g_crc_table[(crc ^ g_bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    h = mix(h, crc ^ 0xFFFFFFFFu);
  }
  return h;
}

static uint64_t run_hash64(int quick) {
  const int passes = quick ? 4 : 192;
  uint64_t h = 0;
  for (int p = 0; p < passes; ++p) {
    uint64_t a = 0x243F6A8885A308D3ull + (uint64_t)p, b = 0x13198A2E03707344ull;
    for (uint32_t i = 0; i < BYTES_MAX; i += 16) {
      uint64_t x, y;
      __builtin_memcpy(&x, &g_bytes[i], 8);
      __builtin_memcpy(&y, &g_bytes[i + 8], 8);
      unsigned __int128 m = (unsigned __int128)(x ^ a) * (y ^ b);
      a = (uint64_t)m ^ (a >> 17);
      b = (uint64_t)(m >> 64) + (b << 5) + i;
    }
    h = mix(h, a ^ b);
  }
  return h;
}

/* ------------------------------------------------------------------------------- */
/* Integer: quicksort                                                               */
/* ------------------------------------------------------------------------------- */

#define SORT_MAX 200000
static uint64_t g_sort[SORT_MAX];

static void insertion_sort(uint64_t* a, long n) {
  for (long i = 1; i < n; ++i) {
    uint64_t v = a[i];
    long j = i - 1;
    while (j >= 0 && a[j] > v) {
      a[j + 1] = a[j];
      --j;
    }
    a[j + 1] = v;
  }
}

static NOINLINE void quick_sort(uint64_t* a, long n) {
  while (n > 16) {
    uint64_t x = a[0], y = a[n / 2], z = a[n - 1];
    uint64_t pivot = x < y ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
    long i = 0, j = n - 1;
    for (;;) {
      while (a[i] < pivot) {
        ++i;
      }
      while (a[j] > pivot) {
        --j;
      }
      if (i >= j) {
        break;
      }
      uint64_t t = a[i];
      a[i] = a[j];
      a[j] = t;
      ++i;
      --j;
    }
    /* Recurse on the smaller side, loop on the larger. */
    long left = j + 1;
    if (left < n - left) {
      quick_sort(a, left);
      a += left;
      n -= left;
    } else {
      quick_sort(a + left, n - left);
      n = left;
    }
  }
  insertion_sort(a, n);
}

static uint64_t run_sort(int quick) {
  const long n = quick ? 20000 : SORT_MAX;
  const int rounds = quick ? 1 : 3;
  uint64_t h = 0;
  for (int r = 0; r < rounds; ++r) {
    uint64_t s = 42 + (uint64_t)r;
    for (long i = 0; i < n; ++i) {
      g_sort[i] = splitmix(&s) >> (r * 8); /* later rounds have more duplicates */
    }
    quick_sort(g_sort, n);
    for (long i = 0; i < n; ++i) {
      h = h * 31 + g_sort[i];
      if (i && g_sort[i - 1] > g_sort[i]) {
        h ^= 0xBAD; /* not sorted */
      }
    }
  }
  return h;
}

/* ------------------------------------------------------------------------------- */
/* Integer: a register-machine interpreter (switch dispatch + indirect calls)       */
/* ------------------------------------------------------------------------------- */

enum { OP_LI, OP_ADD, OP_XOR, OP_MUL, OP_SHRI, OP_ANDI, OP_ADDI, OP_JNZ, OP_CALL, OP_HALT };
typedef struct {
  uint8_t op, a, b, c;
  int32_t imm;
} insn_t;

static NOINLINE uint64_t vm_f0(uint64_t x, uint64_t i) {
  return x * 6364136223846793005ull + i;
}
static NOINLINE uint64_t vm_f1(uint64_t x, uint64_t i) {
  return (x ^ (x >> 13)) + (i << 3);
}
static NOINLINE uint64_t vm_f2(uint64_t x, uint64_t i) {
  return (x << 7 | x >> 57) ^ i;
}
static NOINLINE uint64_t vm_f3(uint64_t x, uint64_t i) {
  return x + 0x9E3779B97F4A7C15ull - i;
}
static uint64_t (*const vm_funcs[4])(uint64_t, uint64_t) = {vm_f0, vm_f1, vm_f2, vm_f3};

static NOINLINE uint64_t vm_run(const insn_t* prog, uint64_t* r) {
  const insn_t* pc = prog;
  for (;;) {
    const insn_t in = *pc++;
    switch (in.op) {
    case OP_LI: r[in.a] = (uint64_t)(int64_t)in.imm; break;
    case OP_ADD: r[in.a] = r[in.b] + r[in.c]; break;
    case OP_XOR: r[in.a] = r[in.b] ^ r[in.c]; break;
    case OP_MUL: r[in.a] = r[in.b] * r[in.c]; break;
    case OP_SHRI: r[in.a] = r[in.b] >> in.imm; break;
    case OP_ANDI: r[in.a] = r[in.b] & (uint64_t)(int64_t)in.imm; break;
    case OP_ADDI: r[in.a] = r[in.b] + (uint64_t)(int64_t)in.imm; break;
    case OP_JNZ:
      if (r[in.a]) {
        pc = prog + in.imm;
      }
      break;
    case OP_CALL: r[in.a] = vm_funcs[r[in.b] & 3](r[in.a], r[in.b]); break;
    case OP_HALT: return r[2] ^ r[1];
    default: return 0;
    }
  }
}

static uint64_t run_interp(int quick) {
  const int32_t n = quick ? 100000 : 6000000;
  const insn_t prog[] = {
    {OP_LI, 0, 0, 0, n},    /* 0: r0 = iterations */
    {OP_LI, 1, 0, 0, 1},    /* 1: r1 = x */
    {OP_LI, 2, 0, 0, 0},    /* 2: r2 = acc */
    {OP_CALL, 1, 0, 0, 0},  /* 3: x = f[r0 & 3](x, r0) */
    {OP_ADD, 2, 2, 1, 0},   /* 4: acc += x */
    {OP_SHRI, 3, 1, 0, 7},  /* 5 */
    {OP_XOR, 2, 2, 3, 0},   /* 6: acc ^= x >> 7 */
    {OP_ANDI, 4, 1, 0, 1},  /* 7 */
    {OP_JNZ, 4, 0, 0, 10},  /* 8: data-dependent branch */
    {OP_MUL, 2, 2, 1, 0},   /* 9: acc *= x when x is even */
    {OP_ADDI, 0, 0, 0, -1}, /* 10 */
    {OP_JNZ, 0, 0, 0, 3},   /* 11 */
    {OP_HALT, 0, 0, 0, 0},  /* 12 */
  };
  uint64_t regs[8] = {0};
  return vm_run(prog, regs);
}

/* ------------------------------------------------------------------------------- */
/* Integer: a tokenizer state machine over generated text                           */
/* ------------------------------------------------------------------------------- */

#define TEXT_MAX (1u << 19)
static uint8_t g_text[TEXT_MAX];
static uint8_t g_class[256];

enum { CL_SPACE, CL_ALPHA, CL_DIGIT, CL_PUNCT, CL_QUOTE };

static void setup_text(int quick) {
  (void)quick;
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz_ABCDEFGHIJ0123456789     \n\t+-*/=;(){}\"";
  uint64_t s = 7;
  for (uint32_t i = 0; i < TEXT_MAX; ++i) {
    g_text[i] = (uint8_t)alphabet[splitmix(&s) % (sizeof(alphabet) - 1)];
  }
  for (int c = 0; c < 256; ++c) {
    g_class[c] = CL_PUNCT;
  }
  g_class[' '] = g_class['\n'] = g_class['\t'] = CL_SPACE;
  for (int c = 'a'; c <= 'z'; ++c) {
    g_class[c] = CL_ALPHA;
  }
  for (int c = 'A'; c <= 'Z'; ++c) {
    g_class[c] = CL_ALPHA;
  }
  g_class['_'] = CL_ALPHA;
  for (int c = '0'; c <= '9'; ++c) {
    g_class[c] = CL_DIGIT;
  }
  g_class['"'] = CL_QUOTE;
}

static uint64_t run_dfa(int quick) {
  const int passes = quick ? 1 : 40;
  uint64_t idents = 0, numbers = 0, puncts = 0, strings = 0, h = 0;
  for (int p = 0; p < passes; ++p) {
    enum { S_START, S_IDENT, S_NUMBER, S_STRING } state = S_START;
    uint64_t len = 0;
    for (uint32_t i = 0; i < TEXT_MAX; ++i) {
      const uint8_t cl = g_class[g_text[i]];
      switch (state) {
      case S_START:
start:
        switch (cl) {
        case CL_ALPHA:
          state = S_IDENT;
          len = 1;
          break;
        case CL_DIGIT:
          state = S_NUMBER;
          len = 1;
          break;
        case CL_QUOTE:
          state = S_STRING;
          len = 0;
          break;
        case CL_PUNCT:
          ++puncts;
          h = mix(h, g_text[i]);
          break;
        default: break;
        }
        break;
      case S_IDENT:
        if (cl == CL_ALPHA || cl == CL_DIGIT) {
          ++len;
        } else {
          ++idents;
          h = mix(h, len);
          state = S_START;
          goto start;
        }
        break;
      case S_NUMBER:
        if (cl == CL_DIGIT) {
          ++len;
        } else if (cl == CL_ALPHA) {
          state = S_IDENT; /* 12abc is an identifier here */
          ++len;
        } else {
          ++numbers;
          h = mix(h, len << 8);
          state = S_START;
          goto start;
        }
        break;
      case S_STRING:
        if (cl == CL_QUOTE) {
          ++strings;
          h = mix(h, len << 16);
          state = S_START;
        } else {
          ++len;
        }
        break;
      }
    }
  }
  return mix(mix(mix(mix(h, idents), numbers), puncts), strings);
}

/* ------------------------------------------------------------------------------- */
/* Memory: rep movs / rep stos, a vectorisable copy loop, pointer chasing           */
/* ------------------------------------------------------------------------------- */

#define COPY_BYTES (1u << 20)
static uint8_t g_src[COPY_BYTES + 128] __attribute__((aligned(64)));
static uint8_t g_dst[COPY_BYTES + 128] __attribute__((aligned(64)));

/* On x86-64 these are the string instructions the translator has to handle. arm64 has
 * none, so it gets the obvious loop (which the compiler turns into its best copy). */
static void copy_bytes(void* d, const void* s, size_t n) {
#if defined(__x86_64__)
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
  uint8_t* dd = d;
  const uint8_t* ss = s;
  for (size_t i = 0; i < n; ++i) {
    dd[i] = ss[i];
  }
#endif
}

static void copy_qwords(void* d, const void* s, size_t n) {
#if defined(__x86_64__)
  __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
  uint64_t* dd = d;
  const uint64_t* ss = s;
  for (size_t i = 0; i < n; ++i) {
    dd[i] = ss[i];
  }
#endif
}

static void fill_bytes(void* d, uint8_t v, size_t n) {
#if defined(__x86_64__)
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
#else
  uint8_t* dd = d;
  for (size_t i = 0; i < n; ++i) {
    dd[i] = v;
  }
#endif
}

static void setup_copy(int quick) {
  (void)quick;
  uint64_t s = 99;
  for (uint32_t i = 0; i < COPY_BYTES + 128; ++i) {
    g_src[i] = (uint8_t)splitmix(&s);
  }
}

static uint64_t sample_dst(uint64_t h) {
  for (uint32_t i = 0; i < COPY_BYTES; i += 4093) {
    h = mix(h, g_dst[i]);
  }
  return h;
}

static uint64_t run_rep_movs(int quick) {
  const int iters = quick ? 4 : 200;
  uint64_t h = 0;
  for (int k = 0; k < iters; ++k) {
    fill_bytes(g_dst, (uint8_t)k, COPY_BYTES);
    h = mix(h, g_dst[(uint32_t)k * 977u % COPY_BYTES]);
    /* Misaligned byte copy, then an aligned quadword copy over part of it. */
    copy_bytes(g_dst + (k * 7 & 63), g_src + (k & 63), COPY_BYTES - 64);
    h = sample_dst(h);
    copy_qwords(g_dst, g_src + 64, COPY_BYTES / 16);
    h = sample_dst(h);
  }
  return h;
}

static uint64_t run_copy_loop(int quick) {
  const int iters = quick ? 4 : 600;
  uint64_t h = 0;
  uint64_t* d = (uint64_t*)g_dst;
  const uint64_t* s = (const uint64_t*)g_src;
  for (int k = 0; k < iters; ++k) {
    const uint64_t key = 0x0101010101010101ull * (uint64_t)k;
    for (uint32_t i = 0; i < COPY_BYTES / 8; ++i) {
      d[i] = s[i] ^ key;
    }
    h = sample_dst(h);
  }
  return h;
}

typedef struct node {
  struct node* next;
  uint64_t value;
} node_t;

#define NODES_MAX (1u << 19) /* 8 MiB */
static node_t g_nodes[NODES_MAX];

static void setup_chase(int quick) {
  (void)quick;
  /* Sattolo's algorithm: a random permutation that is one single cycle. */
  static uint32_t perm[NODES_MAX];
  uint64_t s = 5;
  for (uint32_t i = 0; i < NODES_MAX; ++i) {
    perm[i] = i;
  }
  for (uint32_t i = NODES_MAX - 1; i > 0; --i) {
    uint32_t j = (uint32_t)(splitmix(&s) % i);
    uint32_t t = perm[i];
    perm[i] = perm[j];
    perm[j] = t;
  }
  for (uint32_t i = 0; i < NODES_MAX; ++i) {
    g_nodes[i].next = &g_nodes[perm[i]];
    g_nodes[i].value = splitmix(&s);
  }
}

static uint64_t run_ptr_chase(int quick) {
  const uint32_t steps = quick ? 100000 : 4000000;
  const node_t* n = &g_nodes[0];
  uint64_t sum = 0;
  for (uint32_t i = 0; i < steps; ++i) {
    sum += n->value ^ i;
    n = n->next;
  }
  return mix(sum, (uint64_t)(n - g_nodes));
}

/* ------------------------------------------------------------------------------- */
/* Floating point: scalar and vector matrix multiply, n-body, FFT                   */
/* ------------------------------------------------------------------------------- */

#define MAT_N 128
static double g_ma[MAT_N][MAT_N], g_mb[MAT_N][MAT_N], g_mc[MAT_N][MAT_N];

static void setup_matrix(int quick) {
  (void)quick;
  uint64_t s = 11;
  for (int i = 0; i < MAT_N; ++i) {
    for (int j = 0; j < MAT_N; ++j) {
      g_ma[i][j] = (double)(int64_t)(splitmix(&s) % 2001 - 1000) / 256.0;
      g_mb[i][j] = (double)(int64_t)(splitmix(&s) % 2001 - 1000) / 1000.0;
    }
  }
}

static uint64_t run_matmul_scalar(int quick) {
  const int rounds = quick ? 1 : 24;
  uint64_t h = 0;
  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < MAT_N; ++i) {
      for (int j = 0; j < MAT_N; ++j) {
        double acc = (double)r;
#pragma clang loop vectorize(disable) interleave(disable)
        for (int k = 0; k < MAT_N; ++k) {
          acc += g_ma[i][k] * g_mb[k][j];
        }
        g_mc[i][j] = acc;
      }
    }
    for (int i = 0; i < MAT_N; ++i) {
      for (int j = 0; j < MAT_N; ++j) {
        h = mix(h, bits_of_double(g_mc[i][j]));
      }
    }
  }
  return h;
}

/* Four floats per vector: SSE2 on x86-64, NEON on arm64 (clang's vector extension). */
typedef float f32x4 __attribute__((vector_size(16)));
#define VMAT_N 192
static f32x4 g_va[VMAT_N][VMAT_N / 4], g_vb[VMAT_N][VMAT_N / 4], g_vc[VMAT_N][VMAT_N / 4];

static void setup_vmatrix(int quick) {
  (void)quick;
  uint64_t s = 13;
  for (int i = 0; i < VMAT_N; ++i) {
    for (int j = 0; j < VMAT_N / 4; ++j) {
      for (int l = 0; l < 4; ++l) {
        g_va[i][j][l] = (float)(int)(splitmix(&s) % 201 - 100) / 64.0f;
        g_vb[i][j][l] = (float)(int)(splitmix(&s) % 201 - 100) / 100.0f;
      }
    }
  }
}

static uint64_t run_matmul_vec4(int quick) {
  const int rounds = quick ? 1 : 20;
  uint64_t h = 0;
  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < VMAT_N; ++i) {
      for (int j = 0; j < VMAT_N / 4; ++j) {
        g_vc[i][j] = (f32x4) {(float)r, (float)r, (float)r, (float)r};
      }
      for (int k = 0; k < VMAT_N; ++k) {
        const float a = ((const float*)g_va[i])[k];
        const f32x4 av = {a, a, a, a};
        for (int j = 0; j < VMAT_N / 4; ++j) {
          g_vc[i][j] = g_vc[i][j] + av * g_vb[k][j];
        }
      }
    }
    for (int i = 0; i < VMAT_N; ++i) {
      for (int j = 0; j < VMAT_N / 4; ++j) {
        for (int l = 0; l < 4; ++l) {
          h = mix(h, bits_of_float(g_vc[i][j][l]));
        }
      }
    }
  }
  return h;
}

#define BODIES 48
static uint64_t run_nbody(int quick) {
  const int steps = quick ? 20 : 3000;
  double px[BODIES], py[BODIES], pz[BODIES], vx[BODIES], vy[BODIES], vz[BODIES], m[BODIES];
  uint64_t s = 17;
  for (int i = 0; i < BODIES; ++i) {
    px[i] = (double)(int64_t)(splitmix(&s) % 2001 - 1000) / 100.0;
    py[i] = (double)(int64_t)(splitmix(&s) % 2001 - 1000) / 100.0;
    pz[i] = (double)(int64_t)(splitmix(&s) % 2001 - 1000) / 100.0;
    vx[i] = vy[i] = vz[i] = 0.0;
    m[i] = 1.0 + (double)(splitmix(&s) % 100) / 10.0;
  }
  const double dt = 0.001, eps = 0.01;
  for (int t = 0; t < steps; ++t) {
    for (int i = 0; i < BODIES; ++i) {
      for (int j = i + 1; j < BODIES; ++j) {
        const double dx = px[j] - px[i], dy = py[j] - py[i], dz = pz[j] - pz[i];
        const double r2 = dx * dx + dy * dy + dz * dz + eps;
        const double inv = dt / (r2 * __builtin_sqrt(r2));
        vx[i] += dx * m[j] * inv;
        vy[i] += dy * m[j] * inv;
        vz[i] += dz * m[j] * inv;
        vx[j] -= dx * m[i] * inv;
        vy[j] -= dy * m[i] * inv;
        vz[j] -= dz * m[i] * inv;
      }
    }
    for (int i = 0; i < BODIES; ++i) {
      px[i] += dt * vx[i];
      py[i] += dt * vy[i];
      pz[i] += dt * vz[i];
    }
  }
  uint64_t h = 0;
  for (int i = 0; i < BODIES; ++i) {
    h = mix(mix(mix(h, bits_of_double(px[i])), bits_of_double(py[i])), bits_of_double(vz[i]));
  }
  return h;
}

/* sin and cos from their Taylor series (x in [0, pi]); only + * / so every build gets
 * the same bits. Accurate enough for twiddle factors, which is all they are used for. */
static void sin_cos(double x, double* sn, double* cs) {
  double term_s = x, term_c = 1.0, sum_s = x, sum_c = 1.0;
  const double x2 = x * x;
  for (int n = 1; n < 30; ++n) {
    term_s = -term_s * x2 / (double)((2 * n) * (2 * n + 1));
    term_c = -term_c * x2 / (double)((2 * n - 1) * (2 * n));
    sum_s += term_s;
    sum_c += term_c;
  }
  *sn = sum_s;
  *cs = sum_c;
}

#define FFT_N 4096
#define FFT_LOG 12
static double g_fre[FFT_N], g_fim[FFT_N], g_wre[FFT_N / 2], g_wim[FFT_N / 2];

static void setup_fft(int quick) {
  (void)quick;
  const double pi = 3.14159265358979323846;
  for (int k = 0; k < FFT_N / 2; ++k) {
    double sn, cs;
    sin_cos(2.0 * pi * (double)k / (double)FFT_N, &sn, &cs);
    g_wre[k] = cs;
    g_wim[k] = -sn;
  }
}

static void fft(double* re, double* im, int inverse) {
  for (uint32_t i = 0, j = 0; i < FFT_N; ++i) {
    if (i < j) {
      double t = re[i];
      re[i] = re[j];
      re[j] = t;
      t = im[i];
      im[i] = im[j];
      im[j] = t;
    }
    uint32_t bit = FFT_N >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j |= bit;
  }
  for (int len = 2; len <= FFT_N; len <<= 1) {
    const int half = len >> 1, step = FFT_N / len;
    for (int i = 0; i < FFT_N; i += len) {
      for (int k = 0; k < half; ++k) {
        const double wr = g_wre[k * step], wi = inverse ? -g_wim[k * step] : g_wim[k * step];
        const double xr = re[i + k + half], xi = im[i + k + half];
        const double tr = xr * wr - xi * wi, ti = xr * wi + xi * wr;
        re[i + k + half] = re[i + k] - tr;
        im[i + k + half] = im[i + k] - ti;
        re[i + k] += tr;
        im[i + k] += ti;
      }
    }
  }
  if (inverse) {
    for (int i = 0; i < FFT_N; ++i) {
      re[i] *= 1.0 / FFT_N;
      im[i] *= 1.0 / FFT_N;
    }
  }
}

static uint64_t run_fft(int quick) {
  const int rounds = quick ? 2 : 240;
  uint64_t s = 23;
  for (int i = 0; i < FFT_N; ++i) {
    g_fre[i] = (double)(int64_t)(splitmix(&s) % 2001 - 1000) / 1000.0;
    g_fim[i] = 0.0;
  }
  uint64_t h = 0;
  for (int r = 0; r < rounds; ++r) {
    fft(g_fre, g_fim, 0);
    h = mix(h, bits_of_double(g_fre[r]) ^ bits_of_double(g_fim[FFT_N - 1 - r]));
    fft(g_fre, g_fim, 1);
  }
  for (int i = 0; i < FFT_N; i += 7) {
    h = mix(h, bits_of_double(g_fre[i]));
  }
  return h;
}

/* ------------------------------------------------------------------------------- */
/* x87: long double (80-bit on x86-64; the same as double on arm64)                 */
/* ------------------------------------------------------------------------------- */

static uint64_t bits_of_long_double(long double v) {
  uint64_t lo = 0, hi = 0;
  __builtin_memcpy(&lo, &v, 8);
  if (sizeof(long double) > 8) {
    uint16_t top;
    __builtin_memcpy(&top, (const char*)&v + 8, 2); /* sign and exponent */
    hi = top;
  }
  return mix(lo, hi);
}

static uint64_t run_x87(int quick) {
  const int n = quick ? 20000 : 750000;
  long double zeta = 0.0L, prod = 1.0L, root = 2.0L;
  int64_t ints = 0;
  for (int i = 1; i <= n; ++i) {
    const long double li = (long double)i;
    zeta += 1.0L / (li * li);
    prod *= 1.0L + 1.0L / (li + 3.0L);
    if (prod > 1e6L) {
      prod = __builtin_sqrtl(prod);
    }
    root = 0.5L * (root + 2.0L / root); /* Newton for sqrt(2): stays put once converged */
    ints += (int64_t)(zeta * 1e15L) - (int64_t)(prod * 1e3L);
  }
  return mix(mix(mix(bits_of_long_double(zeta), bits_of_long_double(prod)), bits_of_long_double(root)), (uint64_t)ints);
}

/* ------------------------------------------------------------------------------- */
/* 64-bit multiply and divide: Miller-Rabin with Montgomery arithmetic, and division */
/* ------------------------------------------------------------------------------- */

typedef struct {
  uint64_t n, ninv, r1, r2; /* modulus, -n^-1 mod 2^64, 2^64 mod n, 2^128 mod n */
} mont_t;

static uint64_t mont_mul(const mont_t* m, uint64_t a, uint64_t b) {
  const unsigned __int128 t = (unsigned __int128)a * b;
  const uint64_t q = (uint64_t)t * m->ninv;
  const unsigned __int128 u = (t + (unsigned __int128)q * m->n) >> 64;
  uint64_t r = (uint64_t)u;
  return r >= m->n ? r - m->n : r;
}

static void mont_init(mont_t* m, uint64_t n) { /* n odd, n < 2^63 */
  uint64_t inv = n;                            /* Newton: inverse of n modulo 2^64 */
  for (int i = 0; i < 6; ++i) {
    inv *= 2 - n * inv;
  }
  m->n = n;
  m->ninv = (uint64_t)0 - inv;
  m->r1 = ((uint64_t)0 - n) % n;
  uint64_t r = m->r1;
  for (int i = 0; i < 64; ++i) {
    r = r >= n - r ? r - (n - r) : r + r;
  }
  m->r2 = r;
}

static int is_prime(uint64_t n) {
  static const uint64_t bases[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
  if (n < 2) {
    return 0;
  }
  for (int i = 0; i < 12; ++i) {
    if (n % bases[i] == 0) {
      return n == bases[i];
    }
  }
  mont_t m;
  mont_init(&m, n);
  uint64_t d = n - 1;
  int s = 0;
  while ((d & 1) == 0) {
    d >>= 1;
    ++s;
  }
  const uint64_t one = m.r1, minus_one = n - m.r1;
  for (int i = 0; i < 12; ++i) {
    uint64_t x = one, b = mont_mul(&m, bases[i], m.r2);
    for (uint64_t e = d; e; e >>= 1) {
      if (e & 1) {
        x = mont_mul(&m, x, b);
      }
      b = mont_mul(&m, b, b);
    }
    if (x == one || x == minus_one) {
      continue;
    }
    int composite = 1;
    for (int r = 1; r < s; ++r) {
      x = mont_mul(&m, x, x);
      if (x == minus_one) {
        composite = 0;
        break;
      }
    }
    if (composite) {
      return 0;
    }
  }
  return 1;
}

static uint64_t run_muldiv(int quick) {
  const uint64_t count = quick ? 2000 : 60000;
  uint64_t primes = 0, h = 0;
  const uint64_t start = (1ull << 62) + 1;
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t n = start + 2 * i;
    if (is_prime(n)) {
      ++primes;
      h = mix(h, n);
    }
  }
  /* Plain 64-bit division, signed and unsigned. */
  uint64_t s = 3, acc = 0;
  for (uint64_t i = 0; i < count * 20; ++i) {
    const uint64_t a = splitmix(&s), b = (splitmix(&s) >> (i & 31)) | 1;
    acc += a / b + a % b;
    acc ^= (uint64_t)((int64_t)a / (int64_t)(b | 0x100));
  }
  return mix(mix(h, primes), acc);
}

/* ------------------------------------------------------------------------------- */
/* Calls and returns: recursive Fibonacci, deep recursion                           */
/* ------------------------------------------------------------------------------- */

static NOINLINE uint64_t fib(uint64_t n) {
  return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static uint64_t run_calls(int quick) {
  return fib(quick ? 22 : 34);
}

static NOINLINE uint64_t descend(uint64_t depth, uint64_t acc) {
  if (depth == 0) {
    return acc;
  }
  const uint64_t r = descend(depth - 1, acc * 31 + depth);
  return (r >> 1) + depth; /* not a tail call, and no accumulator LLVM can rewrite */
}

static uint64_t run_deep_recursion(int quick) {
  const int rounds = quick ? 2 : 100;
  uint64_t h = 0;
  for (int r = 0; r < rounds; ++r) {
    h = mix(h, descend(50000, (uint64_t)r));
  }
  return h;
}

/* ------------------------------------------------------------------------------- */
/* Threads: a spin lock and atomics; message passing through rings                  */
/* ------------------------------------------------------------------------------- */

#define WORKERS 4

static struct {
  uint32_t lock;
  uint64_t counter;
  uint64_t sum;
  uint64_t atomic_sum;
  char pad[64];
} g_shared __attribute__((aligned(64)));

typedef struct {
  uint64_t id, iters;
} lock_arg_t;

static void spin_lock(uint32_t* l) {
  for (;;) {
    if (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE) == 0) {
      return;
    }
    while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
      cpu_relax();
    }
  }
}

static void spin_unlock(uint32_t* l) {
  __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static void lock_worker(void* p) {
  const lock_arg_t* a = p;
  uint64_t s = a->id * 1000003;
  for (uint64_t i = 0; i < a->iters; ++i) {
    const uint64_t v = splitmix(&s);
    spin_lock(&g_shared.lock);
    g_shared.counter += 1;
    g_shared.sum += v; /* sums commute, so the result does not depend on the order */
    spin_unlock(&g_shared.lock);
    __atomic_fetch_add(&g_shared.atomic_sum, v >> 3, __ATOMIC_SEQ_CST);
  }
}

static uint64_t run_threads_lock(int quick) {
  lock_arg_t args[WORKERS];
  int handles[WORKERS];
  g_shared.lock = 0;
  g_shared.counter = g_shared.sum = g_shared.atomic_sum = 0;
  for (int i = 0; i < WORKERS; ++i) {
    args[i].id = (uint64_t)i + 1;
    args[i].iters = quick ? 20000 : 400000;
    handles[i] = bench_thread_start(lock_worker, &args[i]);
  }
  for (int i = 0; i < WORKERS; ++i) {
    bench_thread_join(handles[i]);
  }
  return mix(mix(g_shared.counter, g_shared.sum), g_shared.atomic_sum);
}

/* A single-producer single-consumer ring. The data slots are written with plain stores
 * and published by a release store of `head`; the consumer reads them after an acquire
 * load. On x86-64 both are ordinary moves: the program relies on x86's store ordering
 * (TSO), which the translator must emulate. With the emulation off the consumer can see
 * a slot before its data, which shows as a wrong checksum. */
#define RING 256
typedef struct {
  uint64_t head __attribute__((aligned(64)));
  uint64_t tail __attribute__((aligned(64)));
  uint64_t data[RING] __attribute__((aligned(64)));
  uint64_t count, seed, result;
} ring_t;
static ring_t g_rings[WORKERS / 2];

static void producer(void* p) {
  ring_t* r = p;
  uint64_t s = r->seed;
  for (uint64_t i = 0; i < r->count; ++i) {
    const uint64_t h = r->head;
    while (h - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) == RING) {
      cpu_relax();
    }
    r->data[h % RING] = splitmix(&s);
    __atomic_store_n(&r->head, h + 1, __ATOMIC_RELEASE);
  }
}

static void consumer(void* p) {
  ring_t* r = p;
  uint64_t h = 0;
  for (uint64_t i = 0; i < r->count; ++i) {
    const uint64_t t = r->tail;
    while (__atomic_load_n(&r->head, __ATOMIC_ACQUIRE) == t) {
      cpu_relax();
    }
    h = mix(h, r->data[t % RING]);
    __atomic_store_n(&r->tail, t + 1, __ATOMIC_RELEASE);
  }
  r->result = h;
}

static uint64_t run_threads_mp(int quick) {
  int handles[WORKERS];
  for (int i = 0; i < WORKERS / 2; ++i) {
    g_rings[i].head = g_rings[i].tail = 0;
    g_rings[i].count = quick ? 20000 : 1000000;
    g_rings[i].seed = 77 + (uint64_t)i;
    g_rings[i].result = 0;
    handles[2 * i] = bench_thread_start(consumer, &g_rings[i]);
    handles[2 * i + 1] = bench_thread_start(producer, &g_rings[i]);
  }
  for (int i = 0; i < WORKERS; ++i) {
    bench_thread_join(handles[i]);
  }
  uint64_t h = 0;
  for (int i = 0; i < WORKERS / 2; ++i) {
    h = mix(h, g_rings[i].result);
  }
  return h;
}

/* ------------------------------------------------------------------------------- */
/* Driver                                                                           */
/* ------------------------------------------------------------------------------- */

typedef struct {
  const char* name;
  void (*setup)(int quick);
  uint64_t (*run)(int quick);
  uint64_t expect_quick, expect_full; /* 0: not recorded */
} kernel_t;

/* Expected checksums, recorded from all three builds agreeing (see
 * docs/macOS-benchmarks.md). The x87 kernel has one pair for x86-64 (80-bit) and one
 * for arm64 (64-bit long double). */
#if defined(__aarch64__)
#define X87_QUICK 0x737e190ec396e767ull
#define X87_FULL 0x3e644253ca43e7cfull
#else
#define X87_QUICK 0x4375b1b2785fabf8ull
#define X87_FULL 0xd789dcb0d21c1a76ull
#endif

static const kernel_t g_kernels[] = {
  {"crc32", setup_bytes, run_crc32, 0x6d3868fcb274c7b6ull, 0x4b7aaa9108180289ull},
  {"hash64", setup_bytes, run_hash64, 0xa6de4447ab09d03bull, 0x649a98274c2013fcull},
  {"sort", 0, run_sort, 0xd87162ef2264ede8ull, 0x97821945178672ebull},
  {"interp", 0, run_interp, 0xf10c7fe37629dfccull, 0x547d7b1b0381003full},
  {"dfa", setup_text, run_dfa, 0xa42a6c91a4f0df58ull, 0x4d918429caa23301ull},
  {"rep_movs", setup_copy, run_rep_movs, 0xc2795233e945d851ull, 0xbf7a41d6d2155edeull},
  {"copy_loop", setup_copy, run_copy_loop, 0xf34882810293e1c7ull, 0x9f4154e98d608f38ull},
  {"ptr_chase", setup_chase, run_ptr_chase, 0x1a681542e0b02100ull, 0x466db1ae182be898ull},
  {"matmul_f64", setup_matrix, run_matmul_scalar, 0x717a6b1320ae3985ull, 0xf3d00de256019b4aull},
  {"matmul_f32x4", setup_vmatrix, run_matmul_vec4, 0x0e5cf52fdf6fa1d4ull, 0xdab31eba4e7977b5ull},
  {"nbody", 0, run_nbody, 0x33f8fb908f111258ull, 0xa83c67bfb74069cbull},
  {"fft", setup_fft, run_fft, 0x43048cbd3ea38f70ull, 0x3821471867c986e1ull},
  {"x87", 0, run_x87, X87_QUICK, X87_FULL},
  {"muldiv", 0, run_muldiv, 0xda5adc5366947acdull, 0x12bad6a609cf11c6ull},
  {"calls", 0, run_calls, 0x000000000000452full, 0x00000000005704e7ull},
  {"deep_recursion", 0, run_deep_recursion, 0xe5e447e57197395aull, 0xd73d4842a01e7590ull},
  {"threads_lock", 0, run_threads_lock, 0xd33b3540666222e7ull, 0xff003f56a74a02ebull},
  {"threads_mp", 0, run_threads_mp, 0xa0cb8f09e9c3d925ull, 0x6a379e1e56d34819ull},
};

static int selected(const char* only, const char* name) {
  if (!only || !*only) {
    return 1;
  }
  const char* p = only;
  while (*p) {
    const char* e = p;
    while (*e && *e != ',') {
      ++e;
    }
    if (str_eq_n(p, name, (size_t)(e - p))) {
      return 1;
    }
    p = *e ? e + 1 : e;
  }
  return 0;
}

int bench_main(int quick, int reps, const char* only) {
  line_t l = {{0}, 0};
  int failures = 0, ran = 0;
  if (reps < 0) {
    reps = 0;
  }
  put_str(&l, "START ");
  put_str(&l, bench_platform());
  put_str(&l, quick ? " quick reps " : " full reps ");
  put_u64(&l, (uint64_t)reps);
  put_end(&l);

  for (size_t k = 0; k < sizeof(g_kernels) / sizeof(g_kernels[0]); ++k) {
    const kernel_t* kn = &g_kernels[k];
    if (!selected(only, kn->name)) {
      continue;
    }
    ++ran;
    if (kn->setup) {
      kn->setup(quick);
    }
    uint64_t first = 0;
    int varies = 0;
    /* Repetition 0 is the warm-up: it includes translating the kernel's code. */
    for (int r = 0; r <= reps; ++r) {
      const uint64_t t0 = bench_time_ns();
      const uint64_t sum = kn->run(quick);
      const uint64_t t1 = bench_time_ns();
      if (r == 0) {
        first = sum;
      } else if (sum != first) {
        varies = 1;
      }
      put_str(&l, "K ");
      put_str(&l, kn->name);
      put_str(&l, " ");
      put_u64(&l, (uint64_t)r);
      put_str(&l, " ");
      put_u64(&l, t1 - t0);
      put_str(&l, " ");
      put_hex(&l, sum);
      put_end(&l);
    }
    const uint64_t expected = quick ? kn->expect_quick : kn->expect_full;
    const char* verdict = varies ? "VARIES" : expected == 0 ? "UNRECORDED" : expected == first ? "ok" : "MISMATCH";
    if (varies || (expected != 0 && expected != first)) {
      ++failures;
    }
    put_str(&l, "C ");
    put_str(&l, kn->name);
    put_str(&l, " ");
    put_str(&l, verdict);
    put_str(&l, " ");
    put_hex(&l, first);
    put_str(&l, " expected ");
    put_hex(&l, expected);
    put_end(&l);
  }
  put_str(&l, "END kernels ");
  put_u64(&l, (uint64_t)ran);
  put_str(&l, " failures ");
  put_u64(&l, (uint64_t)failures);
  put_end(&l);
  return failures;
}
