/* SPDX-License-Identifier: MIT */
/*
 * Vector memory-ordering emulation forms (VECTORTSOSPLIT, TSOBARRIERMERGE).
 *
 * With VECTORTSOSPLIT the translator emits a vector access under ordering emulation as
 * acquire loads or release stores of at most 8 bytes through general registers instead
 * of a plain access and a barrier (FEXCore JIT/MemoryOps.cpp, UseVectorTSOSplit);
 * TSOBARRIERMERGE merges a vector load's barrier with the next vector store's. These
 * subtests check that what the guest computes and what a fault leaves behind does not
 * depend on either option: every access form the split path emits (1/2 are not reached
 * by these instructions; 4, 8, 16 and 32 bytes, as two 16-byte halves), base plus
 * offsets at both edges of the unscaled range and beyond it, scaled and unscaled index
 * registers, every misalignment from 0 to 15 (so that each 8-byte half in turn crosses
 * 16 bytes and is rewritten by the misaligned-access handler, after which later runs
 * use the rewritten code), stores across a 4K boundary, loads across a 16K page, and
 * stores and loads into a page that faults.
 *
 * Whether the options change the code at all is not seen here (the results must be the
 * same); FEXCoreOrderingTest and the benchmark show that.
 */
#include "TestSupport.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* clang-format off */
/* Assembled from intel syntax with clang -target x86_64-apple-macos11. In: rdi (input,
 * misaligned by the run), rsi (output, misaligned), r8 = rsi + 0x800, r9 = rsi + 0xA00,
 * r10 = rsi + 0xC00, r11 = rsi + 0xE00, rcx = 3. The last accesses use rdi and rsi rounded
 * down to 16 bytes, for the forms that require alignment (movdqa, movaps, movapd, addps). */
static const uint8_t FormsProgram[] = {
  0xF3, 0x0F, 0x6F, 0x07,                                      /*   0: movdqu	(%rdi), %xmm0 */
  0xF3, 0x0F, 0x6F, 0x4F, 0x08,                                /*   4: movdqu	0x8(%rdi), %xmm1 */
  0xF3, 0x0F, 0x6F, 0x97, 0xF7, 0x00, 0x00, 0x00,              /*   9: movdqu	0xf7(%rdi), %xmm2 */
  0xF3, 0x0F, 0x6F, 0x9F, 0xF8, 0x00, 0x00, 0x00,              /*  17: movdqu	0xf8(%rdi), %xmm3 */
  0xF3, 0x0F, 0x6F, 0xA7, 0x00, 0xFF, 0xFF, 0xFF,              /*  25: movdqu	-0x100(%rdi), %xmm4 */
  0xF3, 0x0F, 0x6F, 0xAF, 0xFF, 0xFE, 0xFF, 0xFF,              /*  33: movdqu	-0x101(%rdi), %xmm5 */
  0xF3, 0x0F, 0x6F, 0xB7, 0x34, 0x12, 0x00, 0x00,              /*  41: movdqu	0x1234(%rdi), %xmm6 */
  0xF3, 0x0F, 0x6F, 0x7C, 0xCF, 0x28,                          /*  49: movdqu	0x28(%rdi,%rcx,8), %xmm7 */
  0xF3, 0x44, 0x0F, 0x10, 0x87, 0xFF, 0x00, 0x00, 0x00,        /*  55: movss	0xff(%rdi), %xmm8 */
  0xF3, 0x44, 0x0F, 0x10, 0x8F, 0x00, 0x01, 0x00, 0x00,        /*  64: movss	0x100(%rdi), %xmm9 */
  0xF2, 0x44, 0x0F, 0x10, 0x97, 0xFF, 0xFE, 0xFF, 0xFF,        /*  73: movsd	-0x101(%rdi), %xmm10 */
  0xF2, 0x44, 0x0F, 0x10, 0x9F, 0xFF, 0x00, 0x00, 0x00,        /*  82: movsd	0xff(%rdi), %xmm11 */
  0x0F, 0x6F, 0x47, 0x64,                                      /*  91: movq	0x64(%rdi), %mm0 */
  0x66, 0x44, 0x0F, 0x6E, 0x67, 0x03,                          /*  95: movd	0x3(%rdi), %xmm12 */
  0xF3, 0x44, 0x0F, 0x7E, 0x6C, 0x0F, 0xF7,                    /* 101: movq	-0x9(%rdi,%rcx), %xmm13 */
  0xC5, 0x7E, 0x6F, 0xB7, 0xC8, 0x00, 0x00, 0x00,              /* 108: vmovdqu	0xc8(%rdi), %ymm14 */
  0xC5, 0x7E, 0x6F, 0xBF, 0xF0, 0x00, 0x00, 0x00,              /* 116: vmovdqu	0xf0(%rdi), %ymm15 */
  0xDD, 0x47, 0x10,                                            /* 124: fldl	0x10(%rdi) */
  0xF3, 0x0F, 0x7F, 0x06,                                      /* 127: movdqu	%xmm0, (%rsi) */
  0xF3, 0x0F, 0x7F, 0x4E, 0x18,                                /* 131: movdqu	%xmm1, 0x18(%rsi) */
  0xF3, 0x0F, 0x7F, 0x96, 0xF7, 0x00, 0x00, 0x00,              /* 136: movdqu	%xmm2, 0xf7(%rsi) */
  0xF3, 0x41, 0x0F, 0x7F, 0x98, 0xF8, 0x00, 0x00, 0x00,        /* 144: movdqu	%xmm3, 0xf8(%r8) */
  0xF3, 0x0F, 0x7F, 0xA6, 0x00, 0xFF, 0xFF, 0xFF,              /* 153: movdqu	%xmm4, -0x100(%rsi) */
  0xF3, 0x0F, 0x7F, 0xAE, 0xE8, 0xFE, 0xFF, 0xFF,              /* 161: movdqu	%xmm5, -0x118(%rsi) */
  0xF3, 0x0F, 0x7F, 0xB6, 0x34, 0x12, 0x00, 0x00,              /* 169: movdqu	%xmm6, 0x1234(%rsi) */
  0xF3, 0x0F, 0x7F, 0xBE, 0xF8, 0x0F, 0x00, 0x00,              /* 177: movdqu	%xmm7, 0xff8(%rsi) */
  0xF3, 0x0F, 0x7F, 0x54, 0xCE, 0x28,                          /* 185: movdqu	%xmm2, 0x28(%rsi,%rcx,8) */
  0xF3, 0x45, 0x0F, 0x11, 0x80, 0xFF, 0x00, 0x00, 0x00,        /* 191: movss	%xmm8, 0xff(%r8) */
  0xF3, 0x45, 0x0F, 0x11, 0x89, 0x00, 0x01, 0x00, 0x00,        /* 200: movss	%xmm9, 0x100(%r9) */
  0xF2, 0x45, 0x0F, 0x11, 0x91, 0xFF, 0xFE, 0xFF, 0xFF,        /* 209: movsd	%xmm10, -0x101(%r9) */
  0xF2, 0x45, 0x0F, 0x11, 0x9A, 0xFF, 0x00, 0x00, 0x00,        /* 218: movsd	%xmm11, 0xff(%r10) */
  0xF2, 0x45, 0x0F, 0x11, 0x92, 0x00, 0xFF, 0xFF, 0xFF,        /* 227: movsd	%xmm10, -0x100(%r10) */
  0x0F, 0x7F, 0x46, 0x64,                                      /* 236: movq	%mm0, 0x64(%rsi) */
  0x66, 0x45, 0x0F, 0x7E, 0x62, 0x03,                          /* 240: movd	%xmm12, 0x3(%r10) */
  0x66, 0x44, 0x0F, 0xD6, 0x6C, 0x0E, 0xEC,                    /* 246: movq	%xmm13, -0x14(%rsi,%rcx) */
  0xC5, 0x7E, 0x7F, 0xB6, 0x2C, 0x01, 0x00, 0x00,              /* 253: vmovdqu	%ymm14, 0x12c(%rsi) */
  0xC4, 0x41, 0x7E, 0x7F, 0xBB, 0xF0, 0x00, 0x00, 0x00,        /* 261: vmovdqu	%ymm15, 0xf0(%r11) */
  0xC4, 0x41, 0x7E, 0x7F, 0xB3, 0xB0, 0x00, 0x00, 0x00,        /* 270: vmovdqu	%ymm14, 0xb0(%r11) */
  0xDD, 0x9E, 0xF4, 0x01, 0x00, 0x00,                          /* 279: fstpl	0x1f4(%rsi) */
  0x0F, 0x10, 0x47, 0x21,                                      /* 285: movups	0x21(%rdi), %xmm0 */
  0x0F, 0x11, 0x86, 0xFC, 0x1F, 0x00, 0x00,                    /* 289: movups	%xmm0, 0x1ffc(%rsi) */
  0x66, 0x0F, 0x10, 0x8F, 0xFC, 0x3F, 0x00, 0x00,              /* 296: movupd	0x3ffc(%rdi), %xmm1 */
  0x66, 0x0F, 0x11, 0x8E, 0xF9, 0x2F, 0x00, 0x00,              /* 304: movupd	%xmm1, 0x2ff9(%rsi) */
  0x66, 0x0F, 0xC4, 0x57, 0x4D, 0x03,                          /* 312: pinsrw	$0x3, 0x4d(%rdi), %xmm2 */
  0x66, 0x0F, 0x3A, 0x15, 0x96, 0x08, 0x02, 0x00, 0x00, 0x03,  /* 318: pextrw	$0x3, %xmm2, 0x208(%rsi) */
  0x0F, 0x16, 0x5F, 0x51,                                      /* 328: movhps	0x51(%rdi), %xmm3 */
  0x0F, 0x17, 0x9E, 0x1C, 0x02, 0x00, 0x00,                    /* 332: movhps	%xmm3, 0x21c(%rsi) */
  0x0F, 0x13, 0x9E, 0x30, 0x02, 0x00, 0x00,                    /* 339: movlps	%xmm3, 0x230(%rsi) */
  0xF3, 0x0F, 0x6F, 0x67, 0x11,                                /* 346: movdqu	0x11(%rdi), %xmm4 */
  0xF3, 0x0F, 0x7F, 0xA6, 0x58, 0x02, 0x00, 0x00,              /* 351: movdqu	%xmm4, 0x258(%rsi) */
  0xF3, 0x0F, 0x6F, 0x6F, 0x21,                                /* 359: movdqu	0x21(%rdi), %xmm5 */
  0xF3, 0x0F, 0x6F, 0x77, 0x31,                                /* 364: movdqu	0x31(%rdi), %xmm6 */
  0xF3, 0x0F, 0x7F, 0xAE, 0x6C, 0x02, 0x00, 0x00,              /* 369: movdqu	%xmm5, 0x26c(%rsi) */
  0xF3, 0x0F, 0x7F, 0xB6, 0x7C, 0x02, 0x00, 0x00,              /* 377: movdqu	%xmm6, 0x27c(%rsi) */
  0x48, 0x89, 0xF8,                                            /* 385: movq	%rdi, %rax */
  0x48, 0x83, 0xE0, 0xF0,                                      /* 388: andq	$-0x10, %rax */
  0x48, 0x89, 0xF2,                                            /* 392: movq	%rsi, %rdx */
  0x48, 0x83, 0xE2, 0xF0,                                      /* 395: andq	$-0x10, %rdx */
  0x66, 0x0F, 0x6F, 0x48, 0x40,                                /* 399: movdqa	0x40(%rax), %xmm1 */
  0x0F, 0x29, 0x8A, 0x00, 0x07, 0x00, 0x00,                    /* 404: movaps	%xmm1, 0x700(%rdx) */
  0x66, 0x0F, 0x28, 0x90, 0x40, 0x12, 0x00, 0x00,              /* 411: movapd	0x1240(%rax), %xmm2 */
  0x66, 0x0F, 0x7F, 0x92, 0x20, 0x07, 0x00, 0x00,              /* 419: movdqa	%xmm2, 0x720(%rdx) */
  0x0F, 0x58, 0x58, 0x60,                                      /* 427: addps	0x60(%rax), %xmm3 */
  0x0F, 0x29, 0x9A, 0x40, 0x07, 0x00, 0x00,                    /* 431: movaps	%xmm3, 0x740(%rdx) */
  0x0F, 0x77,                                                  /* 438: emms */
  0xF4,                                                        /* 440: hlt */
};

/* From the independent review: 61 more instruction classes (element inserts and
 * extracts, scalar arithmetic and conversions with memory operands, VEX scalar moves,
 * broadcasts, widening loads, MMX, non-temporal stores, x87 m32/m64 integer and float
 * forms, rip-relative and index-register addressing), the same registers as above.
 * Source: round1-work/review-r2-vector/probe/forms2/f.s, assembled the same way. */
static const uint8_t FormsProgram2[] = {
  0x66, 0x0f, 0x3a, 0x20, 0x47, 0x03, 0x05, 0x66, 0x0f, 0x3a, 0x14, 0x46, 0x01, 0x05, 0x66, 0x0f,
  0x3a, 0x22, 0x4f, 0x13, 0x02, 0x66, 0x0f, 0x3a, 0x16, 0x4e, 0x11, 0x02, 0x66, 0x48, 0x0f, 0x3a,
  0x22, 0x57, 0x27, 0x01, 0x66, 0x48, 0x0f, 0x3a, 0x16, 0x56, 0x23, 0x01, 0x66, 0x0f, 0x3a, 0x17,
  0x56, 0x35, 0x03, 0x66, 0x0f, 0x3a, 0x21, 0x5f, 0x41, 0x30, 0x0f, 0x12, 0x67, 0x51, 0x66, 0x0f,
  0x12, 0x6f, 0x59, 0x66, 0x0f, 0x16, 0x6f, 0x61, 0x66, 0x0f, 0x17, 0x6e, 0x49, 0x66, 0x0f, 0x13,
  0x66, 0x55, 0xf2, 0x0f, 0x58, 0x77, 0x69, 0xf3, 0x0f, 0x59, 0x7f, 0x73, 0xf3, 0x44, 0x0f, 0x5a,
  0x47, 0x7b, 0xf2, 0x4c, 0x0f, 0x2a, 0x8f, 0x81, 0x00, 0x00, 0x00, 0xf3, 0x44, 0x0f, 0x2a, 0x97,
  0x8b, 0x00, 0x00, 0x00, 0x66, 0x44, 0x0f, 0x2f, 0x8f, 0x93, 0x00, 0x00, 0x00, 0xf2, 0x44, 0x0f,
  0x12, 0x9f, 0x9b, 0x00, 0x00, 0x00, 0xc5, 0x7a, 0x10, 0xa7, 0xa3, 0x00, 0x00, 0x00, 0xc5, 0x7b,
  0x10, 0xaf, 0xab, 0x00, 0x00, 0x00, 0xc5, 0x7a, 0x7e, 0xb7, 0xb3, 0x00, 0x00, 0x00, 0xc5, 0x79,
  0x6e, 0xbf, 0xbb, 0x00, 0x00, 0x00, 0xc5, 0x7a, 0x11, 0x66, 0x61, 0xc5, 0x7b, 0x11, 0x6e, 0x67,
  0xc5, 0x79, 0xd6, 0x76, 0x71, 0xc4, 0xe2, 0x79, 0x18, 0x87, 0xc3, 0x00, 0x00, 0x00, 0xc4, 0xe2,
  0x7d, 0x59, 0x8f, 0xcb, 0x00, 0x00, 0x00, 0x66, 0x0f, 0x38, 0x30, 0x97, 0xd3, 0x00, 0x00, 0x00,
  0x66, 0x0f, 0x38, 0x25, 0x9f, 0xdb, 0x00, 0x00, 0x00, 0x0f, 0x60, 0x87, 0xe3, 0x00, 0x00, 0x00,
  0x0f, 0xd4, 0x87, 0xeb, 0x00, 0x00, 0x00, 0x0f, 0xe7, 0x86, 0x81, 0x00, 0x00, 0x00, 0x48, 0x0f,
  0xc3, 0x86, 0x91, 0x00, 0x00, 0x00, 0x0f, 0x2b, 0x96, 0xa0, 0x00, 0x00, 0x00, 0x0f, 0x7f, 0x86,
  0x99, 0x01, 0x00, 0x00, 0x0f, 0x7e, 0x86, 0xa3, 0x01, 0x00, 0x00, 0x0f, 0x6e, 0x8f, 0xf3, 0x00,
  0x00, 0x00, 0x0f, 0x7e, 0x8e, 0xab, 0x01, 0x00, 0x00, 0xd9, 0x87, 0x01, 0x01, 0x00, 0x00, 0xdf,
  0xaf, 0x09, 0x01, 0x00, 0x00, 0xdb, 0x9e, 0xb3, 0x01, 0x00, 0x00, 0xd9, 0x9e, 0xbb, 0x01, 0x00,
  0x00, 0x66, 0x0f, 0xc4, 0xb7, 0x11, 0x01, 0x00, 0x00, 0x07, 0x66, 0x0f, 0x3a, 0x15, 0xb6, 0xc3,
  0x01, 0x00, 0x00, 0x07, 0xf3, 0x0f, 0x10, 0x7f, 0xf9, 0xf3, 0x0f, 0x11, 0xbe, 0xfb, 0xfe, 0xff,
  0xff, 0xf2, 0x44, 0x0f, 0x10, 0x87, 0xe0, 0x7f, 0x00, 0x00, 0xf2, 0x44, 0x0f, 0x11, 0x86, 0xff,
  0x07, 0x00, 0x00, 0xf2, 0x44, 0x0f, 0x10, 0x8c, 0xcf, 0x00, 0xfe, 0xff, 0xff, 0xf2, 0x44, 0x0f,
  0x11, 0x8c, 0x8e, 0x00, 0xfe, 0xff, 0xff, 0xf2, 0x44, 0x0f, 0x10, 0x15, 0xc0, 0xff, 0xff, 0xff,
  0xf2, 0x44, 0x0f, 0x5a, 0x9f, 0x23, 0x01, 0x00, 0x00, 0xf2, 0x44, 0x0f, 0x51, 0xa7, 0x2b, 0x01,
  0x00, 0x00, 0xf2, 0x44, 0x0f, 0x5f, 0xaf, 0x33, 0x01, 0x00, 0x00, 0xc5, 0x73, 0x58, 0xb7, 0x3b,
  0x01, 0x00, 0x00, 0x44, 0x0f, 0x2e, 0xb7, 0x43, 0x01, 0x00, 0x00, 0x0f, 0x92, 0xc0, 0x88, 0x86,
  0xd1, 0x01, 0x00, 0x00, 0xc5, 0xf8, 0x77, 0x0f, 0x77, 0xf4,
};

/* Single accesses at rdi, each followed by hlt, for the fault cases. */
enum { STORE16 = 0, STORE8 = 5, STORE32 = 10, LOAD16 = 15 };
static const uint8_t AccessProgram[] = {
  0xF3, 0x0F, 0x7F, 0x07,                                      /*   0: movdqu	%xmm0, (%rdi) */
  0xF4,                                                        /*   4: hlt */
  0xF2, 0x0F, 0x11, 0x07,                                      /*   5: movsd	%xmm0, (%rdi) */
  0xF4,                                                        /*   9: hlt */
  0xC5, 0xFE, 0x7F, 0x07,                                      /*  10: vmovdqu	%ymm0, (%rdi) */
  0xF4,                                                        /*  14: hlt */
  0xF3, 0x0F, 0x6F, 0x0F,                                      /*  15: movdqu	(%rdi), %xmm1 */
  0xF4,                                                        /*  19: hlt */
};
/* clang-format on */

/* The configurations compared: the first is the reference. (VECTORTSOSPLIT bits 4, 8 and
 * 16, 16-byte halves, exist only in the testing build; fexmac_hook_test covers them.) */
struct OrderingConfig {
  const char* Name;
  fexmac_option Options[5];
  size_t Count;
};
static const struct OrderingConfig Configs[] = {
  {"no ordering", {{"TSOENABLED", "0"}}, 1},
  {"barriers", {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}}, 2},
  {"8-byte and smaller loads and stores acquire/release (3)", {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"VECTORTSOSPLIT", "3"}}, 3},
  {"small loads only (1)", {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"VECTORTSOSPLIT", "1"}}, 3},
  {"small stores only (2)", {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"VECTORTSOSPLIT", "2"}}, 3},
  {"merged barriers", {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"TSOBARRIERMERGE", "1"}}, 3},
  {"small accesses split and merged barriers", {{"TSOENABLED", "1"}, {"VECTORTSOENABLED", "1"}, {"VECTORTSOSPLIT", "3"}, {"TSOBARRIERMERGE", "1"}}, 4},
};
enum { CONFIGS = sizeof(Configs) / sizeof(Configs[0]) };

enum { REGION = 0x10000, MIDDLE = 0x8000 };

static uint64_t Hash(const uint8_t* Data, size_t Size) {
  uint64_t H = 1469598103934665603ULL;
  for (size_t i = 0; i < Size; ++i) {
    H = (H ^ Data[i]) * 1099511628211ULL;
  }
  return H;
}

/* What a run leaves: the output region and every register the program writes. */
struct FormsResult {
  int Reason;
  uint64_t Rip;
  uint64_t OutputHash;
  uint64_t Xmm[16][2];
  uint64_t YmmHigh[16][2];
  uint64_t Mm0;
};

static void FillInput(uint8_t* In) {
  for (size_t i = 0; i < REGION; ++i) {
    In[i] = (uint8_t)(i * 37 + (i >> 8) * 11 + 1);
  }
}

static struct FormsResult RunForms(fexmac_thread* Thread, uint64_t CodeBase, uint8_t* In, uint8_t* Out, unsigned InMis, unsigned OutMis) {
  memset(Out, 0xEE, REGION);
  fexmac_x86_state State = GetState(Thread);
  State.rip = CodeBase;
  const uint64_t Rsi = (uint64_t)(uintptr_t)Out + MIDDLE + OutMis;
  State.gpr[RDI] = (uint64_t)(uintptr_t)In + MIDDLE + InMis;
  State.gpr[RSI] = Rsi;
  State.gpr[RCX] = 3;
  State.gpr[8] = Rsi + 0x800;
  State.gpr[9] = Rsi + 0xA00;
  State.gpr[10] = Rsi + 0xC00;
  State.gpr[11] = Rsi + 0xE00;
  memset(State.xmm, 0x5A, sizeof(State.xmm));
  memset(State.ymm_high, 0x6B, sizeof(State.ymm_high));
  SetState(Thread, &State);
  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state After = GetState(Thread);
  struct FormsResult R;
  memset(&R, 0, sizeof(R));
  R.Reason = Stop.reason;
  R.Rip = After.rip - CodeBase;
  R.OutputHash = Hash(Out, REGION);
  memcpy(R.Xmm, After.xmm, sizeof(R.Xmm));
  memcpy(R.YmmHigh, After.ymm_high, sizeof(R.YmmHigh));
  R.Mm0 = After.mm[0][0];
  return R;
}

static void RunFormsProgram(const char* What, const uint8_t* Program, size_t ProgramSize) {
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* In = MapGuestMemory(REGION);
  uint8_t* Out = MapGuestMemory(REGION);
  memcpy(Code, Program, ProgramSize);
  FillInput(In);
  const uint64_t CodeBase = (uint64_t)(uintptr_t)Code;
  /* Input and output misalignments: every value for each, in a fixed pairing, twice
   * (the second pass runs code the misaligned-access handler has rewritten). */
  enum { RUNS = 32 };
  static struct FormsResult Results[CONFIGS][RUNS];
  for (int C = 0; C < CONFIGS; ++C) {
    fexmac_context* Context = CreateContextWithOptions(Configs[C].Options, Configs[C].Count, NULL, NULL);
    fexmac_thread* Thread = CreateThreadAt(Context, CodeBase, 0);
    for (int RunIndex = 0; RunIndex < RUNS; ++RunIndex) {
      const unsigned InMis = (unsigned)RunIndex % 16;
      const unsigned OutMis = (unsigned)(RunIndex * 7 + RunIndex / 16) % 16;
      Results[C][RunIndex] = RunForms(Thread, CodeBase, In, Out, InMis, OutMis);
    }
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
  int Halted = 1;
  for (int RunIndex = 0; RunIndex < RUNS; ++RunIndex) {
    Halted &= Results[0][RunIndex].Reason == FEXMAC_STOP_HALT && Results[0][RunIndex].Rip == ProgramSize - 1;
  }
  Check(Halted, "%s: every reference run reaches the final hlt", What);
  for (int C = 1; C < CONFIGS; ++C) {
    int Same = 0;
    for (int RunIndex = 0; RunIndex < RUNS; ++RunIndex) {
      const struct FormsResult* A = &Results[0][RunIndex];
      const struct FormsResult* B = &Results[C][RunIndex];
      const int Equal = A->Reason == B->Reason && A->Rip == B->Rip && A->OutputHash == B->OutputHash &&
                        !memcmp(A->Xmm, B->Xmm, sizeof(A->Xmm)) && !memcmp(A->YmmHigh, B->YmmHigh, sizeof(A->YmmHigh)) &&
                        A->Mm0 == B->Mm0;
      if (!Equal) {
        fprintf(stderr, "  %s, %s, run %d (input +%d, output +%d): reason %d rip %" PRIu64 " output %016" PRIx64
                        ", reference reason %d rip %" PRIu64 " output %016" PRIx64 "\n",
                What, Configs[C].Name, RunIndex, RunIndex % 16, (RunIndex * 7 + RunIndex / 16) % 16, B->Reason, B->Rip, B->OutputHash,
                A->Reason, A->Rip, A->OutputHash);
      }
      Same += Equal;
    }
    Check(Same == RUNS, "%s, %s: memory and registers as without ordering in %d of %d runs", What, Configs[C].Name, Same, RUNS);
  }
  munmap(Code, 0x10000);
  munmap(In, REGION);
  munmap(Out, REGION);
}

static void TestVectorTSOForms(void) {
  fexmac_install_fault_handlers();
  RunFormsProgram("forms", FormsProgram, sizeof(FormsProgram));
  RunFormsProgram("forms 2", FormsProgram2, sizeof(FormsProgram2));
}

/* One access across the boundary between two 16K pages, one of them read-only (stores)
 * or inaccessible (loads). Returns whether the run stopped with a page fault at the
 * access; *Written says whether any byte of either page changed; *Loaded whether xmm1
 * changed. Then makes the page accessible, runs the access again, and checks the bytes
 * (or register) it must have written. */
struct FaultOutcome {
  int Faulted;
  int Written;
  int RetryRight;
};

static struct FaultOutcome RunAcrossPage(const struct OrderingConfig* Config, unsigned Offset, unsigned Below, int LowerFaults) {
  struct FaultOutcome Outcome = {0, 0, 0};
  const size_t Page = (size_t)PageSize;
  uint8_t* Code = MapGuestMemory(0x10000);
  uint8_t* Data = MapGuestMemory(2 * Page);
  memcpy(Code, AccessProgram, sizeof(AccessProgram));
  memset(Data, 0x11, 2 * Page);
  uint8_t* Faulting = LowerFaults ? Data : Data + Page;
  const int IsLoad = Offset == LOAD16;
  const unsigned Size = Offset == STORE8 ? 8 : (Offset == STORE32 ? 32 : 16);
  if (!IsLoad) {
    (void)*(volatile uint8_t*)Faulting; /* touched: the store is a permission fault */
  }
  mprotect(Faulting, Page, IsLoad ? PROT_NONE : PROT_READ);
  fexmac_context* Context = CreateContextWithOptions(Config->Options, Config->Count, NULL, NULL);
  const uint64_t Rip = (uint64_t)(uintptr_t)Code + Offset;
  fexmac_thread* Thread = CreateThreadAt(Context, Rip, 0);
  fexmac_x86_state State = GetState(Thread);
  const uint64_t Address = (uint64_t)(uintptr_t)Data + Page - Below;
  State.gpr[RDI] = Address;
  for (int i = 0; i < 16; ++i) {
    State.xmm[i][0] = 0xA1A2A3A4A5A6A7A8ULL + (uint64_t)i;
    State.xmm[i][1] = 0xB1B2B3B4B5B6B7B8ULL + (uint64_t)i;
    State.ymm_high[i][0] = 0xC1C2C3C4C5C6C7C8ULL + (uint64_t)i;
    State.ymm_high[i][1] = 0xD1D2D3D4D5D6D7D8ULL + (uint64_t)i;
  }
  SetState(Thread, &State);
  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state After = GetState(Thread);
  Outcome.Faulted = Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 14 && Stop.instruction_address == Rip && After.rip == Rip;
  mprotect(Faulting, Page, PROT_READ | PROT_WRITE);
  for (size_t i = 0; i < 2 * Page; ++i) {
    Outcome.Written |= Data[i] != 0x11;
  }
  if (IsLoad) {
    Outcome.Written |= After.xmm[1][0] != State.xmm[1][0] || After.xmm[1][1] != State.xmm[1][1];
  }
  Stop = Run(Thread);
  After = GetState(Thread);
  uint8_t Want[32];
  memcpy(Want, State.xmm[0], 16);
  memcpy(Want + 16, State.ymm_high[0], 16);
  if (IsLoad) {
    uint8_t Got[16];
    memcpy(Got, After.xmm[1], 16);
    Outcome.RetryRight = Stop.reason == FEXMAC_STOP_HALT && !memcmp(Got, Data + Page - Below, 16);
  } else {
    Outcome.RetryRight = Stop.reason == FEXMAC_STOP_HALT && !memcmp(Data + Page - Below, Want, Size);
  }
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
  munmap(Data, 2 * Page);
  munmap(Code, 0x10000);
  return Outcome;
}

static void TestVectorTSOPageFaults(void) {
  fexmac_install_fault_handlers();
  static const struct {
    const char* Name;
    unsigned Offset;
  } Forms[] = {{"16-byte store", STORE16}, {"8-byte store", STORE8}, {"32-byte store", STORE32}, {"16-byte load", LOAD16}};
  static const unsigned Belows[] = {1, 4, 8, 12, 15, 16, 20, 24, 31};
  int Cases = 0;
  int Right = 0;
  for (size_t F = 0; F < sizeof(Forms) / sizeof(Forms[0]); ++F) {
    const unsigned Size = Forms[F].Offset == STORE8 ? 8 : (Forms[F].Offset == STORE32 ? 32 : 16);
    for (size_t B = 0; B < sizeof(Belows) / sizeof(Belows[0]); ++B) {
      const unsigned Below = Belows[B];
      if (Below >= Size) {
        continue;
      }
      for (int LowerFaults = 0; LowerFaults < 2; ++LowerFaults) {
        /* The reference is the barrier form; the documented exception is a 32-byte store
         * whose lower page faults after its upper 16 bytes, all in the upper page, were
         * written (docs/macOS-port-status.md, known limits). Any other write before a
         * fault is wrong. */
        const struct FaultOutcome Reference = RunAcrossPage(&Configs[1], Forms[F].Offset, Below, LowerFaults);
        const int KnownPartial = Forms[F].Offset == STORE32 && LowerFaults && Below <= 16;
        for (int C = 1; C < CONFIGS; ++C) {
          const struct FaultOutcome Got = C == 1 ? Reference : RunAcrossPage(&Configs[C], Forms[F].Offset, Below, LowerFaults);
          const int Ok = Got.Faulted && Got.RetryRight && Got.Written == Reference.Written && (!Got.Written || KnownPartial);
          if (!Ok) {
            fprintf(stderr, "  %s, %s, %u bytes below the boundary, %s page faulting: faulted %d, written %d (barriers: %d), retry %s\n",
                    Configs[C].Name, Forms[F].Name, Below, LowerFaults ? "lower" : "upper", Got.Faulted, Got.Written,
                    Reference.Written, Got.RetryRight ? "right" : "WRONG");
          }
          ++Cases;
          Right += Ok;
        }
      }
    }
  }
  Check(Right == Cases, "%d of %d page-crossing accesses fault at the access, write nothing before the fault (except the known "
                        "32-byte case) and are right when run again", Right, Cases);
}

const struct Subtest VectorOrderingSubtests[] = {
  {"vector-tso-forms", TestVectorTSOForms, 0, NULL, "fexmac:", 0},
  {"vector-tso-page-faults", TestVectorTSOPageFaults, 0, NULL, "fexmac:", 0},
};
const size_t VectorOrderingSubtestCount = sizeof(VectorOrderingSubtests) / sizeof(VectorOrderingSubtests[0]);
