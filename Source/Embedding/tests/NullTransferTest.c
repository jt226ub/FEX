/* SPDX-License-Identifier: MIT */
/*
 * Guest control transfers to address 0 and to other addresses with no code (round 2
 * review, item 1). Before the fix a jump, call or return to 0 killed the host process:
 * an empty L1 lookup entry and an empty call/return stack entry are all zeros, so rip 0
 * "matched" them and translated code branched to host address 0; an invalidated L1
 * entry kept its host code, so a jump to 0 after invalidating a block in L1 slot 0 ran
 * the old translation. Every case here must stop with FEXMAC_STOP_GUEST_FAULT, a page
 * fault (trap 14) fetching at the target, rip = the target and rsp as x86 leaves it.
 *
 * Matrix: each transfer kind x {fresh thread, after other code, after invalidating a
 * block in L1 slot 0 (stale)} x multiblock on/off x {ordering on, TSODEFERRED before the
 * switch, after it} x {no guest memory registered, registered}. One fresh context per
 * case.
 */
#include "TestSupport.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { R8 = 8 };

/* 2MB of guest memory aligned to 1MB, so its start is in L1 slot 0 for every L1 size. */
static uint8_t* AlignedMemory(size_t Size, size_t Align, uint8_t** Raw, size_t* RawSize) {
  *RawSize = Size + Align;
  *Raw = MapGuestMemory(*RawSize);
  uintptr_t Start = ((uintptr_t)*Raw + Align - 1) & ~(uintptr_t)(Align - 1);
  return (uint8_t*)Start;
}

struct Transfer {
  const char* Name;
  uint8_t Code[24];
  size_t Size;
  uint64_t Target;     /* 0 or 0x10000 */
  int64_t RspDelta;    /* rsp at the fault minus rsp at the start */
  int PushedReturn;    /* [rsp] at the fault is the address after the instruction at this offset + 1, or -1 */
  int Kind;            /* 0 plain, 1 rip set before running, 2 rip set by the system call callback, 3 iretq frame */
};

/* clang-format off */
static const struct Transfer Transfers[] = {
  {"xor eax,eax; jmp rax", {0x31, 0xC0, 0xFF, 0xE0}, 4, 0, 0, -1, 0},
  {"xor eax,eax; call rax", {0x31, 0xC0, 0xFF, 0xD0}, 4, 0, -8, 4, 0},
  {"mov rax,[rbx]; jmp rax", {0x48, 0x8B, 0x03, 0xFF, 0xE0}, 5, 0, 0, -1, 0},
  {"push 0; ret", {0x6A, 0x00, 0xC3}, 3, 0, 0, -1, 0},
  {"call n; n: pop rcx; push 0; ret", {0xE8, 0x00, 0x00, 0x00, 0x00, 0x59, 0x6A, 0x00, 0xC3}, 9, 0, 0, -1, 0},
  {"jmp qword [rbx]", {0xFF, 0x23}, 2, 0, 0, -1, 0},
  {"call qword [rbx]", {0xFF, 0x13}, 2, 0, -8, 2, 0},
  {"push 0; ret 8", {0x6A, 0x00, 0xC2, 0x08, 0x00}, 5, 0, 8, -1, 0},
  {"iretq to rip 0", {0x48, 0xCF}, 2, 0, 40, -1, 3},
  {"rip 0 set before running", {0xF4}, 1, 0, 0, -1, 1},
  {"rip 0 set by the system call callback", {0xB8, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x05}, 7, 0, 0, -1, 2},
  {"control: mov eax,0x10000; jmp rax", {0xB8, 0x00, 0x00, 0x01, 0x00, 0xFF, 0xE0}, 7, 0x10000, 0, -1, 0},
  {"control: push 0x10000; ret", {0x68, 0x00, 0x00, 0x01, 0x00, 0xC3}, 6, 0x10000, 0, -1, 0},
};
/* clang-format on */

static fexmac_syscall_action RipZeroCallback(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  fexmac_x86_state S = GetState(Thread);
  S.rip = 0;
  SetState(Thread, &S);
  return FEXMAC_SYSCALL_RESUME;
}

static int RunTransfer(const struct Transfer* T, int Prior, int Multiblock, int Ordering, int Register, char* Why, size_t WhySize) {
  uint8_t* Raw;
  size_t RawSize;
  uint8_t* Mem = AlignedMemory(0x200000, 0x100000, &Raw, &RawSize);
  memset(Mem, 0, 0x200000);
  const uint64_t Base = (uint64_t)(uintptr_t)Mem;
  const uint64_t Code = Base + 0x10040;
  const uint64_t Stack = Base + 0x1F0000;
  const uint64_t Data = Base + 0x30000;

  fexmac_option Options[3] = {{"MULTIBLOCK", Multiblock ? "1" : "0"}, {"TSOENABLED", "1"}, {"TSODEFERRED", "1"}};
  fexmac_context* Context = CreateContextWithOptions(Options, Ordering ? 3 : 2, T->Kind == 2 ? RipZeroCallback : NULL, NULL);
  if (Register) {
    CheckResult(fexmac_memory_map(Context, Base, 0x200000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE | FEXMAC_PROT_EXEC), FEXMAC_OK, "register");
  }
  fexmac_thread* Thread = CreateThreadAt(Context, Code, Stack);
  fexmac_thread* Other = NULL;
  if (Ordering == 2) {
    Other = CreateThreadAt(Context, Code, Stack); /* the second live thread: the switch */
  }

  if (Prior) {
    /* A block at Base (L1 slot 0) and calls into it, so lookups and the call/return
     * stack hold entries: mov ecx, 0x1234; hlt. */
    const uint8_t Marker[] = {0xB9, 0x34, 0x12, 0x00, 0x00, 0xF4};
    memcpy(Mem, Marker, sizeof(Marker));
    fexmac_x86_state S = GetState(Thread);
    S.rip = Base;
    SetState(Thread, &S);
    if (Run(Thread).reason != FEXMAC_STOP_HALT) {
      snprintf(Why, WhySize, "setup block did not halt");
      return 0;
    }
    if (Prior == 2) {
      /* The block's bytes change and it is invalidated: its L1 entry must not survive. */
      if (Register) {
        fexmac_memory_write_begin(Context, Base, 0x4000);
      }
      memset(Mem, 0x90, 16);
      if (Register) {
        fexmac_memory_write_end(Context, Base, 0x4000);
      }
      fexmac_invalidate(Context, Base, 0x1000);
    }
  }

  memcpy((void*)(uintptr_t)Code, T->Code, T->Size);
  fexmac_x86_state S = GetState(Thread);
  S.rip = T->Kind == 1 ? 0 : Code;
  S.gpr[RSP] = Stack;
  S.gpr[RCX] = 0;
  S.gpr[RBX] = Data;
  uint64_t Expected = Stack + (uint64_t)T->RspDelta;
  if (T->Kind == 3) {
    uint64_t* Frame = (uint64_t*)(uintptr_t)Stack;
    Frame[0] = 0;          /* rip */
    Frame[1] = S.cs;       /* cs */
    Frame[2] = 0x202;      /* rflags */
    Frame[3] = Stack + 40; /* rsp */
    Frame[4] = S.ss;       /* ss */
  }
  SetState(Thread, &S);
  const fexmac_stop Stop = Run(Thread);
  const fexmac_x86_state After = GetState(Thread);
  int Ok = Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.trap_number == 14 && Stop.instruction_address == T->Target &&
           Stop.fault_address == T->Target && After.rip == T->Target && After.gpr[RSP] == Expected && After.gpr[RCX] != 0x1234;
  if (Ok && T->PushedReturn >= 0) {
    Ok = *(uint64_t*)(uintptr_t)After.gpr[RSP] == Code + (uint64_t)T->PushedReturn;
  }
  if (!Ok) {
    snprintf(Why, WhySize, "stop %d trap %u at 0x%" PRIx64 " fault 0x%" PRIx64 " rip 0x%" PRIx64 " rsp %+" PRId64 " rcx 0x%" PRIx64, Stop.reason,
             Stop.trap_number, Stop.instruction_address, Stop.fault_address, After.rip, (int64_t)(After.gpr[RSP] - Stack), After.gpr[RCX]);
  }
  if (Other) {
    fexmac_thread_destroy(Other);
  }
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
  munmap(Raw, RawSize);
  return Ok;
}

static void RunMatrix(int Register) {
  CheckResult(fexmac_install_fault_handlers(), FEXMAC_OK, "install fault handlers");
  static const char* PriorNames[] = {"fresh", "after other code", "after invalidating slot 0"};
  static const char* OrderNames[] = {"ordering on", "deferred", "after the switch"};
  int Cases = 0, Bad = 0;
  /* NULL_TRANSFER_ONLY=n runs only transfer n (diagnosis). */
  const char* Only = getenv("NULL_TRANSFER_ONLY");
  for (size_t t = 0; t < sizeof(Transfers) / sizeof(Transfers[0]); ++t) {
    if (Only && (size_t)atoi(Only) != t) {
      continue;
    }
    for (int Prior = 0; Prior < 3; ++Prior) {
      for (int Mb = 0; Mb < 2; ++Mb) {
        for (int Ord = 0; Ord < 3; ++Ord) {
          char Why[200] = "";
          ++Cases;
          if (!RunTransfer(&Transfers[t], Prior, Mb, Ord, Register, Why, sizeof(Why))) {
            ++Bad;
            printf("    FAIL %s, %s, multiblock %d, %s: %s\n", Transfers[t].Name, PriorNames[Prior], Mb, OrderNames[Ord], Why);
          }
        }
      }
    }
  }
  Check(Bad == 0, "%d transfers to 0 or to no code (%s): every one a fetch page fault at the target with exact rip and rsp, never stale code",
        Cases, Register ? "guest memory registered" : "nothing registered");
}

static void TestNullTransfersUnregistered(void) {
  RunMatrix(0);
}
static void TestNullTransfersRegistered(void) {
  RunMatrix(1);
}

const struct Subtest NullTransferSubtests[] = {
  {"transfer-to-zero", TestNullTransfersUnregistered, 0, NULL, NULL, 0},
  {"transfer-to-zero-registered", TestNullTransfersRegistered, 0, NULL, NULL, 0},
};
const size_t NullTransferSubtestCount = sizeof(NullTransferSubtests) / sizeof(NullTransferSubtests[0]);
