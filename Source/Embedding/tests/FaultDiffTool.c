/* SPDX-License-Identifier: MIT */
/*
 * fexmac_fault_diff: runs one guest program and prints the complete guest state (all
 * registers, x87/MMX, XMM and YMM upper halves, flags, MXCSR, FS/GS bases and a hash of
 * the data pages) at each stop, for Scripts/macos_fault_diff.py, which compares runs:
 * the state at a guest page fault against a reference run in which the faulting
 * instruction is replaced by hlt; the result after fixing the page and resuming against
 * a clean run; and the result of a write to translated code under SMCCHECKS mtrack
 * against SMCCHECKS full. Written from the package D review's differential harness.
 *
 * Usage: fexmac_fault_diff <code.bin> <multiblock 0|1> <tso 0|1> <prot of the test page>
 *        [resume]
 * Environment: SMC (full, mtrack, none; default full), WARM=1 (translate code on the
 * test page's host page first, so that writes to it reach the write-protection path),
 * NOHASHPAGE=1 (leave the test page out of the first stop's memory hash, so that a run
 * where it is inaccessible compares with one where it is not).
 *
 * Layout (offsets from a 1MB mapping): code at 0, data 0x10000-0x13FFF (RW), the test
 * page 0x14000-0x17FFF (its protection given), data 0x18000-0x2FFFF (RW); rsp
 * 0x2F000, rdi 0x10100, rsi 0x10200, r15 the test page, r14 the test page - 4, rcx 16.
 */
#include "fexmac.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static uint8_t* Base;
static fexmac_thread* Thread;
static int TestPageReadable;

static void Dump(const char* Tag, const fexmac_stop* Stop) {
  fexmac_x86_state S;
  fexmac_x86_state_init(&S);
  fexmac_thread_get_state(Thread, &S);
  const uint64_t B = (uint64_t)(uintptr_t)Base;
  printf("%s.reason=%d\n%s.trap=%u\n%s.ia=+%" PRIx64 "\n%s.faddr=+%" PRIx64 "\n%s.access=%u\n%s.err=%x\n%s.sig=%d\n%s.code=%d\n", Tag,
         Stop->reason, Tag, Stop->trap_number, Tag, Stop->instruction_address - B, Tag, Stop->fault_address - B, Tag, Stop->access, Tag,
         Stop->error_code, Tag, Stop->signal, Tag, Stop->si_code);
  printf("%s.rip=+%" PRIx64 "\n", Tag, S.rip - B);
  for (int i = 0; i < 16; i++) {
    const uint64_t V = S.gpr[i];
    if (V >= B && V < B + 0x100000) {
      printf("%s.gpr%d=B+%" PRIx64 "\n", Tag, i, V - B);
    } else {
      printf("%s.gpr%d=%" PRIx64 "\n", Tag, i, V);
    }
  }
  printf("%s.rflags=%" PRIx64 "\n", Tag, S.rflags);
  printf("%s.mxcsr=%x\n%s.fcw=%x\n%s.fsw=%x\n%s.ftw=%x\n", Tag, S.mxcsr, Tag, S.fcw, Tag, S.fsw, Tag, S.ftw);
  printf("%s.fsbase=%" PRIx64 "\n%s.gsbase=%" PRIx64 "\n", Tag, S.fs_base, Tag, S.gs_base);
  for (int i = 0; i < 8; i++) {
    printf("%s.mm%d=%" PRIx64 ":%" PRIx64 "\n", Tag, i, S.mm[i][1] & 0xffff, S.mm[i][0]);
  }
  for (int i = 0; i < 16; i++) {
    printf("%s.xmm%d=%" PRIx64 ":%" PRIx64 "\n", Tag, i, S.xmm[i][1], S.xmm[i][0]);
  }
  for (int i = 0; i < 16; i++) {
    printf("%s.ymmh%d=%" PRIx64 ":%" PRIx64 "\n", Tag, i, S.ymm_high[i][1], S.ymm_high[i][0]);
  }
  /* Hashed per aligned 8-byte word, with addresses inside the mapping made relative to
   * it, so that a stored address (a return address, a pushed rsp) hashes alike in two
   * runs that got the mapping at different addresses. */
  uint64_t H = 1469598103934665603ULL;
  for (int i = 0x10000; i < 0x30000; i += 8) {
    if (i >= 0x14000 && i < 0x18000 && !TestPageReadable) {
      continue;
    }
    if (i >= 0x17F00 && i < 0x18000) {
      continue; /* the warm-up stub */
    }
    uint64_t V;
    memcpy(&V, Base + i, sizeof(V));
    if (V >= B && V < B + 0x100000) {
      V -= B;
    }
    H ^= V;
    H *= 1099511628211ULL;
  }
  printf("%s.memhash=%" PRIx64 "\n", Tag, H);
}

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: fexmac_fault_diff <code.bin> <multiblock> <tso> <prot> [resume]\n");
    return 2;
  }
  FILE* F = fopen(argv[1], "rb");
  if (!F) {
    return 2;
  }
  static uint8_t Code[65536];
  const size_t N = fread(Code, 1, sizeof(Code), F);
  fclose(F);
  const int Prot = atoi(argv[4]);
  const int Resume = argc > 5 && !strcmp(argv[5], "resume");
  const char* SMC = getenv("SMC") ? getenv("SMC") : "full";
  const int Warm = getenv("WARM") && !strcmp(getenv("WARM"), "1");
  /* FAULTDIFF_OPTIONS="NAME=VALUE,NAME=VALUE": further options from the harness's
   * --option (for example the string-ordering settings). */
  fexmac_option Options[3 + 16] = {{"MULTIBLOCK", argv[2]}, {"TSOENABLED", argv[3]}, {"SMCCHECKS", SMC}};
  size_t OptionCount = 3;
  static char Extra[1024];
  if (getenv("FAULTDIFF_OPTIONS")) {
    snprintf(Extra, sizeof(Extra), "%s", getenv("FAULTDIFF_OPTIONS"));
    for (char *Save = NULL, *Item = strtok_r(Extra, ",", &Save); Item; Item = strtok_r(NULL, ",", &Save)) {
      char* Eq = strchr(Item, '=');
      if (!Eq || OptionCount == sizeof(Options) / sizeof(Options[0])) {
        fprintf(stderr, "bad FAULTDIFF_OPTIONS item: %s\n", Item);
        return 2;
      }
      *Eq = 0;
      Options[OptionCount].name = Item;
      Options[OptionCount].value = Eq + 1;
      ++OptionCount;
    }
  }
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = Options;
  Config.option_count = OptionCount;
  fexmac_context* Context;
  if (fexmac_context_create(&Config, &Context) != FEXMAC_OK) {
    printf("context failed\n");
    return 2;
  }
  fexmac_install_fault_handlers();
  /* The same address in every run, so that memory holding addresses (return addresses,
   * pushed pointers, also stored misaligned) compares alike between runs. */
  Base = mmap((void*)(uintptr_t)0x500000000000ULL, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (Base == MAP_FAILED) {
    printf("mapping failed\n");
    return 2;
  }
  memcpy(Base, Code, N);
  for (int i = 0x10000; i < 0x30000; i++) {
    Base[i] = (uint8_t)(i * 7 + 3);
  }
  const uint64_t B = (uint64_t)(uintptr_t)Base;
  fexmac_memory_map(Context, B, 0x10000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE | FEXMAC_PROT_EXEC);
  fexmac_memory_map(Context, B + 0x10000, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE);
  fexmac_memory_map(Context, B + 0x14000, 0x4000, (uint32_t)Prot);
  fexmac_memory_map(Context, B + 0x18000, 0x18000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE);
  fexmac_thread_create(Context, &Thread);
  if (Warm) {
    /* Translate a ret at the end of the test page's host page (mov eax, 0; ret is not
     * needed: a lone hlt), so it holds translated code. */
    Base[0x17F00] = 0xF4;
    fexmac_x86_state W;
    fexmac_x86_state_init(&W);
    fexmac_thread_get_state(Thread, &W);
    W.rip = B + 0x17F00;
    fexmac_thread_set_state(Thread, &W);
    fexmac_stop Stop;
    fexmac_stop_init(&Stop);
    fexmac_thread_run(Thread, &Stop);
  }
  fexmac_x86_state S;
  fexmac_x86_state_init(&S);
  fexmac_thread_get_state(Thread, &S);
  for (int i = 0; i < 16; i++) {
    S.gpr[i] = 0x0101010101010101ULL * (uint64_t)(i + 1);
  }
  S.gpr[4] = B + 0x2F000;
  S.gpr[7] = B + 0x10100;
  S.gpr[6] = B + 0x10200;
  S.gpr[15] = B + 0x14000;
  S.gpr[14] = B + 0x13FFC;
  S.gpr[1] = 16;
  S.rip = B;
  S.rflags = 0x202;
  for (int i = 0; i < 16; i++) {
    S.xmm[i][0] = 0x1111111111111111ULL * (uint64_t)(i + 1);
    S.xmm[i][1] = ~S.xmm[i][0];
    S.ymm_high[i][0] = 0xABCD000000000000ULL + (uint64_t)i;
    S.ymm_high[i][1] = 0x1234 + (uint64_t)i;
  }
  fexmac_thread_set_state(Thread, &S);
  TestPageReadable = Prot != 0 && !(getenv("NOHASHPAGE") && !strcmp(getenv("NOHASHPAGE"), "1"));
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  fexmac_thread_run(Thread, &Stop);
  Dump("a", &Stop);
  if (Resume) {
    if (getenv("ROUNDTRIP") && !strcmp(getenv("ROUNDTRIP"), "1")) {
      /* What an embedder does to deliver a guest signal and return from it: read the
       * state at the stop and write the same values back before running again. */
      fexmac_x86_state R;
      fexmac_x86_state_init(&R);
      fexmac_thread_get_state(Thread, &R);
      fexmac_thread_set_state(Thread, &R);
    }
    fexmac_memory_protect(Context, B + 0x14000, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE);
    TestPageReadable = 1;
    fexmac_stop_init(&Stop);
    fexmac_thread_run(Thread, &Stop);
    Dump("b", &Stop);
  }
  printf("codewrites=%" PRIu64 "\n", fexmac_memory_code_write_count(Context));
  return 0;
}
