// SPDX-License-Identifier: MIT
//
// Times the arm64 instruction sequences that x86 memory-ordering emulation is built from,
// natively, so that the cost of each way of emulating an x86 vector access or string copy
// can be compared before the translator is changed.
//
// Every timed loop is hand-written inline assembly: the compiler only adds the call frame
// around it, so the sequences are exactly the ones named (check with
// `otool -tv OrderingCostProbe`). The sequences that reproduce what the JIT emits today
// follow FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp (LoadMemTSO, StoreMemTSO and
// EmitRepStringInPlace) with FEAT_LRCPC2 present, as on an M4.
//
// Method: a calibration loop (64 dependent 64-bit adds, one cycle each on Apple's
// performance and efficiency cores) runs before every sample of every kernel, so each
// sample has its own clock frequency; a kernel's "cycles" figure uses the frequency of the
// sample it was measured in, which makes it robust against frequency ramping, and its "ns"
// figure is wall time. Each kernel's iteration count is chosen so that one sample takes
// about --ms milliseconds; there are warm-up samples, then --reps measured ones, and the
// minimum and median are reported. Every sample records the CPU number it started and
// ended on (pthread_cpu_number_np), so the report shows where the code actually ran.
//
//   OrderingCostProbe [--qos interactive|initiated|default|utility|background|darwinbg|both]
//                     [--reps N] [--ms T] [--only substring] [--list] [--cpumap]
//
// --qos sets the quality-of-service class of the thread that runs the kernels (default
// interactive). macOS has no affinity interface. User-interactive prefers the performance
// cores, background QoS keeps the thread on the efficiency cores (as long as nothing of
// higher priority waits for it: see RunOn); `darwinbg` adds the Darwin background state
// (setpriority(PRIO_DARWIN_THREAD, 0, PRIO_DARWIN_BG)). `both` runs the whole set twice,
// interactive then background. --cpumap only measures the calibration loop at
// each QoS class many times and prints the frequency seen per CPU number, which tells
// which CPU numbers belong to which core type on this machine.
//
// Build (no dependencies):
//   clang -O2 -mcpu=apple-m1 -o OrderingCostProbe Source/Tools/OrderingCostProbe/Main.c

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <pthread/qos.h>

typedef void (*KernelFn)(uint64_t N, uint8_t* A, uint8_t* B, uint8_t* C);

// Scratch registers every kernel may use. The base registers (%[a], %[b], %[c]) and the
// count (%[n]) are chosen by the compiler from the others.
#define CLOBBERS                                                                                                                  \
  "x9", "x10", "x11", "x12", "x13", "x14", "x15", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", \
    "memory", "cc"

// One kernel: INIT once, then BODY repeated N times.
#define KERNEL_INIT(Name, Init, Body)                                                                            \
  __attribute__((noinline)) static void Name(uint64_t N, uint8_t* A, uint8_t* B, uint8_t* C) {                  \
    __asm__ volatile(Init ".p2align 6\n"                                                                         \
                          "1:\n" Body "subs %[n], %[n], #1\n"                                                  \
                          "b.ne 1b\n"                                                                            \
                     : [n] "+r"(N)                                                                               \
                     : [a] "r"(A), [b] "r"(B), [c] "r"(C)                                                        \
                     : CLOBBERS);                                                                                \
  }
#define KERNEL(Name, Body) KERNEL_INIT(Name, "", Body)

// Eight copies of an access, to distinct 16-byte slots of one 128-byte line, with
// distinct vector registers: M(register index, offset, offset + 8).
#define X8(M) M(0, 0, 8) M(1, 16, 24) M(2, 32, 40) M(3, 48, 56) M(4, 64, 72) M(5, 80, 88) M(6, 96, 104) M(7, 112, 120)
#define X4(M) M(0, 0, 8) M(1, 16, 24) M(2, 32, 40) M(3, 48, 56)

#define REP4(S) S S S S
#define REP8(S) REP4(S) REP4(S)
#define ADD1 "add x9, x9, #1\n"
#define ADD4 REP4(ADD1)
#define ADD16 REP4(ADD4)
#define ADD64 REP4(ADD16)

// ---------------------------------------------------------------------------------------
// Calibration: 64 dependent adds per iteration (the loop counter is a separate chain).
KERNEL(Calibrate, ADD64)

// ---------------------------------------------------------------------------------------
// 16-byte loads.
#define LDR_Q(i, o, o8) "ldr q" #i ", [%[a], #" #o "]\n"
#define LDR_Q_DMB(i, o, o8) "ldr q" #i ", [%[a], #" #o "]\ndmb ishld\n"
#define LDAPUR2_INS2(i, o, o8) \
  "ldapur x9, [%[a], #" #o "]\nldapur x10, [%[a], #" #o8 "]\nins v" #i ".d[0], x9\nins v" #i ".d[1], x10\n"
#define LDAPUR2_FMOV_INS(i, o, o8) \
  "ldapur x9, [%[a], #" #o "]\nldapur x10, [%[a], #" #o8 "]\nfmov d" #i ", x9\nins v" #i ".d[1], x10\n"
#define LDAPUR2_NOP_FMOV_INS(i, o, o8) \
  "ldapur x9, [%[a], #" #o "]\nnop\nldapur x10, [%[a], #" #o8 "]\nnop\nfmov d" #i ", x9\nins v" #i ".d[1], x10\n"
#define LDAPUR2(i, o, o8) "ldapur x9, [%[a], #" #o "]\nldapur x10, [%[a], #" #o8 "]\n"
#define LDP_X(i, o, o8) "ldp x9, x10, [%[a], #" #o "]\n"
#define LDP_X_FMOV_INS(i, o, o8) "ldp x9, x10, [%[a], #" #o "]\nfmov d" #i ", x9\nins v" #i ".d[1], x10\n"
#define LDR_X2(i, o, o8) "ldr x9, [%[a], #" #o "]\nldr x10, [%[a], #" #o8 "]\n"
KERNEL(LdrQ, X8(LDR_Q))
KERNEL(LdrQDmb, X8(LDR_Q_DMB))
KERNEL(Ldapur2Ins2, X8(LDAPUR2_INS2))
KERNEL(Ldapur2FmovIns, X8(LDAPUR2_FMOV_INS))
KERNEL(Ldapur2NopFmovIns, X8(LDAPUR2_NOP_FMOV_INS))
KERNEL(Ldapur2, X8(LDAPUR2))
KERNEL(LdpX, X8(LDP_X))
KERNEL(LdpXFmovIns, X8(LDP_X_FMOV_INS))
KERNEL(LdrX2, X8(LDR_X2))

// 16-byte stores.
#define STR_Q(i, o, o8) "str q" #i ", [%[b], #" #o "]\n"
#define DMB_STR_Q(i, o, o8) "dmb ish\nstr q" #i ", [%[b], #" #o "]\n"
#define UMOV2_STLUR2(i, o, o8) \
  "umov x9, v" #i ".d[0]\numov x10, v" #i ".d[1]\nstlur x9, [%[b], #" #o "]\nstlur x10, [%[b], #" #o8 "]\n"
#define FMOV_UMOV_STLUR2(i, o, o8) \
  "fmov x9, d" #i "\numov x10, v" #i ".d[1]\nstlur x9, [%[b], #" #o "]\nstlur x10, [%[b], #" #o8 "]\n"
#define FMOV_UMOV_NOP_STLUR2(i, o, o8) \
  "fmov x9, d" #i "\numov x10, v" #i ".d[1]\nnop\nstlur x9, [%[b], #" #o "]\nnop\nstlur x10, [%[b], #" #o8 "]\n"
#define STLUR2(i, o, o8) "stlur x9, [%[b], #" #o "]\nstlur x10, [%[b], #" #o8 "]\n"
#define STP_X(i, o, o8) "stp x9, x10, [%[b], #" #o "]\n"
#define STR_X2(i, o, o8) "str x9, [%[b], #" #o "]\nstr x10, [%[b], #" #o8 "]\n"
KERNEL(StrQ, X8(STR_Q))
KERNEL(DmbStrQ, X8(DMB_STR_Q))
KERNEL(Umov2Stlur2, X8(UMOV2_STLUR2))
KERNEL(FmovUmovStlur2, X8(FMOV_UMOV_STLUR2))
KERNEL(FmovUmovNopStlur2, X8(FMOV_UMOV_NOP_STLUR2))
KERNEL(Stlur2, X8(STLUR2))
KERNEL(StpX, X8(STP_X))
KERNEL(StrX2, X8(STR_X2))

// A 16-byte load followed by a 16-byte store (a guest movdqu load then store), each way.
#define LS_PLAIN(i, o, o8) "ldr q" #i ", [%[a], #" #o "]\nstr q" #i ", [%[b], #" #o "]\n"
#define LS_TODAY(i, o, o8) "ldr q" #i ", [%[a], #" #o "]\ndmb ishld\ndmb ish\nstr q" #i ", [%[b], #" #o "]\n"
#define LS_E21(i, o, o8)                                                                                                     \
  "ldapur x9, [%[a], #" #o "]\nldapur x10, [%[a], #" #o8 "]\nfmov d" #i ", x9\nins v" #i ".d[1], x10\nfmov x11, d" #i \
  "\numov x12, v" #i ".d[1]\nstlur x11, [%[b], #" #o "]\nstlur x12, [%[b], #" #o8 "]\n"
KERNEL(LsPlain, X8(LS_PLAIN))
KERNEL(LsToday, X8(LS_TODAY))
KERNEL(LsE21, X8(LS_E21))

// ---------------------------------------------------------------------------------------
// Scalar 8-byte accesses: is the scalar TSO path (ldapur + nop, nop + stlur) free?
#define LDR_X(i, o, o8) "ldr x9, [%[a], #" #o "]\n"
#define LDAPUR_X(i, o, o8) "ldapur x9, [%[a], #" #o "]\n"
#define LDAPUR_X_NOP(i, o, o8) "ldapur x9, [%[a], #" #o "]\nnop\n"
#define STR_X(i, o, o8) "str x9, [%[b], #" #o "]\n"
#define STLUR_X(i, o, o8) "stlur x9, [%[b], #" #o "]\n"
#define NOP_STLUR_X(i, o, o8) "nop\nstlur x9, [%[b], #" #o "]\n"
KERNEL(LdrX, X8(LDR_X))
KERNEL(LdapurX, X8(LDAPUR_X))
KERNEL(LdapurXNop, X8(LDAPUR_X_NOP))
KERNEL(LdaprX, REP8("ldapr x9, [%[a]]\n"))
KERNEL(LdarX, REP8("ldar x9, [%[a]]\n"))
KERNEL(StrX, X8(STR_X))
KERNEL(StlurX, X8(STLUR_X))
KERNEL(NopStlurX, X8(NOP_STLUR_X))
KERNEL(StlrX, REP8("stlr x9, [%[b]]\n"))
// Load-to-use latency: a pointer chain (the word at C points to itself).
KERNEL_INIT(ChainLdr, "mov x9, %[c]\n", REP8("ldr x9, [x9]\n"))
KERNEL_INIT(ChainLdapr, "mov x9, %[c]\n", REP8("ldapr x9, [x9]\n"))
KERNEL_INIT(ChainLdar, "mov x9, %[c]\n", REP8("ldar x9, [x9]\n"))
// A store then an unrelated load (different lines), as in ordinary code: RCpc lets the
// load pass the release store, ldar does not.
#define ST_LD_PLAIN(i, o, o8) "str x9, [%[b], #" #o "]\nldr x10, [%[a], #" #o "]\n"
#define ST_LD_RCPC(i, o, o8) "stlur x9, [%[b], #" #o "]\nldapur x10, [%[a], #" #o "]\n"
KERNEL(StLdPlain, X8(ST_LD_PLAIN))
KERNEL(StLdRcpc, X8(ST_LD_RCPC))
KERNEL(StLdSc, REP8("stlr x9, [%[b]]\nldar x10, [%[a]]\n"))

// ---------------------------------------------------------------------------------------
// Barriers: alone, separated by N dependent adds, and next to a store and a load.
KERNEL(Nop8, REP8("nop\n"))
KERNEL(DmbIshld8, REP8("dmb ishld\n"))
KERNEL(DmbIsh8, REP8("dmb ish\n"))
KERNEL(DmbSy8, REP8("dmb sy\n"))
KERNEL(Alu1, REP4(ADD1))
KERNEL(Alu4, REP4(ADD4))
KERNEL(Alu16, REP4(ADD16))
KERNEL(Alu64, REP4(ADD64))
#define BARRIER_AFTER_ALU(Bar) \
  KERNEL(Alu1##Bar, REP4(ADD1 "dmb " #Bar "\n")) \
  KERNEL(Alu4##Bar, REP4(ADD4 "dmb " #Bar "\n")) \
  KERNEL(Alu16##Bar, REP4(ADD16 "dmb " #Bar "\n")) \
  KERNEL(Alu64##Bar, REP4(ADD64 "dmb " #Bar "\n"))
BARRIER_AFTER_ALU(ishld)
BARRIER_AFTER_ALU(ish)
BARRIER_AFTER_ALU(sy)
#define SL(i, o, o8) "str x10, [%[b], #" #o "]\nldr x11, [%[a], #" #o "]\n"
#define SL_ISHLD(i, o, o8) SL(i, o, o8) "dmb ishld\n"
#define SL_ISH(i, o, o8) SL(i, o, o8) "dmb ish\n"
#define SL_SY(i, o, o8) SL(i, o, o8) "dmb sy\n"
KERNEL(StLd, X8(SL))
KERNEL(StLdIshld, X8(SL_ISHLD))
KERNEL(StLdIsh, X8(SL_ISH))
KERNEL(StLdSy, X8(SL_SY))
// A pending store then a load barrier: does dmb ishld wait for the store buffer?
#define ST_ISHLD(i, o, o8) "str q" #i ", [%[b], #" #o "]\ndmb ishld\n"
#define ST_ISH(i, o, o8) "str q" #i ", [%[b], #" #o "]\ndmb ish\n"
KERNEL(StQIshld, X8(ST_ISHLD))
KERNEL(StLdAlu16, REP4("str x10, [%[b]]\nldr x11, [%[a], #64]\n" ADD16))
KERNEL(StLdAlu16Ish, REP4("str x10, [%[b]]\nldr x11, [%[a], #64]\n" ADD16 "dmb ish\n"))
KERNEL(StQAlu16, REP4("str q0, [%[b]]\n" ADD16))
KERNEL(StQAlu16Ish, REP4("str q0, [%[b]]\n" ADD16 "dmb ish\n"))
KERNEL(StQIsh, X8(ST_ISH))

// ---------------------------------------------------------------------------------------
// Store-to-load forwarding round trips (latency: each step depends on the previous). Every
// 16-byte chain starts and ends in v0 and has the same register moves as the E2.1 forms
// where it needs them; "moves only" is the same moves without memory, to subtract.
KERNEL_INIT(Fwd8to8, "mov x9, #1\n", REP4("str x9, [%[b]]\nldr x9, [%[b]]\n"))
KERNEL_INIT(Fwd8to8Rcpc, "mov x9, #1\n", REP4("stlur x9, [%[b]]\nldapur x9, [%[b]]\n"))
#define MOVES_OUT "fmov x9, d0\numov x10, v0.d[1]\n"
#define MOVES_IN "fmov d0, x9\nins v0.d[1], x10\n"
KERNEL(FwdMoves, REP4(MOVES_OUT MOVES_IN))
KERNEL(FwdQQ, REP4("str q0, [%[b]]\nldr q0, [%[b]]\n"))
KERNEL(FwdQQToday, REP4("dmb ish\nstr q0, [%[b]]\nldr q0, [%[b]]\ndmb ishld\n"))
KERNEL(FwdQQMoves, REP4(MOVES_OUT MOVES_IN "str q0, [%[b]]\nldr q0, [%[b]]\n"))
KERNEL(FwdX2Q, REP4(MOVES_OUT "str x9, [%[b]]\nstr x10, [%[b], #8]\nldr q0, [%[b]]\n"))
KERNEL(FwdX2QRel, REP4(MOVES_OUT "stlur x9, [%[b]]\nstlur x10, [%[b], #8]\nldr q0, [%[b]]\n"))
KERNEL(FwdQX2, REP4("str q0, [%[b]]\nldr x9, [%[b]]\nldr x10, [%[b], #8]\n" MOVES_IN))
KERNEL(FwdQX2Acq, REP4("str q0, [%[b]]\nldapur x9, [%[b]]\nldapur x10, [%[b], #8]\n" MOVES_IN))
KERNEL(FwdE21, REP4(MOVES_OUT "stlur x9, [%[b]]\nstlur x10, [%[b], #8]\nldapur x9, [%[b]]\nldapur x10, [%[b], #8]\n" MOVES_IN))
KERNEL(FwdX2X2Plain, REP4(MOVES_OUT "str x9, [%[b]]\nstr x10, [%[b], #8]\nldr x9, [%[b]]\nldr x10, [%[b], #8]\n" MOVES_IN))

// ---------------------------------------------------------------------------------------
// Copies. A copy of TOTAL bytes in runs of RUN bytes, STEP bytes per inner iteration:
// RUN_PRE/RUN_POST bracket each run, BODY copies STEP bytes from [x11] to [x12].
#define COPY(Name, Total, Run, Step, RunPre, Body, RunPost)                                                                \
  KERNEL(Name, "mov x11, %[a]\nmov x12, %[b]\nmov x14, #(" #Total "/" #Run ")\n"                                         \
               "2:\n" RunPre "mov x13, #(" #Run "/" #Step ")\n"                                                           \
               "3:\n" Body "add x11, x11, #" #Step "\nadd x12, x12, #" #Step "\nsubs x13, x13, #1\nb.ne 3b\n" RunPost \
               "subs x14, x14, #1\nb.ne 2b\n")
#define C_LDRQ(i, o, o8) "ldr q" #i ", [x11, #" #o "]\nstr q" #i ", [x12, #" #o "]\n"
#define C_TODAY(i, o, o8) "ldr q" #i ", [x11, #" #o "]\ndmb ishld\ndmb ish\nstr q" #i ", [x12, #" #o "]\n"
#define C_TODAY_MERGED(i, o, o8) "ldr q" #i ", [x11, #" #o "]\ndmb ish\nstr q" #i ", [x12, #" #o "]\n"
#define C_E21(i, o, o8)                                                                                                     \
  "ldapur x9, [x11, #" #o "]\nldapur x10, [x11, #" #o8 "]\nfmov d" #i ", x9\nins v" #i ".d[1], x10\nfmov x9, d" #i \
  "\numov x10, v" #i ".d[1]\nstlur x9, [x12, #" #o "]\nstlur x10, [x12, #" #o8 "]\n"
#define C_E21_GPR(i, o, o8) "ldapur x9, [x11, #" #o "]\nldapur x10, [x11, #" #o8 "]\nstlur x9, [x12, #" #o "]\nstlur x10, [x12, #" #o8 "]\n"
#define C_LDPX(i, o, o8) "ldp x9, x10, [x11, #" #o "]\nstp x9, x10, [x12, #" #o "]\n"
#define C_LDPQ "ldp q16, q17, [x11]\nldp q18, q19, [x11, #32]\nstp q16, q17, [x12]\nstp q18, q19, [x12, #32]\n"
#define BAR "dmb ish\n"
COPY(Copy4kLdrQ, 4096, 4096, 64, "", X4(C_LDRQ), "")
COPY(Copy4kLdpQ, 4096, 4096, 64, "", C_LDPQ, "")
COPY(Copy4kToday, 4096, 4096, 64, "", X4(C_TODAY), "")
COPY(Copy4kTodayMerged, 4096, 4096, 64, "", X4(C_TODAY_MERGED), "")
COPY(Copy4kE21, 4096, 4096, 64, "", X4(C_E21), "")
COPY(Copy4kE21Gpr, 4096, 4096, 64, "", X4(C_E21_GPR), "")
COPY(Copy4kLdpX, 4096, 4096, 64, "", X4(C_LDPX), "")
COPY(Copy4kBracketLdpQ, 4096, 4096, 64, BAR, C_LDPQ, BAR)
COPY(Copy4kBracketLdrQ, 4096, 4096, 64, BAR, X4(C_LDRQ), BAR)
COPY(Copy4kPer64LdpQ, 4096, 64, 64, BAR, C_LDPQ, BAR)
COPY(Copy16kLdpQ, 16384, 16384, 64, "", C_LDPQ, "")
COPY(Copy16kOnceLdpQ, 16384, 16384, 64, BAR, C_LDPQ, BAR)
COPY(Copy16kPerPageLdpQ, 16384, 4096, 64, BAR, C_LDPQ, BAR)
COPY(Copy16kPer64LdpQ, 16384, 64, 64, BAR, C_LDPQ, BAR)
COPY(Copy16kToday, 16384, 16384, 64, "", X4(C_TODAY), "")

// Element loops over 4KB.
#define ELEM(Name, Bytes, Body) \
  KERNEL(Name, "mov x11, %[a]\nmov x12, %[b]\nmov x13, #(4096/" #Bytes ")\n2:\n" Body "subs x13, x13, #1\nb.ne 2b\n")
ELEM(BytesPlain, 1, "ldrb w9, [x11], #1\nstrb w9, [x12], #1\n")
ELEM(BytesRcpc, 1, "ldaprb w9, [x11]\nstlrb w9, [x12]\nadd x11, x11, #1\nadd x12, x12, #1\n")
ELEM(QwordsRcpc, 8, "ldapr x9, [x11]\nstlr x9, [x12]\nadd x11, x11, #8\nadd x12, x12, #8\n")
ELEM(QwordsPlain, 8, "ldr x9, [x11], #8\nstr x9, [x12], #8\n")

// rep movsb / rep movsq with string ordering on, exactly as EmitRepStringInPlace emits the
// forward element loop with FEAT_LRCPC: element, pointer and count updates, the interrupt
// fault page store every 64 elements (x15 stands for the guest state register).
#define REP_ELEMENT_LOOP(Name, Elements, Load, Store, Bytes)                                                 \
  KERNEL(Name, "mov x11, %[a]\nmov x12, %[b]\nmov x13, #" #Elements "\nmov x15, %[c]\n"                     \
               "2:\n" Load Store "add x12, x12, #" #Bytes "\nadd x11, x11, #" #Bytes "\nsub x13, x13, #1\n" \
               "and x14, x13, #63\ncbnz x14, 4f\nstr xzr, [x15, #64]\n4:\ncbnz x13, 2b\n")
REP_ELEMENT_LOOP(RepMovsbToday, 4096, "ldaprb w9, [x11]\n", "stlrb w9, [x12]\n", 1)
REP_ELEMENT_LOOP(RepMovsqToday, 512, "ldapr x9, [x11]\nnop\nnop\n", "stlr x9, [x12]\n", 8)
REP_ELEMENT_LOOP(RepMovsbPlainElem, 4096, "ldrb w9, [x11]\n", "strb w9, [x12]\n", 1)
// rep movsb with string ordering off, as the chunk path emits it for 4KB (one page run:
// 32-byte ldp/stp of q registers, post-indexed store, the count updates), and the same
// with a barrier before and after the run (the bracketed form of experiment E2.2).
#define REP_CHUNK_RUN(Name, Pre, Post)                                                                             \
  KERNEL(Name, "mov x11, %[a]\nmov x12, %[b]\nmov x13, #4096\nmov x15, %[c]\n" Pre "mov x10, #128\n"               \
               "2:\nldp q16, q17, [x11]\nstp q16, q17, [x12], #32\nadd x11, x11, #32\nsub x13, x13, #32\n" \
               "sub x10, x10, #1\ncbnz x10, 2b\nstr xzr, [x15, #64]\n" Post)
REP_CHUNK_RUN(RepMovsbChunk, "", "")
REP_CHUNK_RUN(RepMovsbChunkBracket, BAR, BAR)

// ---------------------------------------------------------------------------------------

typedef struct {
  const char* Group;
  const char* Name;
  KernelFn Fn;
  double Ops;   // operations per loop iteration
  double Bytes; // bytes copied or accessed per loop iteration (0: none)
  int Base;     // index of the kernel this one is compared with (-1: none), set in main
  const char* BaseName;
  const char* Note;
} Kernel;

#define K(G, N, F, O, B, BN, Note) {G, N, F, O, B, -1, BN, Note}
static Kernel Kernels[] = {
  K("load16", "ldr q", LdrQ, 8, 128, NULL, "plain 16-byte load"),
  K("load16", "ldr q + dmb ishld", LdrQDmb, 8, 128, "ldr q", "today's vector TSO load"),
  K("load16", "2 ldapur x + 2 ins", Ldapur2Ins2, 8, 128, "ldr q", "E2.1 as written in the handoff"),
  K("load16", "2 ldapur x + fmov + ins", Ldapur2FmovIns, 8, 128, "ldr q", "E2.1, fmov breaks the merge dependency"),
  K("load16", "2 (ldapur x + nop) + fmov + ins", Ldapur2NopFmovIns, 8, 128, "ldr q", "E2.1 with the scalar path's nop slots"),
  K("load16", "2 ldapur x", Ldapur2, 8, 128, "ldr q", "the two acquire loads alone"),
  K("load16", "ldp x", LdpX, 8, 128, "ldr q", "plain pair of 64-bit loads"),
  K("load16", "ldp x + fmov + ins", LdpXFmovIns, 8, 128, "ldr q", NULL),
  K("load16", "2 ldr x", LdrX2, 8, 128, "ldr q", NULL),
  K("store16", "str q", StrQ, 8, 128, NULL, "plain 16-byte store"),
  K("store16", "dmb ish + str q", DmbStrQ, 8, 128, "str q", "today's vector TSO store"),
  K("store16", "2 umov + 2 stlur x", Umov2Stlur2, 8, 128, "str q", "E2.1 as written in the handoff"),
  K("store16", "fmov + umov + 2 stlur x", FmovUmovStlur2, 8, 128, "str q", "E2.1"),
  K("store16", "fmov + umov + 2 (nop + stlur x)", FmovUmovNopStlur2, 8, 128, "str q", "E2.1 with the scalar path's nop slots"),
  K("store16", "2 stlur x", Stlur2, 8, 128, "str q", "the two release stores alone"),
  K("store16", "stp x", StpX, 8, 128, "str q", "plain pair of 64-bit stores"),
  K("store16", "2 str x", StrX2, 8, 128, "str q", NULL),
  K("ldst16", "ldr q; str q", LsPlain, 8, 128, NULL, "a vector load then a vector store"),
  K("ldst16", "ldr q; dmb ishld; dmb ish; str q", LsToday, 8, 128, "ldr q; str q", "today, ordering on"),
  K("ldst16", "E2.1 load; E2.1 store", LsE21, 8, 128, "ldr q; str q", "2 ldapur + fmov + ins; fmov + umov + 2 stlur"),
  K("scalar8", "ldr x", LdrX, 8, 64, NULL, NULL),
  K("scalar8", "ldapur x", LdapurX, 8, 64, "ldr x", NULL),
  K("scalar8", "ldapur x + nop", LdapurXNop, 8, 64, "ldr x", "today's scalar TSO load"),
  K("scalar8", "ldapr x", LdaprX, 8, 64, "ldr x", "same address every time"),
  K("scalar8", "ldar x", LdarX, 8, 64, "ldr x", "same address every time"),
  K("scalar8", "str x", StrX, 8, 64, NULL, NULL),
  K("scalar8", "stlur x", StlurX, 8, 64, "str x", NULL),
  K("scalar8", "nop + stlur x", NopStlurX, 8, 64, "str x", "today's scalar TSO store"),
  K("scalar8", "stlr x", StlrX, 8, 64, "str x", "same address every time"),
  K("scalar8", "chain ldr x", ChainLdr, 8, 0, NULL, "load-to-use latency"),
  K("scalar8", "chain ldapr x", ChainLdapr, 8, 0, "chain ldr x", "load-to-use latency"),
  K("scalar8", "chain ldar x", ChainLdar, 8, 0, "chain ldr x", "load-to-use latency"),
  K("scalar8", "str x; ldr x", StLdPlain, 8, 0, NULL, "store then unrelated load"),
  K("scalar8", "stlur x; ldapur x", StLdRcpc, 8, 0, "str x; ldr x", "RCpc: the load may pass the store"),
  K("scalar8", "stlr x; ldar x", StLdSc, 8, 0, "str x; ldr x", "sequentially consistent: it may not"),
  K("barrier", "nop", Nop8, 8, 0, NULL, NULL),
  K("barrier", "dmb ishld alone", DmbIshld8, 8, 0, NULL, "back to back"),
  K("barrier", "dmb ish alone", DmbIsh8, 8, 0, NULL, "back to back"),
  K("barrier", "dmb sy alone", DmbSy8, 8, 0, NULL, "back to back"),
  K("barrier", "1 add", Alu1, 4, 0, NULL, "per group of adds"),
  K("barrier", "1 add + dmb ishld", Alu1ishld, 4, 0, "1 add", NULL),
  K("barrier", "1 add + dmb ish", Alu1ish, 4, 0, "1 add", NULL),
  K("barrier", "1 add + dmb sy", Alu1sy, 4, 0, "1 add", NULL),
  K("barrier", "4 adds", Alu4, 4, 0, NULL, "per group of adds"),
  K("barrier", "4 adds + dmb ishld", Alu4ishld, 4, 0, "4 adds", NULL),
  K("barrier", "4 adds + dmb ish", Alu4ish, 4, 0, "4 adds", NULL),
  K("barrier", "4 adds + dmb sy", Alu4sy, 4, 0, "4 adds", NULL),
  K("barrier", "16 adds", Alu16, 4, 0, NULL, "per group of adds"),
  K("barrier", "16 adds + dmb ishld", Alu16ishld, 4, 0, "16 adds", NULL),
  K("barrier", "16 adds + dmb ish", Alu16ish, 4, 0, "16 adds", NULL),
  K("barrier", "16 adds + dmb sy", Alu16sy, 4, 0, "16 adds", NULL),
  K("barrier", "64 adds", Alu64, 4, 0, NULL, "per group of adds"),
  K("barrier", "64 adds + dmb ishld", Alu64ishld, 4, 0, "64 adds", NULL),
  K("barrier", "64 adds + dmb ish", Alu64ish, 4, 0, "64 adds", NULL),
  K("barrier", "64 adds + dmb sy", Alu64sy, 4, 0, "64 adds", NULL),
  K("barrier", "str x; ldr x (same line)", StLd, 8, 0, NULL, "per store-load pair"),
  K("barrier", "str x; ldr x; dmb ishld", StLdIshld, 8, 0, "str x; ldr x (same line)", NULL),
  K("barrier", "str x; ldr x; dmb ish", StLdIsh, 8, 0, "str x; ldr x (same line)", NULL),
  K("barrier", "str x; ldr x; dmb sy", StLdSy, 8, 0, "str x; ldr x (same line)", NULL),
  K("barrier", "str x; ldr x; 16 adds", StLdAlu16, 4, 0, NULL, "pending accesses, then work"),
  K("barrier", "str x; ldr x; 16 adds; dmb ish", StLdAlu16Ish, 4, 0, "str x; ldr x; 16 adds", NULL),
  K("barrier", "str q; 16 adds", StQAlu16, 4, 0, NULL, NULL),
  K("barrier", "str q; 16 adds; dmb ish", StQAlu16Ish, 4, 0, "str q; 16 adds", NULL),
  K("barrier", "str q; dmb ishld", StQIshld, 8, 0, "str q", "does a load barrier wait for stores?"),
  K("barrier", "str q; dmb ish", StQIsh, 8, 0, "str q", NULL),
  K("forward", "str x -> ldr x", Fwd8to8, 4, 0, NULL, "round trip latency"),
  K("forward", "stlur x -> ldapur x", Fwd8to8Rcpc, 4, 0, "str x -> ldr x", NULL),
  K("forward", "moves only (fmov+umov, fmov+ins)", FwdMoves, 4, 0, NULL, "subtract from the rows with moves"),
  K("forward", "str q -> ldr q", FwdQQ, 4, 0, NULL, "16 to 16"),
  K("forward", "dmb ish; str q -> ldr q; dmb ishld", FwdQQToday, 4, 0, "str q -> ldr q", "today, ordering on"),
  K("forward", "moves + str q -> ldr q", FwdQQMoves, 4, 0, NULL, "16 to 16 with the moves, for the rows below"),
  K("forward", "2 str x -> ldr q", FwdX2Q, 4, 0, "moves + str q -> ldr q", "8+8 read as 16"),
  K("forward", "2 stlur x -> ldr q", FwdX2QRel, 4, 0, "moves + str q -> ldr q", "E2.1 store, plain vector load"),
  K("forward", "str q -> 2 ldr x", FwdQX2, 4, 0, "moves + str q -> ldr q", "16 read as 8+8"),
  K("forward", "str q -> 2 ldapur x", FwdQX2Acq, 4, 0, "moves + str q -> ldr q", "16 read as 8+8 acquire"),
  K("forward", "2 stlur x -> 2 ldapur x (E2.1 both)", FwdE21, 4, 0, "moves + str q -> ldr q", NULL),
  K("forward", "2 str x -> 2 ldr x", FwdX2X2Plain, 4, 0, "moves + str q -> ldr q", NULL),
  K("copy", "4K ldr q/str q", Copy4kLdrQ, 1, 4096, NULL, "plain"),
  K("copy", "4K ldp q/stp q", Copy4kLdpQ, 1, 4096, NULL, "plain, 32 bytes per pair"),
  K("copy", "4K today (ldr q; dmb ishld; dmb ish; str q)", Copy4kToday, 1, 4096, "4K ldr q/str q", "copy_loop's barriers"),
  K("copy", "4K today, barriers merged (ldr q; dmb ish; str q)", Copy4kTodayMerged, 1, 4096, "4K ldr q/str q", "E2.3"),
  K("copy", "4K E2.1 (ldapur/ins, umov/stlur)", Copy4kE21, 1, 4096, "4K ldr q/str q", NULL),
  K("copy", "4K 2 ldapur x/2 stlur x", Copy4kE21Gpr, 1, 4096, "4K ldr q/str q", "no vector moves"),
  K("copy", "4K ldp x/stp x", Copy4kLdpX, 1, 4096, "4K ldr q/str q", NULL),
  K("copy", "4K ldp q/stp q, dmb ish before and after", Copy4kBracketLdpQ, 1, 4096, "4K ldp q/stp q", "E2.2"),
  K("copy", "4K ldr q/str q, dmb ish before and after", Copy4kBracketLdrQ, 1, 4096, "4K ldr q/str q", NULL),
  K("copy", "4K ldp q/stp q, dmb ish around every 64B", Copy4kPer64LdpQ, 1, 4096, "4K ldp q/stp q", NULL),
  K("copy", "16K ldp q/stp q", Copy16kLdpQ, 1, 16384, NULL, "plain"),
  K("copy", "16K ldp q/stp q, dmb ish around the whole", Copy16kOnceLdpQ, 1, 16384, "16K ldp q/stp q", NULL),
  K("copy", "16K ldp q/stp q, dmb ish around each 4K page", Copy16kPerPageLdpQ, 1, 16384, "16K ldp q/stp q", NULL),
  K("copy", "16K ldp q/stp q, dmb ish around every 64B", Copy16kPer64LdpQ, 1, 16384, "16K ldp q/stp q", NULL),
  K("copy", "16K today (ldr q; dmb ishld; dmb ish; str q)", Copy16kToday, 1, 16384, "16K ldp q/stp q", NULL),
  K("string", "4K bytes ldrb/strb", BytesPlain, 1, 4096, NULL, "minimal loop"),
  K("string", "4K bytes ldaprb/stlrb", BytesRcpc, 1, 4096, "4K bytes ldrb/strb", "minimal loop"),
  K("string", "4K qwords ldr/str", QwordsPlain, 1, 4096, NULL, "minimal loop"),
  K("string", "4K qwords ldapr/stlr", QwordsRcpc, 1, 4096, "4K qwords ldr/str", "minimal loop"),
  K("string", "rep movsb 4K, ordering on (today)", RepMovsbToday, 1, 4096, "rep movsb 4K, ordering off (today)", "JIT's loop"),
  K("string", "rep movsq 4K, ordering on (today)", RepMovsqToday, 1, 4096, "rep movsb 4K, ordering off (today)", "JIT's loop"),
  K("string", "rep movsb 4K, plain element loop", RepMovsbPlainElem, 1, 4096, "rep movsb 4K, ordering off (today)", NULL),
  K("string", "rep movsb 4K, ordering off (today)", RepMovsbChunk, 1, 4096, NULL, "JIT's 32-byte chunk run"),
  K("string", "rep movsb 4K, chunk run + dmb ish before/after", RepMovsbChunkBracket, 1, 4096, "rep movsb 4K, ordering off (today)",
    "E2.2 per page run"),
};
#define NKERNELS (sizeof(Kernels) / sizeof(Kernels[0]))

// ---------------------------------------------------------------------------------------

static uint8_t *BufA, *BufB, *BufC;

static uint64_t NowNs(void) {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static int CpuNumber(void) {
  size_t N = 0;
  return pthread_cpu_number_np(&N) == 0 ? (int)N : -1;
}

static uint64_t TimeRun(KernelFn Fn, uint64_t N) {
  const uint64_t T0 = NowNs();
  Fn(N, BufA, BufB, BufC);
  return NowNs() - T0;
}

// Iterations for about Ms milliseconds.
static uint64_t SizeFor(KernelFn Fn, double Ms) {
  uint64_t N = 16;
  for (;;) {
    const uint64_t T = TimeRun(Fn, N);
    if (T > Ms * 1e6 / 8 || N > (1ULL << 40)) {
      const double Scaled = (double)N * (Ms * 1e6) / (double)(T ? T : 1);
      return Scaled < 1 ? 1 : (uint64_t)Scaled;
    }
    N *= 2;
  }
}

static int CompareDouble(const void* A, const void* B) {
  const double X = *(const double*)A, Y = *(const double*)B;
  return X < Y ? -1 : X > Y;
}

static double Median(double* V, int N) {
  qsort(V, N, sizeof(double), CompareDouble);
  return N % 2 ? V[N / 2] : (V[N / 2 - 1] + V[N / 2]) / 2;
}

typedef struct {
  double NsMin, NsMed, CycMed, GHzMin, GHzMax;
  uint32_t CpuMask;
} Result;

static int Reps = 21;
static double SampleMs = 1.0;
static const double CalMs = 0.25;
static uint64_t CalN;

static double CalibrateGHz(void) {
  const uint64_t T = TimeRun(Calibrate, CalN);
  return 64.0 * (double)CalN / (double)T;
}

static Result Measure(const Kernel* Kn) {
  Result R = {0};
  const uint64_t N = SizeFor(Kn->Fn, SampleMs);
  double Ns[256], Cyc[256];
  int Count = Reps > 256 ? 256 : Reps;
  R.GHzMin = 1e9;
  for (int i = -3; i < Count; ++i) {
    const int CpuBefore = CpuNumber();
    const double GHz = CalibrateGHz();
    const uint64_t T = TimeRun(Kn->Fn, N);
    const int CpuAfter = CpuNumber();
    if (i < 0) {
      continue; // warm-up
    }
    Ns[i] = (double)T / ((double)N * Kn->Ops);
    Cyc[i] = Ns[i] * GHz;
    if (GHz < R.GHzMin) {
      R.GHzMin = GHz;
    }
    if (GHz > R.GHzMax) {
      R.GHzMax = GHz;
    }
    if (CpuBefore >= 0 && CpuBefore < 32) {
      R.CpuMask |= 1U << CpuBefore;
    }
    if (CpuAfter >= 0 && CpuAfter < 32) {
      R.CpuMask |= 1U << CpuAfter;
    }
  }
  double Sorted[256];
  memcpy(Sorted, Ns, sizeof(double) * Count);
  qsort(Sorted, Count, sizeof(double), CompareDouble);
  R.NsMin = Sorted[0];
  R.NsMed = Median(Ns, Count);
  R.CycMed = Median(Cyc, Count);
  return R;
}

static void CpuList(uint32_t Mask, char* Out, size_t Size) {
  Out[0] = 0;
  for (int c = 0; c < 32; ++c) {
    if (Mask & (1U << c)) {
      const size_t L = strlen(Out);
      snprintf(Out + L, Size - L, "%s%d", L ? "," : "", c);
    }
  }
}

static const char* Only;

typedef struct {
  qos_class_t Qos;
  const char* Name;
  int DarwinBG; // also put the thread in the Darwin background state
  int Done;
} QosRun;

// Applies the run's scheduling class to the calling thread: a QoS class, and for darwinbg
// also the Darwin background state (setpriority PRIO_DARWIN_THREAD/PRIO_DARWIN_BG, which
// `taskpolicy -b` applies to a whole process).
static void ApplyClass(const QosRun* Q) {
  pthread_set_qos_class_self_np(Q->Qos, 0);
  if (Q->DarwinBG && setpriority(PRIO_DARWIN_THREAD, 0, PRIO_DARWIN_BG) != 0) {
    perror("setpriority(PRIO_DARWIN_THREAD, PRIO_DARWIN_BG)");
  }
}

static void PrintLoad(const char* When) {
  double L[3] = {0};
  getloadavg(L, 3);
  printf("load average %s: %.2f %.2f %.2f\n", When, L[0], L[1], L[2]);
}

static void* RunAll(void* Arg) {
  const QosRun* Q = Arg;
  ApplyClass(Q);
  // Warm-up: about 300 ms of calibration, so the core has ramped up.
  CalN = 4096;
  const uint64_t WarmEnd = NowNs() + 300000000ULL;
  while (NowNs() < WarmEnd) {
    Calibrate(1 << 16, BufA, BufB, BufC);
  }
  CalN = SizeFor(Calibrate, CalMs);

  qos_class_t Got = QOS_CLASS_UNSPECIFIED;
  int Rel = 0;
  pthread_get_qos_class_np(pthread_self(), &Got, &Rel);
  printf("\n=== QoS %s (thread reports class 0x%x); calibration %.2f GHz on CPU %d ===\n", Q->Name, Got, CalibrateGHz(), CpuNumber());
  printf("%-8s %-52s %9s %9s %8s %9s %9s %11s %s\n", "group", "kernel", "ns min", "ns med", "cyc med", "ns/B med", "cyc/base",
         "GHz", "CPUs");
  uint32_t AllMask = 0;
  double GMin = 1e9, GMax = 0;
  static Result Res[NKERNELS];
  static int Done[NKERNELS];
  // Measure everything first, then print, so that every row can be compared with its base.
  for (size_t i = 0; i < NKERNELS; ++i) {
    Kernel* Kn = &Kernels[i];
    Done[i] = 0;
    if (Only && !strstr(Kn->Name, Only) && !strstr(Kn->Group, Only) && !(Kn->Base >= 0 && strstr(Kernels[Kn->Base].Name, Only))) {
      continue;
    }
    Res[i] = Measure(Kn);
    Done[i] = 1;
  }
  for (size_t i = 0; i < NKERNELS; ++i) {
    // A base that was filtered out is measured now.
    if (Done[i] && Kernels[i].Base >= 0 && !Done[Kernels[i].Base]) {
      Res[Kernels[i].Base] = Measure(&Kernels[Kernels[i].Base]);
      Done[Kernels[i].Base] = 1;
    }
  }
  for (size_t i = 0; i < NKERNELS; ++i) {
    const Kernel* Kn = &Kernels[i];
    if (!Done[i]) {
      continue;
    }
    const Result R = Res[i];
    char Rel[32] = "";
    if (Kn->Base >= 0 && Done[Kn->Base] && Res[Kn->Base].NsMed > 0) {
      snprintf(Rel, sizeof(Rel), "x%.2f", R.CycMed / Res[Kn->Base].CycMed);
    }
    char NsB[32] = "";
    if (Kn->Bytes > 0) {
      snprintf(NsB, sizeof(NsB), "%.4f", R.NsMed * Kn->Ops / Kn->Bytes);
    }
    char Cpus[96];
    CpuList(R.CpuMask, Cpus, sizeof(Cpus));
    printf("%-8s %-52s %9.3f %9.3f %8.2f %9s %9s %5.2f-%5.2f %s\n", Kn->Group, Kn->Name, R.NsMin, R.NsMed, R.CycMed, NsB, Rel, R.GHzMin,
           R.GHzMax, Cpus);
    AllMask |= R.CpuMask;
    if (R.GHzMin < GMin) {
      GMin = R.GHzMin;
    }
    if (R.GHzMax > GMax) {
      GMax = R.GHzMax;
    }
  }
  char Cpus[96];
  CpuList(AllMask, Cpus, sizeof(Cpus));
  printf("=== QoS %s: calibration frequency %.2f to %.2f GHz over all samples; CPUs seen: %s ===\n", Q->Name, GMin, GMax, Cpus);
  __atomic_store_n(&((QosRun*)Arg)->Done, 1, __ATOMIC_RELEASE);
  return NULL;
}

// Frequency per CPU number, at each QoS class.
static void* CpuMap(void* Arg) {
  const QosRun* Q = Arg;
  ApplyClass(Q);
  CalN = 4096;
  const uint64_t WarmEnd = NowNs() + 300000000ULL;
  while (NowNs() < WarmEnd) {
    Calibrate(1 << 16, BufA, BufB, BufC);
  }
  CalN = SizeFor(Calibrate, 1.0);
  double Sum[32] = {0}, Max[32] = {0};
  int Count[32] = {0};
  for (int i = 0; i < 2000; ++i) {
    const int C0 = CpuNumber();
    const double G = CalibrateGHz();
    const int C1 = CpuNumber();
    if (C0 == C1 && C0 >= 0 && C0 < 32) {
      Sum[C0] += G;
      Count[C0]++;
      if (G > Max[C0]) {
        Max[C0] = G;
      }
    }
  }
  printf("QoS %-12s", Q->Name);
  for (int c = 0; c < 32; ++c) {
    if (Count[c]) {
      printf("  cpu%d: %d samples, mean %.2f max %.2f GHz;", c, Count[c], Sum[c] / Count[c], Max[c]);
    }
  }
  printf("\n");
  __atomic_store_n(&((QosRun*)Arg)->Done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void RunOn(void* (*Fn)(void*), QosRun* Q) {
  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  pthread_attr_set_qos_class_np(&Attr, Q->Qos, 0);
  pthread_t T;
  Q->Done = 0;
  if (pthread_create(&T, &Attr, Fn, Q) != 0) {
    fprintf(stderr, "pthread_create failed\n");
    exit(1);
  }
  // Wait without blocking in pthread_join: a thread blocked in pthread_join lends its own
  // priority to the thread it waits for (seen on macOS 27.0.1: a background-QoS thread ran
  // on the performance cores while the main thread was in pthread_join, and on the
  // efficiency cores while it polled).
  while (!__atomic_load_n(&Q->Done, __ATOMIC_ACQUIRE)) {
    usleep(2000);
  }
  pthread_join(T, NULL);
  pthread_attr_destroy(&Attr);
}

static void PrintSysctl(const char* Name) {
  char S[128];
  size_t SL = sizeof(S);
  int64_t V = 0;
  size_t VL = sizeof(V);
  if (sysctlbyname(Name, S, &SL, NULL, 0) == 0 && SL > 0 && SL < sizeof(S) && S[SL - 1] == 0 && SL > 1 && S[0] >= 'A') {
    printf("  %s: %s\n", Name, S);
  } else if (sysctlbyname(Name, &V, &VL, NULL, 0) == 0) {
    printf("  %s: %lld\n", Name, (long long)(VL == 4 ? (int64_t)(int32_t)V : V));
  } else {
    printf("  %s: (not present: %s)\n", Name, strerror(errno));
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char* QosName = "interactive";
  int List = 0, Map = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--qos") && i + 1 < argc) {
      QosName = argv[++i];
    } else if (!strcmp(argv[i], "--reps") && i + 1 < argc) {
      Reps = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--ms") && i + 1 < argc) {
      SampleMs = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
      Only = argv[++i];
    } else if (!strcmp(argv[i], "--list")) {
      List = 1;
    } else if (!strcmp(argv[i], "--cpumap")) {
      Map = 1;
    } else {
      fprintf(stderr, "usage: %s [--qos interactive|initiated|default|utility|background|darwinbg|both] [--reps N] [--ms T] [--only substring]\n"
                      "       [--list] [--cpumap]\n",
              argv[0]);
      return 2;
    }
  }
  if (Reps < 1) {
    Reps = 1;
  }
  for (size_t i = 0; i < NKERNELS; ++i) {
    if (List) {
      printf("%-8s %-52s %s\n", Kernels[i].Group, Kernels[i].Name, Kernels[i].Note ? Kernels[i].Note : "");
    }
    if (Kernels[i].BaseName) {
      for (size_t j = 0; j < NKERNELS; ++j) {
        if (!strcmp(Kernels[j].Name, Kernels[i].BaseName)) {
          Kernels[i].Base = (int)j;
        }
      }
      if (Kernels[i].Base < 0) {
        fprintf(stderr, "internal error: no base kernel '%s'\n", Kernels[i].BaseName);
        return 1;
      }
    }
  }
  if (List) {
    return 0;
  }

  // Three 64K buffers, each in its own 16K-aligned mapping; C holds a word that points to
  // itself (pointer-chain kernels) and the stand-in for the interrupt fault page.
  const size_t Size = 0x10000;
  uint8_t* M = mmap(NULL, Size * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (M == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  BufA = M;
  BufB = M + Size;
  BufC = M + 2 * Size;
  memset(M, 0x5A, Size * 3);
  *(uint64_t*)BufC = (uint64_t)(uintptr_t)BufC;

  printf("OrderingCostProbe: arm64 building blocks of x86 memory-ordering emulation\n");
  PrintSysctl("machdep.cpu.brand_string");
  PrintSysctl("hw.perflevel0.name");
  PrintSysctl("hw.perflevel0.logicalcpu");
  PrintSysctl("hw.perflevel1.name");
  PrintSysctl("hw.perflevel1.logicalcpu");
  PrintSysctl("hw.optional.arm.FEAT_LRCPC");
  PrintSysctl("hw.optional.arm.FEAT_LRCPC2");
  PrintSysctl("hw.optional.arm.FEAT_LRCPC3");
  PrintSysctl("hw.optional.arm.FEAT_LSE2");
  printf("reps %d, about %.2f ms per sample; ns = wall time per operation, cyc = ns x the sample's calibration GHz\n", Reps,
         SampleMs);
  PrintLoad("at start");

  QosRun Runs[2];
  int NRuns = 0;
  static const struct {
    const char* Name;
    qos_class_t Qos;
    int DarwinBG;
  } Classes[] = {{"interactive", QOS_CLASS_USER_INTERACTIVE, 0},
                 {"initiated", QOS_CLASS_USER_INITIATED, 0},
                 {"default", QOS_CLASS_DEFAULT, 0},
                 {"utility", QOS_CLASS_UTILITY, 0},
                 {"background", QOS_CLASS_BACKGROUND, 0},
                 {"darwinbg", QOS_CLASS_BACKGROUND, 1}};
  if (Map) {
    for (size_t c = 0; c < sizeof(Classes) / sizeof(Classes[0]); ++c) {
      QosRun Q = {Classes[c].Qos, Classes[c].Name, Classes[c].DarwinBG};
      RunOn(CpuMap, &Q);
    }
    PrintLoad("at end");
    return 0;
  }
  if (!strcmp(QosName, "both")) {
    Runs[NRuns++] = (QosRun) {QOS_CLASS_USER_INTERACTIVE, "interactive", 0};
    Runs[NRuns++] = (QosRun) {QOS_CLASS_BACKGROUND, "background", 0};
  } else {
    for (size_t c = 0; c < sizeof(Classes) / sizeof(Classes[0]); ++c) {
      if (!strcmp(QosName, Classes[c].Name)) {
        Runs[NRuns++] = (QosRun) {Classes[c].Qos, Classes[c].Name, Classes[c].DarwinBG};
      }
    }
    if (!NRuns) {
      fprintf(stderr, "unknown QoS class '%s'\n", QosName);
      return 2;
    }
  }
  for (int r = 0; r < NRuns; ++r) {
    RunOn(RunAll, &Runs[r]);
  }
  PrintLoad("at end");
  return 0;
}
