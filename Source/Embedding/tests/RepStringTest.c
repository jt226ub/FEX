/* SPDX-License-Identifier: MIT */
/*
 * rep movs / rep stos against exact x86 semantics (round 2, task E2).
 *
 * rep-string-diff-*: a data differential. Each case runs one `rep movs{b,w,d,q}` (with no
 * prefix, fs: or gs: on the source) or `rep stos{b,w,d,q}`, DF clear or set, on guest
 * memory filled with position-dependent bytes, and compares the memory around both
 * strings and rcx, rsi, rdi with a reference that performs the instruction element by
 * element, as x86 defines it. Overlap distances -80 to +80 bytes in both directions, every
 * element size, the destination at every offset within 8 bytes (so every relative skew of
 * source and destination), counts 0 to 300 and a few large ones across pages, and fs/gs
 * bases at every offset within 8 bytes. One subtest per translation of the instruction:
 * string ordering off (32-byte chunks), on (element loop), on with MEMCPYSETTSOWIDE (8-byte
 * units), and each of those with GUESTMEMORYALIASED (element loop for every copy).
 *
 * rep-movs-alias: the destination reaches the source's memory through a second mapping
 * (mach_vm_remap), 1 to 80 bytes ahead of it in memory but far away in addresses. With
 * GUESTMEMORYALIASED=1 every case must give x86's result in every translation; without
 * it the translation cannot see the overlap, and the wrong cases are only counted (that
 * is the documented limit, not a test).
 */
#include "TestSupport.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t Addr(const void* P) {
  return (uint64_t)(uintptr_t)P;
}

/* Translations of the instruction under test. */
struct Mode {
  const char* Name;
  const char* StringTSO;
  const char* Wide;
  const char* Aliased;
};

/* Code: one snippet per (operation, size, segment) at 16-byte strides, each the string
 * instruction then hlt. */
enum { OpMovs = 0, OpStos = 1 };
enum { SegNone = 0, SegFS = 1, SegGS = 2 };
static size_t SnippetIndex(int Op, int Size, int Seg) {
  const int SizeIndex = Size == 1 ? 0 : Size == 2 ? 1 : Size == 4 ? 2 : 3;
  return (size_t)((Op * 3 + Seg) * 4 + SizeIndex);
}
static void WriteSnippets(uint8_t* Code) {
  for (int Op = 0; Op < 2; ++Op) {
    for (int Seg = 0; Seg < 3; ++Seg) {
      for (int Size = 1; Size <= 8; Size *= 2) {
        uint8_t* P = Code + 16 * SnippetIndex(Op, Size, Seg);
        size_t N = 0;
        if (Seg == SegFS) {
          P[N++] = 0x64;
        } else if (Seg == SegGS) {
          P[N++] = 0x65;
        }
        P[N++] = 0xF3;
        if (Size == 2) {
          P[N++] = 0x66;
        } else if (Size == 8) {
          P[N++] = 0x48;
        }
        P[N++] = Op == OpMovs ? (Size == 1 ? 0xA4 : 0xA5) : (Size == 1 ? 0xAA : 0xAB);
        P[N++] = 0xF4;
      }
    }
  }
}

/* Guest memory: Area bytes, pattern-filled; Master keeps the pattern, Expect the
 * reference result. */
#define AREA (1u << 20)
static uint8_t* Area;
static uint8_t* Master;
static uint8_t* Expect;

static uint8_t Pattern(uint64_t I) {
  return (uint8_t)(I * 131 + 7 + (I >> 8) * 17 + (I >> 16) * 29);
}

struct Case {
  int Op, Size, Seg, Back;
  uint64_t Count;
  int64_t Dst;      /* offset in the area of the first element processed (rdi) */
  int64_t Src;      /* offset of the first source element (base + rsi) */
  int64_t SegBase;  /* offset of the segment base in the area (fs:/gs: only) */
  uint64_t Value;   /* rax for stos */
};

struct Totals {
  uint64_t Cases, Wrong;
  int Printed;
};

/* The x86 definition, element by element, on Expect (a copy of Master's span). */
static void Reference(const struct Case* C, uint8_t* Mem) {
  int64_t D = C->Dst, S = C->Src;
  const int64_t Step = C->Back ? -C->Size : C->Size;
  for (uint64_t i = 0; i < C->Count; ++i) {
    if (C->Op == OpMovs) {
      memmove(Mem + D, Mem + S, (size_t)C->Size);
      S += Step;
    } else {
      memcpy(Mem + D, &C->Value, (size_t)C->Size);
    }
    D += Step;
  }
}

static void Span(const struct Case* C, int64_t* Lo, int64_t* Hi) {
  const int64_t Bytes = (int64_t)C->Count * C->Size;
  int64_t L = C->Back ? C->Dst + C->Size - Bytes : C->Dst;
  int64_t H = L + Bytes;
  if (C->Op == OpMovs) {
    const int64_t SL = C->Back ? C->Src + C->Size - Bytes : C->Src;
    L = SL < L ? SL : L;
    H = SL + Bytes > H ? SL + Bytes : H;
  }
  *Lo = L - 128 < 0 ? 0 : L - 128;
  *Hi = H + 128 > (int64_t)AREA ? (int64_t)AREA : H + 128;
}

static void RunCase(fexmac_thread* T, uint8_t* Code, const struct Case* C, struct Totals* Tot, const char* ModeName) {
  int64_t Lo, Hi;
  Span(C, &Lo, &Hi);
  memcpy(Area + Lo, Master + Lo, (size_t)(Hi - Lo));
  memcpy(Expect + Lo, Master + Lo, (size_t)(Hi - Lo));
  Reference(C, Expect);

  fexmac_x86_state St = GetState(T);
  St.rip = Addr(Code + 16 * SnippetIndex(C->Op, C->Size, C->Seg));
  St.gpr[RCX] = C->Count;
  St.gpr[RDI] = Addr(Area) + (uint64_t)C->Dst;
  St.gpr[RAX] = C->Value;
  const uint64_t Base = C->Seg == SegNone ? 0 : Addr(Area) + (uint64_t)C->SegBase;
  St.fs_base = C->Seg == SegFS ? Base : 0;
  St.gs_base = C->Seg == SegGS ? Base : 0;
  St.gpr[RSI] = C->Op == OpMovs ? Addr(Area) + (uint64_t)C->Src - Base : 0x1234;
  St.rflags = C->Back ? 0x602 : 0x202;
  SetState(T, &St);
  const fexmac_stop Stop = Run(T);
  St = GetState(T);

  const int64_t Moved = (int64_t)C->Count * (C->Back ? -C->Size : C->Size);
  const uint64_t WantRdi = Addr(Area) + (uint64_t)(C->Dst + Moved);
  const uint64_t WantRsi = C->Op == OpMovs ? Addr(Area) + (uint64_t)(C->Src + Moved) - Base : 0x1234;
  int Bad = Stop.reason != FEXMAC_STOP_HALT || St.gpr[RCX] != 0 || St.gpr[RDI] != WantRdi || St.gpr[RSI] != WantRsi;
  int64_t First = -1;
  if (memcmp(Area + Lo, Expect + Lo, (size_t)(Hi - Lo)) != 0) {
    Bad = 1;
    for (int64_t i = Lo; i < Hi; ++i) {
      if (Area[i] != Expect[i]) {
        First = i;
        break;
      }
    }
  }
  ++Tot->Cases;
  if (Bad) {
    ++Tot->Wrong;
    if (Tot->Printed < 12) {
      ++Tot->Printed;
      printf("  WRONG [%s] %s%d %s seg %d count %llu dst %+lld src %+lld base %+lld: stop %d rcx %llu rdi %+lld (want %+lld) "
             "rsi ok %d, first byte wrong at %+lld\n",
             ModeName, C->Op == OpMovs ? "movs" : "stos", C->Size * 8, C->Back ? "DF=1" : "DF=0", C->Seg,
             (unsigned long long)C->Count, (long long)C->Dst, (long long)C->Src, (long long)C->SegBase, Stop.reason,
             (unsigned long long)St.gpr[RCX], (long long)(St.gpr[RDI] - Addr(Area)), (long long)(WantRdi - Addr(Area)),
             St.gpr[RSI] == WantRsi, (long long)First);
    }
  }
}

static fexmac_context* ModeContext(const struct Mode* M) {
  const fexmac_option O[] = {
    {"MULTIBLOCK", "1"},        {"TSOENABLED", "1"},         {"SMCCHECKS", "none"},
    {"MEMCPYSETTSOENABLED", M->StringTSO}, {"MEMCPYSETTSOWIDE", M->Wide}, {"GUESTMEMORYALIASED", M->Aliased},
  };
  return CreateContextWithOptions(O, 6, NULL, NULL);
}

static void Setup(void) {
  /* String ordering on takes alignment faults on misaligned elements (back-patched). */
  fexmac_install_fault_handlers();
  if (!Area) {
    Area = MapGuestMemory(AREA);
    Master = malloc(AREA);
    Expect = malloc(AREA);
    for (uint64_t i = 0; i < AREA; ++i) {
      Master[i] = Pattern(i);
    }
  }
}

static void Differential(const struct Mode* M) {
  Setup();
  fexmac_context* Ctx = ModeContext(M);
  uint8_t* Code = MapGuestMemory(0x4000);
  WriteSnippets(Code);
  fexmac_thread* T = CreateThreadAt(Ctx, Addr(Code), 0);
  struct Totals Tot = {0, 0, 0};
  static const uint64_t Counts[] = {0, 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 129, 300};
  const size_t NC = sizeof(Counts) / sizeof(Counts[0]);
  struct Totals Part[5] = {{0, 0, 0}};

  /* 1. Overlap distance -80..+80 (dst - src) x destination offset 0..7 x counts, every
   *    element size, both directions. */
  for (int Size = 1; Size <= 8; Size *= 2) {
    for (int Back = 0; Back < 2; ++Back) {
      for (int64_t d = -80; d <= 80; ++d) {
        for (int Skew = 0; Skew < 8; ++Skew) {
          for (size_t c = 0; c < NC; ++c) {
            struct Case C = {OpMovs, Size, SegNone, Back, Counts[c], (Back ? 0x40000 : 0x10000) + Skew, 0, 0, 0};
            C.Src = C.Dst - d;
            RunCase(T, Code, &C, &Part[0], M->Name);
          }
        }
      }
    }
  }
  /* 2. Every count 0..300, not overlapping (and far apart), destination and source skews. */
  for (int Size = 1; Size <= 8; Size *= 2) {
    for (int Back = 0; Back < 2; ++Back) {
      for (uint64_t Count = 0; Count <= 300; ++Count) {
        static const int64_t Dists[] = {-4099, -200, 200, 4101};
        for (size_t k = 0; k < 4; ++k) {
          for (int Skew = 0; Skew < 8; Skew += 5) {
            struct Case C = {OpMovs, Size, SegNone, Back, Count, (Back ? 0x60000 : 0x30000) + Skew, 0, 0, 0};
            C.Src = C.Dst - Dists[k];
            RunCase(T, Code, &C, &Part[1], M->Name);
          }
        }
      }
    }
  }
  /* 3. Large strings across 4K and 16K pages: far apart, and overlapping either way. */
  for (int Size = 1; Size <= 8; Size *= 2) {
    for (int Back = 0; Back < 2; ++Back) {
      static const uint64_t Bytes[] = {4095, 4096, 4097, 20000, 70001};
      static const int64_t Dists[] = {-(1 << 17) - 3, -9, 9, 65, -65, (1 << 17) + 6};
      for (size_t b = 0; b < 5; ++b) {
        for (size_t k = 0; k < 6; ++k) {
          for (int Skew = 0; Skew < 8; Skew += 3) {
            struct Case C = {OpMovs, Size, SegNone, Back, Bytes[b] / (uint64_t)Size, (Back ? 0xC0000 : 0x40000) + Skew, 0, 0, 0};
            C.Src = C.Dst - Dists[k];
            RunCase(T, Code, &C, &Part[2], M->Name);
          }
        }
      }
    }
  }
  /* 4. fs: and gs: sources, the base at every offset within 8 bytes (and far from the
   *    source), every source and destination skew, near and far distances. */
  for (int Seg = SegFS; Seg <= SegGS; ++Seg) {
    for (int Size = 1; Size <= 8; Size *= 2) {
      for (int Back = 0; Back < 2; ++Back) {
        for (int BaseMis = 0; BaseMis < 8; ++BaseMis) {
          for (int Skew = 0; Skew < 8; ++Skew) {
            static const int64_t Dists[] = {-4099, -40, -5, 3, 33, 70, 4101};
            static const uint64_t Bytes[] = {33, 100, 1000};
            for (size_t k = 0; k < 7; ++k) {
              for (size_t b = 0; b < 3; ++b) {
                struct Case C = {OpMovs, Size, Seg, Back, (Bytes[b] + (uint64_t)Size - 1) / (uint64_t)Size,
                                 (Back ? 0x80000 : 0x70000) + Skew, 0, 0x1000 + BaseMis, 0};
                C.Src = C.Dst - Dists[k];
                RunCase(T, Code, &C, &Part[3], M->Name);
              }
            }
          }
        }
      }
    }
  }
  /* 5. rep stos: every size, both directions, destination skews, counts 0..300 and large. */
  for (int Size = 1; Size <= 8; Size *= 2) {
    for (int Back = 0; Back < 2; ++Back) {
      for (int Skew = 0; Skew < 8; ++Skew) {
        for (uint64_t Count = 0; Count <= 300; Count += (Count < 70 ? 1 : 23)) {
          struct Case C = {OpStos, Size, SegNone, Back, Count, (Back ? 0x90000 : 0x88000) + Skew, 0, 0, 0x8877665544332211ULL};
          RunCase(T, Code, &C, &Part[4], M->Name);
        }
        static const uint64_t Bytes[] = {4095, 4097, 70001};
        for (size_t b = 0; b < 3; ++b) {
          struct Case C = {OpStos, Size, SegNone, Back, Bytes[b] / (uint64_t)Size, (Back ? 0xD0000 : 0xA0000) + Skew, 0, 0,
                           0x8877665544332211ULL};
          RunCase(T, Code, &C, &Part[4], M->Name);
        }
      }
    }
  }
  static const char* PartName[] = {"overlap distance -80..80 x skew x count", "every count 0..300", "large strings across pages",
                                   "fs:/gs: sources, every base offset", "rep stos"};
  for (int p = 0; p < 5; ++p) {
    Check(Part[p].Wrong == 0, "[%s] %s: %llu cases, %llu wrong", M->Name, PartName[p], (unsigned long long)Part[p].Cases,
          (unsigned long long)Part[p].Wrong);
    Tot.Cases += Part[p].Cases;
  }
  printf("  [%s] %llu cases in all\n", M->Name, (unsigned long long)Tot.Cases);
  fexmac_thread_destroy(T);
  fexmac_context_destroy(Ctx);
}

static const struct Mode ModeOff = {"string ordering off", "0", "0", "0"};
static const struct Mode ModeOn = {"string ordering on", "1", "0", "0"};
static const struct Mode ModeWide = {"string ordering on, MEMCPYSETTSOWIDE", "1", "1", "0"};
static const struct Mode ModeOffAliased = {"string ordering off, GUESTMEMORYALIASED", "0", "0", "1"};
static const struct Mode ModeWideAliased = {"MEMCPYSETTSOWIDE, GUESTMEMORYALIASED", "1", "1", "1"};

static void TestDiffOff(void) {
  Differential(&ModeOff);
}
static void TestDiffOn(void) {
  Differential(&ModeOn);
}
static void TestDiffWide(void) {
  Differential(&ModeWide);
}
static void TestDiffOffAliased(void) {
  Differential(&ModeOffAliased);
}
static void TestDiffWideAliased(void) {
  Differential(&ModeWideAliased);
}

/* ---- aliases ---- */

/* One rep movs whose destination is addressed through the second mapping Q of the
 * memory P: destination = source + Dist in memory. Returns 1 if the result is x86's. */
static int AliasCase(fexmac_thread* T, uint8_t* Code, uint8_t* P, uint8_t* Q, int Size, int Back, int64_t Dist, uint64_t Count) {
  enum { Len = 0x10000 };
  const int64_t Src = Back ? 0xC000 : 0x2000;
  const int64_t Dst = Src + Dist;
  for (int i = 0; i < Len; ++i) {
    P[i] = Pattern((uint64_t)i);
  }
  memcpy(Expect, P, Len);
  const struct Case C = {OpMovs, Size, SegNone, Back, Count, Dst, Src, 0, 0};
  Reference(&C, Expect);
  fexmac_x86_state St = GetState(T);
  St.rip = Addr(Code + 16 * SnippetIndex(OpMovs, Size, SegNone));
  St.gpr[RCX] = Count;
  St.gpr[RSI] = Addr(P) + (uint64_t)Src;
  St.gpr[RDI] = Addr(Q) + (uint64_t)Dst;
  St.fs_base = St.gs_base = 0;
  St.rflags = Back ? 0x602 : 0x202;
  SetState(T, &St);
  const fexmac_stop Stop = Run(T);
  St = GetState(T);
  return Stop.reason == FEXMAC_STOP_HALT && St.gpr[RCX] == 0 && memcmp(P, Expect, Len) == 0;
}

/* WrongAt[d] counts the wrong cases with the destination d bytes ahead of the source in
 * the copy's direction (1..80), WrongAt[0] those behind it. */
static void AliasRun(const struct Mode* M, uint8_t* P, uint8_t* Q, uint64_t* Cases, uint64_t* Wrong, uint64_t* WrongAt) {
  fexmac_context* Ctx = ModeContext(M);
  uint8_t* Code = MapGuestMemory(0x4000);
  WriteSnippets(Code);
  fexmac_thread* T = CreateThreadAt(Ctx, Addr(Code), 0);
  for (int Size = 1; Size <= 8; Size *= 2) {
    for (int Back = 0; Back < 2; ++Back) {
      for (int64_t d = 1; d <= 80; ++d) {
        static const uint64_t Bytes[] = {100, 700, 5000};
        for (size_t b = 0; b < 3; ++b) {
          /* Destination ahead of the source in the copy's direction (where x86 copies
           * propagate), and behind it. */
          for (int Sign = -1; Sign <= 1; Sign += 2) {
            const int64_t Dist = (Back ? -d : d) * Sign;
            ++*Cases;
            const int Bad = !AliasCase(T, Code, P, Q, Size, Back, Dist, Bytes[b] / (uint64_t)Size);
            *Wrong += Bad;
            WrongAt[Sign > 0 ? d : 0] += Bad;
          }
        }
      }
    }
  }
  fexmac_thread_destroy(T);
  fexmac_context_destroy(Ctx);
}

static void TestAlias(void) {
  Setup();
  mach_vm_address_t P = 0, Q = 0;
  vm_prot_t Cur, Max;
  Check(mach_vm_allocate(mach_task_self(), &P, 0x10000, VM_FLAGS_ANYWHERE) == KERN_SUCCESS, "mach_vm_allocate");
  Check(mach_vm_remap(mach_task_self(), &Q, 0x10000, 0, VM_FLAGS_ANYWHERE, mach_task_self(), P, FALSE, &Cur, &Max, VM_INHERIT_SHARE) ==
          KERN_SUCCESS,
        "mach_vm_remap: a second mapping of the same memory");
  ((volatile uint8_t*)P)[5] = 0x5A;
  Check(((volatile uint8_t*)Q)[5] == 0x5A && P != Q, "the second mapping shows the first one's bytes at another address");
  static const struct Mode Declared[] = {
    {"string ordering off, GUESTMEMORYALIASED", "0", "0", "1"},
    {"string ordering on, GUESTMEMORYALIASED", "1", "0", "1"},
    {"MEMCPYSETTSOWIDE, GUESTMEMORYALIASED", "1", "1", "1"},
  };
  for (size_t m = 0; m < 3; ++m) {
    uint64_t Cases = 0, Wrong = 0, WrongAt[81] = {0};
    AliasRun(&Declared[m], (uint8_t*)P, (uint8_t*)Q, &Cases, &Wrong, WrongAt);
    Check(Wrong == 0, "[%s] destination through an alias 1 to 80 bytes from the source: %llu cases, %llu wrong", Declared[m].Name,
          (unsigned long long)Cases, (unsigned long long)Wrong);
  }
  /* Not declared: the documented limit, counted only. */
  static const struct Mode Undeclared[] = {
    {"string ordering off", "0", "0", "0"},
    {"string ordering on", "1", "0", "0"},
    {"MEMCPYSETTSOWIDE", "1", "1", "0"},
  };
  for (size_t m = 0; m < 3; ++m) {
    uint64_t Cases = 0, Wrong = 0, WrongAt[81] = {0};
    AliasRun(&Undeclared[m], (uint8_t*)P, (uint8_t*)Q, &Cases, &Wrong, WrongAt);
    printf("  report only, not declared [%s]: %llu cases, %llu not x86's result; destination ahead by (bytes: wrong cases):",
           Undeclared[m].Name, (unsigned long long)Cases, (unsigned long long)Wrong);
    for (int d = 1; d <= 80; ++d) {
      if (WrongAt[d]) {
        printf(" %d:%llu", d, (unsigned long long)WrongAt[d]);
      }
    }
    printf("; behind: %llu\n", (unsigned long long)WrongAt[0]);
  }
  mach_vm_deallocate(mach_task_self(), P, 0x10000);
  mach_vm_deallocate(mach_task_self(), Q, 0x10000);
}

const struct Subtest RepSubtests[] = {
  {"rep-string-diff-off", TestDiffOff, 0, NULL, NULL, 0},
  {"rep-string-diff-on", TestDiffOn, 0, NULL, NULL, 0},
  {"rep-string-diff-wide", TestDiffWide, 0, NULL, NULL, 0},
  {"rep-string-diff-off-aliased", TestDiffOffAliased, 0, NULL, NULL, 0},
  {"rep-string-diff-wide-aliased", TestDiffWideAliased, 0, NULL, NULL, 0},
  {"rep-movs-alias", TestAlias, 0, NULL, NULL, 0},
};
const size_t RepSubtestCount = sizeof(RepSubtests) / sizeof(RepSubtests[0]);
