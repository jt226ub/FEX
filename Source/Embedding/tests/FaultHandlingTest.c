/* SPDX-License-Identifier: MIT */
/*
 * fexmac_c_test subtests for guest fault delivery, stopping a thread from another thread
 * and self-modifying code detected by write protection. Public header only.
 *
 * The guest programs were assembled from the Intel-syntax source in each comment with
 * `clang -target x86_64-linux-gnu -c` and copied byte for byte.
 */
#include "TestSupport.h"

#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* clang-format off */
static const uint8_t MidBlockFault[] = {
  0x48, 0xC7, 0xC0, 0x11, 0x11, 0x00, 0x00, /*   0: mov rax, 0x1111 */
  0x48, 0x83, 0xC3, 0x05,                  /*   7: add rbx, 0x5 */
  0x48, 0x8D, 0x4C, 0x18, 0x07,            /*  11: lea rcx, [rax + rbx + 0x7] */
  0x66, 0x48, 0x0F, 0x6E, 0xC9,            /*  16: movq xmm1, rcx */
  0x66, 0x0F, 0xD4, 0xC9,                  /*  21: paddq xmm1, xmm1 */
  0x48, 0x8B, 0x17,                        /*  25: mov rdx, qword ptr [rdi] */
  0x48, 0x83, 0xC2, 0x01,                  /*  28: add rdx, 0x1 */
  0x49, 0x83, 0xE8, 0x03,                  /*  32: sub r8, 0x3 */
  0x48, 0x89, 0xD6,                        /*  36: mov rsi, rdx */
  0xF4,                                    /*  39: hlt */
};
/* clang-format on */
#define MIDBLOCKFAULT_LOAD 25

/* clang-format off */
static const uint8_t MidBlockFaultLive[] = {
  0x48, 0xC7, 0xC0, 0x11, 0x11, 0x00, 0x00, /*   0: mov rax, 0x1111 */
  0x48, 0x83, 0xC3, 0x05,                  /*   7: add rbx, 0x5 */
  0x48, 0x8D, 0x4C, 0x18, 0x07,            /*  11: lea rcx, [rax + rbx + 0x7] */
  0x48, 0x8B, 0x17,                        /*  16: mov rdx, qword ptr [rdi] */
  0x9F,                                    /*  19: lahf */
  0xF4,                                    /*  20: hlt */
};
/* clang-format on */
#define MIDBLOCKFAULTLIVE_LOAD 16

/* clang-format off */
static const uint8_t StoreLoop[] = {
  0x31, 0xC9,                              /*   0: xor ecx, ecx */
  0x48, 0x89, 0x0C, 0xCF,                  /*   2: mov qword ptr [rdi + 8*rcx], rcx */
  0x48, 0xFF, 0xC1,                        /*   6: inc rcx */
  0x48, 0x81, 0xF9, 0x00, 0x10, 0x00, 0x00, /*   9: cmp rcx, 0x1000 */
  0x75, 0xF0,                              /*  16: jne 0x2 <loop> */
  0xF4,                                    /*  18: hlt */
};
/* clang-format on */
#define STORELOOP_LOOP 2

/* clang-format off */
static const uint8_t RepLods[] = {
  0xF3, 0xAC,                              /*   0: rep lodsb al, byte ptr [rsi] */
  0xF4,                                    /*   2: hlt */
};
/* clang-format on */

/* clang-format off */
static const uint8_t RepStos[] = {
  0xF3, 0xAA,                              /*   0: rep stosb byte ptr es:[rdi], al */
  0xF4,                                    /*   2: hlt */
};
/* clang-format on */

/* clang-format off */
static const uint8_t VectorFault[] = {
  0x66, 0x0F, 0xFE, 0xD3,                  /*   0: paddd xmm2, xmm3 */
  0xF3, 0x0F, 0x6F, 0x07,                  /*   4: movdqu xmm0, xmmword ptr [rdi] */
  0x66, 0x0F, 0x6F, 0xE0,                  /*   8: movdqa xmm4, xmm0 */
  0xF4,                                    /*  12: hlt */
};
/* clang-format on */
#define VECTORFAULT_LOAD 4

/* clang-format off */
static const uint8_t PushFault[] = {
  0x48, 0xC7, 0xC0, 0x77, 0x00, 0x00, 0x00, /*   0: mov rax, 0x77 */
  0x50,                                    /*   7: push rax */
  0xF4,                                    /*   8: hlt */
};
/* clang-format on */
#define PUSHFAULT_PUSH 7

/* clang-format off */
static const uint8_t CallChainFault[] = {
  0xE8, 0x08, 0x00, 0x00, 0x00,            /*   0: call 0xd <f> */
  0x49, 0xC7, 0xC1, 0x99, 0x00, 0x00, 0x00, /*   5: mov r9, 0x99 */
  0xF4,                                    /*  12: hlt */
  0xE8, 0x05, 0x00, 0x00, 0x00,            /*  13: call 0x17 <g> */
  0x48, 0x83, 0xC0, 0x01,                  /*  18: add rax, 0x1 */
  0xC3,                                    /*  22: ret */
  0x48, 0x8B, 0x07,                        /*  23: mov rax, qword ptr [rdi] */
  0xC3,                                    /*  26: ret */
};
/* clang-format on */
#define CALLCHAINFAULT_F 13
#define CALLCHAINFAULT_G 23

/* clang-format off */
static const uint8_t JumpTo[] = {
  0xFF, 0xE0,                              /*   0: jmp rax */
};
/* clang-format on */

/* clang-format off */
static const uint8_t Handler[] = {
  0x49, 0xC7, 0xC0, 0x5A, 0x00, 0x00, 0x00, /*   0: mov r8, 0x5a */
  0xF4,                                    /*   7: hlt */
};
/* clang-format on */

/* clang-format off */
static const uint8_t CountLoopJmp[] = {
  0x48, 0xFF, 0xC0,                        /*   0: inc rax */
  0x48, 0x83, 0xC3, 0x02,                  /*   3: add rbx, 0x2 */
  0xEB, 0xF7,                              /*   7: jmp 0x0 <loop> */
};
/* clang-format on */
#define COUNTLOOPJMP_LOOP 0

/* clang-format off */
static const uint8_t CountLoopJcc[] = {
  0x48, 0xFF, 0xC0,                        /*   0: inc rax */
  0x48, 0x83, 0xC3, 0x02,                  /*   3: add rbx, 0x2 */
  0x48, 0xFF, 0xC9,                        /*   7: dec rcx */
  0x75, 0xF4,                              /*  10: jne 0x0 <loop> */
  0xF4,                                    /*  12: hlt */
};
/* clang-format on */
#define COUNTLOOPJCC_LOOP 0

/* clang-format off */
static const uint8_t RepCmps[] = {
  0xF3, 0xA6,                              /*   0: rep cmpsb byte ptr [rsi], byte ptr es:[rdi] */
  0xF4,                                    /*   2: hlt */
};
/* clang-format on */

/* clang-format off */
static const uint8_t SyscallSpin[] = {
  0xB8, 0x01, 0x00, 0x00, 0x00,            /*   0: mov eax, 0x1 */
  0x0F, 0x05,                              /*   5: syscall */
  0x48, 0xFF, 0xC3,                        /*   7: inc rbx */
  0xEB, 0xFB,                              /*  10: jmp 0x7 <loop> */
};
/* clang-format on */
#define SYSCALLSPIN_LOOP 7

/* clang-format off */
static const uint8_t SyscallLoop[] = {
  0x48, 0xFF, 0xC0,                        /*   0: inc rax */
  0x0F, 0x05,                              /*   3: syscall */
  0xEB, 0xF9,                              /*   5: jmp 0x0 <loop> */
};
/* clang-format on */
#define SYSCALLLOOP_LOOP 0

/* clang-format off */
static const uint8_t PatchLoop[] = {
  0x31, 0xDB,                              /*   0: xor ebx, ebx */
  0xB9, 0xE8, 0x03, 0x00, 0x00,            /*   2: mov ecx, 0x3e8 */
  0xFF, 0xD7,                              /*   7: call rdi */
  0x01, 0xC3,                              /*   9: add ebx, eax */
  0xFF, 0x47, 0x01,                        /*  11: inc dword ptr [rdi + 0x1] */
  0xFF, 0xC9,                              /*  14: dec ecx */
  0x75, 0xF5,                              /*  16: jne 0x7 <loop> */
  0xF4,                                    /*  18: hlt */
};
/* clang-format on */
#define PATCHLOOP_LOOP 7

/* clang-format off */
static const uint8_t SelfPatch[] = {
  0xC7, 0x05, 0x0C, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, /*   0: mov dword ptr [rip + 0xc], 0x90909090 # 0x16 <patch> */
  0xC6, 0x05, 0x09, 0x00, 0x00, 0x00, 0x90, /*  10: mov byte ptr [rip + 0x9], -0x70 # 0x1a <patch+0x4> */
  0xB8, 0x20, 0x00, 0x00, 0x00,            /*  17: mov eax, 0x20 */
  0xB8, 0xFF, 0xFF, 0xFF, 0xFF,            /*  22: mov eax, 0xffffffff */
  0xF4,                                    /*  27: hlt */
};
/* clang-format on */
#define SELFPATCH_PATCH 22

/* clang-format off */
static const uint8_t DataOnCodePage[] = {
  0xB9, 0x20, 0x4E, 0x00, 0x00,            /*   0: mov ecx, 0x4e20 */
  0x48, 0x83, 0x07, 0x01,                  /*   5: add qword ptr [rdi], 0x1 */
  0xFF, 0xC9,                              /*   9: dec ecx */
  0x75, 0xF8,                              /*  11: jne 0x5 <loop> */
  0xF4,                                    /*  13: hlt */
};
/* clang-format on */
#define DATAONCODEPAGE_LOOP 5

/* clang-format off */
static const uint8_t Spinner[] = {
  0xB8, 0x00, 0x00, 0x00, 0x00,            /*   0: mov eax, 0x0 */
  0x85, 0xC0,                              /*   5: test eax, eax */
  0x74, 0xF7,                              /*   7: je 0x0 <loop> */
  0xF4,                                    /*   9: hlt */
};
/* clang-format on */
#define SPINNER_LOOP 0

/* clang-format off */
static const uint8_t Patcher[] = {
  0xC7, 0x07, 0x07, 0x00, 0x00, 0x00,      /*   0: mov dword ptr [rdi], 0x7 */
  0xF4,                                    /*   6: hlt */
};
/* clang-format on */

/* clang-format off */
static const uint8_t MisalignedOnCode[] = {
  0xB8, 0x01, 0x00, 0x00, 0x00,            /*   0: mov eax, 0x1 */
  0xF0,                                    /*   5: lock */
  0x0F, 0xC1, 0x07,                        /*   6: xadd dword ptr [rdi], eax */
  0xF4,                                    /*   9: hlt */
};
/* clang-format on */

/* clang-format off */
static const uint8_t ReadAfterCallback[] = {
  0xB8, 0x03, 0x00, 0x00, 0x00,            /*   0: mov eax, 0x3 */
  0x0F, 0x05,                              /*   5: syscall */
  0xFF, 0xD7,                              /*   7: call rdi */
  0xF4,                                    /*   9: hlt */
};
/* clang-format on */


/* ---------------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------------- */

static uint64_t Addr(const void* Pointer) {
  return (uint64_t)(uintptr_t)Pointer;
}

static double Now(void) {
  struct timespec Time;
  clock_gettime(CLOCK_MONOTONIC, &Time);
  return (double)Time.tv_sec + (double)Time.tv_nsec * 1e-9;
}

static fexmac_context* MakeContext(int Multiblock, int TSO, const char* SMC, fexmac_syscall_fn Syscall, void* UserData) {
  const fexmac_option Options[] = {
    {"MULTIBLOCK", Multiblock ? "1" : "0"},
    {"TSOENABLED", TSO ? "1" : "0"},
    {"SMCCHECKS", SMC ? SMC : "mtrack"},
  };
  return CreateContextWithOptions(Options, 3, Syscall, UserData);
}

/* Maps Size bytes of guest code with Program at the start; registers it RWX if asked. */
static uint8_t* MapCode(fexmac_context* Context, const uint8_t* Program, size_t Size, int Register) {
  uint8_t* Code = MapGuestMemory(0x10000);
  memcpy(Code, Program, Size);
  if (Register) {
    CheckResult(fexmac_memory_map(Context, Addr(Code), 0x10000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE | FEXMAC_PROT_EXEC), FEXMAC_OK,
                "registering guest code");
  }
  return Code;
}

/* Guest memory with the given protection, registered with it if asked. */
static uint8_t* MapData(fexmac_context* Context, size_t Size, int HostProt, uint32_t GuestProt, int Register) {
  uint8_t* Data = MapGuestMemory(Size);
  if (Register) {
    CheckResult(fexmac_memory_map(Context, Addr(Data), Size, GuestProt), FEXMAC_OK, "registering guest data");
  } else {
    mprotect(Data, Size, HostProt);
  }
  return Data;
}

static void SetProt(fexmac_context* Context, void* Address, size_t Size, int HostProt, uint32_t GuestProt, int Registered) {
  if (Registered) {
    CheckResult(fexmac_memory_protect(Context, Addr(Address), Size, GuestProt), FEXMAC_OK, "changing guest protection");
  } else {
    mprotect(Address, Size, HostProt);
  }
}

static void CheckPageFault(const fexmac_stop* Stop, uint64_t Rip, uint64_t FaultAddress, uint32_t Access, int Signal, int Code,
                           uint32_t Error) {
  Check(Stop->reason == FEXMAC_STOP_GUEST_FAULT && Stop->trap_number == 14, "page fault stop (reason %d, trap %u)", Stop->reason,
        Stop->trap_number);
  Check(Stop->instruction_address == Rip, "at the faulting instruction (+0x%" PRIx64 ", expected +0x%" PRIx64 ")",
        Stop->instruction_address - Rip + Rip, Rip);
  Check(Stop->fault_address == FaultAddress, "fault address 0x%" PRIx64 " (expected 0x%" PRIx64 ")", Stop->fault_address, FaultAddress);
  Check(Stop->access == Access, "access %u (expected %u)", Stop->access, Access);
  Check(Stop->signal == Signal && Stop->si_code == Code, "signal %d si_code %d (expected %d, %d)", Stop->signal, Stop->si_code, Signal, Code);
  Check(Stop->error_code == Error, "error code 0x%x (expected 0x%x)", Stop->error_code, Error);
}

/* The arithmetic flags of RFLAGS. */
#define ARITH_FLAGS 0x8D5u

/* ---------------------------------------------------------------------------------
 * Guest fault delivery
 * --------------------------------------------------------------------------------- */

/* A load fault after several instructions of the same block changed registers, flags and
 * a vector register, and before instructions that overwrite the flags; every combination
 * of multiblock, memory-ordering emulation and registered memory. */
static void TestFaultMidBlock(void) {
  fexmac_install_fault_handlers();
  for (int Variant = 0; Variant < 8; ++Variant) {
    const int Multiblock = Variant & 1, TSO = (Variant >> 1) & 1, Registered = (Variant >> 2) & 1;
    printf("  multiblock %d, TSO %d, registered memory %d\n", Multiblock, TSO, Registered);
    fexmac_context* Context = MakeContext(Multiblock, TSO, NULL, NULL, NULL);
    uint8_t* Code = MapCode(Context, MidBlockFault, sizeof(MidBlockFault), Registered);
    uint8_t* Data = MapData(Context, 0x4000, PROT_NONE, FEXMAC_PROT_NONE, Registered);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RBX] = (uint64_t)-5;
    State.gpr[RDX] = 0xdead;
    State.gpr[RSI] = 0xbeef;
    State.gpr[RDI] = Addr(Data) + 8;
    State.gpr[8] = 100;
    SetState(Thread, &State);
    fexmac_stop Stop = Run(Thread);
    State = GetState(Thread);
    CheckPageFault(&Stop, Addr(Code) + MIDBLOCKFAULT_LOAD, Addr(Data) + 8, FEXMAC_ACCESS_READ, SIGSEGV, SEGV_ACCERR, FEXMAC_PF_USER);
    Check(State.rip == Addr(Code) + MIDBLOCKFAULT_LOAD, "rip at the load");
    Check(State.gpr[RAX] == 0x1111 && State.gpr[RBX] == 0 && State.gpr[RCX] == 0x1118, "registers written before it (rax %" PRIx64
          ", rbx %" PRIx64 ", rcx %" PRIx64 ")", State.gpr[RAX], State.gpr[RBX], State.gpr[RCX]);
    Check(State.gpr[RDX] == 0xdead && State.gpr[RSI] == 0xbeef && State.gpr[8] == 100, "registers written after it unchanged");
    Check(State.xmm[1][0] == 0x2230, "vector register written before it (xmm1 %" PRIx64 ")", State.xmm[1][0]);
    /* The flags of add are dead (add rdx and sub r8 overwrite them without reading them
     * after the load), and the core's flag optimisation does not compute them. Shown, not
     * checked; TestFaultFlags checks flags that are read later. */
    printf("  flags at the stop 0x%" PRIx64 " (0x55 if computed)\n", State.rflags & ARITH_FLAGS);
    SetProt(Context, Data, 0x4000, PROT_READ | PROT_WRITE, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, Registered);
    *(uint64_t*)(Data + 8) = 41;
    Stop = Run(Thread);
    State = GetState(Thread);
    Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RDX] == 42 && State.gpr[RSI] == 42 && State.gpr[8] == 97,
          "after the fix the load runs again and the block completes (rdx %" PRIu64 ", r8 %" PRIu64 ")", State.gpr[RDX], State.gpr[8]);
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* Flags that a later instruction of the block reads are reported as they were before the
 * faulting instruction. */
static void TestFaultFlags(void) {
  fexmac_install_fault_handlers();
  for (int Variant = 0; Variant < 4; ++Variant) {
    const int Multiblock = Variant & 1, TSO = Variant >> 1;
    fexmac_context* Context = MakeContext(Multiblock, TSO, NULL, NULL, NULL);
    uint8_t* Code = MapCode(Context, MidBlockFaultLive, sizeof(MidBlockFaultLive), 1);
    uint8_t* Data = MapData(Context, 0x4000, 0, FEXMAC_PROT_NONE, 1);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RBX] = (uint64_t)-5;
    State.gpr[RDI] = Addr(Data);
    State.rflags = 0x202 | 0x800 | 0x80; /* OF and SF set before; add clears them */
    SetState(Thread, &State);
    const fexmac_stop Stop = Run(Thread);
    State = GetState(Thread);
    Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && State.rip == Addr(Code) + MIDBLOCKFAULTLIVE_LOAD, "multiblock %d TSO %d: stop at the load",
          Multiblock, TSO);
    Check((State.rflags & ARITH_FLAGS) == 0x55, "flags as add left them (0x%" PRIx64 ", expected 0x55)", State.rflags & ARITH_FLAGS);
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* A store in a loop running into a read-only page: rcx shows the progress made. */
static void TestFaultStoreLoop(void) {
  fexmac_install_fault_handlers();
  for (int Variant = 0; Variant < 4; ++Variant) {
    const int Multiblock = Variant & 1, Registered = (Variant >> 1) & 1;
    printf("  multiblock %d, registered memory %d\n", Multiblock, Registered);
    fexmac_context* Context = MakeContext(Multiblock, 1, NULL, NULL, NULL);
    uint8_t* Code = MapCode(Context, StoreLoop, sizeof(StoreLoop), Registered);
    uint8_t* Data = MapData(Context, 0x8000, PROT_READ | PROT_WRITE, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, Registered);
    (void)*(volatile uint8_t*)(Data + 0x4000);
    SetProt(Context, Data + 0x4000, 0x4000, PROT_READ, FEXMAC_PROT_READ, Registered);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RDI] = Addr(Data);
    SetState(Thread, &State);
    fexmac_stop Stop = Run(Thread);
    State = GetState(Thread);
    CheckPageFault(&Stop, Addr(Code) + STORELOOP_LOOP, Addr(Data) + 0x4000, FEXMAC_ACCESS_WRITE, SIGSEGV, SEGV_ACCERR,
                   FEXMAC_PF_USER | FEXMAC_PF_WRITE | FEXMAC_PF_PRESENT);
    Check(State.gpr[RCX] == 2048, "rcx shows the 2048 stores made (%" PRIu64 ")", State.gpr[RCX]);
    SetProt(Context, Data + 0x4000, 0x4000, PROT_READ | PROT_WRITE, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, Registered);
    Stop = Run(Thread);
    State = GetState(Thread);
    Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RCX] == 4096 && ((uint64_t*)Data)[4095] == 4095 && ((uint64_t*)Data)[2048] == 2048,
          "the loop then completes");
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* Unmapped, PROT_NONE and allowed-but-refused pages, registered; unmapped unregistered. */
static void TestFaultKinds(void) {
  fexmac_install_fault_handlers();
  static const uint8_t LoadRdi[] = {0x48, 0x8B, 0x07, 0xF4}; /* mov rax, [rdi] ; hlt */
  for (int Kind = 0; Kind < 4; ++Kind) {
    fexmac_context* Context = MakeContext(1, 0, NULL, NULL, NULL);
    uint8_t* Code = MapCode(Context, LoadRdi, sizeof(LoadRdi), Kind != 3);
    /* Far from anything this process maps, so it stays unmapped. */
    uint8_t* Data = (Kind == 0 || Kind == 3) ? (uint8_t*)(uintptr_t)0x400000000000ULL : MapGuestMemory(0x4000);
    int Signal = SIGSEGV, Code_ = SEGV_ACCERR;
    uint32_t Error = FEXMAC_PF_USER;
    if (Kind == 0) {
      printf("  registered as unmapped, nothing mapped\n");
      CheckResult(fexmac_memory_unmap(Context, Addr(Data), 0x4000), FEXMAC_OK, "unmap");
      Code_ = SEGV_MAPERR;
    } else if (Kind == 1) {
      printf("  registered PROT_NONE\n");
      CheckResult(fexmac_memory_map(Context, Addr(Data), 0x4000, FEXMAC_PROT_NONE), FEXMAC_OK, "map PROT_NONE");
    } else if (Kind == 2) {
      printf("  registered readable, but the host page is inaccessible (as a file mapping past its end)\n");
      CheckResult(fexmac_memory_map(Context, Addr(Data), 0x4000, FEXMAC_PROT_READ), FEXMAC_OK, "map");
      mprotect(Data, 0x4000, PROT_NONE);
      Signal = SIGBUS;
      Code_ = BUS_ADRERR;
      Error |= FEXMAC_PF_PRESENT;
    } else {
      printf("  nothing registered, nothing mapped\n");
      Code_ = SEGV_MAPERR;
    }
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RDI] = Addr(Data) + 0x100;
    SetState(Thread, &State);
    const fexmac_stop Stop = Run(Thread);
    CheckPageFault(&Stop, Addr(Code), Addr(Data) + 0x100, FEXMAC_ACCESS_READ, Signal, Code_, Error);
    uint32_t Prot = 99;
    int Mapped = -1;
    fexmac_memory_query(Context, Addr(Data), &Prot, &Mapped);
    Check(Kind == 3 || Mapped == (Kind != 0), "query says mapped %d", Mapped);
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* rep lodsb into an inaccessible page shows its progress; rep stosb reports the state from
 * before the instruction (the core's limit) and completes when run again. */
static void TestFaultRep(void) {
  fexmac_install_fault_handlers();
  for (int TSO = 0; TSO < 2; ++TSO) {
    for (int Stos = 0; Stos < 2; ++Stos) {
      printf("  %s, TSO %d\n", Stos ? "rep stosb" : "rep lodsb", TSO);
      fexmac_context* Context = MakeContext(1, TSO, NULL, NULL, NULL);
      uint8_t* Code = MapCode(Context, Stos ? RepStos : RepLods, sizeof(RepLods), 1);
      uint8_t* Data = MapData(Context, 0x8000, 0, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, 1);
      SetProt(Context, Data + 0x4000, 0x4000, PROT_NONE, FEXMAC_PROT_NONE, 1);
      fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
      fexmac_x86_state State = GetState(Thread);
      const uint64_t Start = Addr(Data) + 0x4000 - 100;
      State.gpr[Stos ? RDI : RSI] = Start;
      State.gpr[RCX] = 1000;
      State.gpr[RAX] = 0x5A;
      SetState(Thread, &State);
      fexmac_stop Stop = Run(Thread);
      State = GetState(Thread);
      Check(Stop.reason == FEXMAC_STOP_GUEST_FAULT && Stop.instruction_address == Addr(Code) && State.rip == Addr(Code),
            "page fault at the rep instruction");
      Check(Stop.fault_address >= Start && Stop.fault_address < Addr(Data) + 0x4000 + 64,
            "fault address at the inaccessible page (+0x%" PRIx64 ")", Stop.fault_address - Addr(Data));
      if (Stos) {
        Check(State.gpr[RCX] == 1000 && State.gpr[RDI] == Start, "rep stos: registers as before the instruction (rcx %" PRIu64 ")",
              State.gpr[RCX]);
      } else {
        Check(State.gpr[RCX] == 900 && State.gpr[RSI] == Start + 100, "rep lods: rcx %" PRIu64 " and rsi +%" PRIu64 " show 100 bytes done",
              State.gpr[RCX], State.gpr[RSI] - Start);
      }
      SetProt(Context, Data + 0x4000, 0x4000, 0, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, 1);
      Stop = Run(Thread);
      State = GetState(Thread);
      Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RCX] == 0 && State.gpr[Stos ? RDI : RSI] == Start + 1000,
            "running again completes it (rcx %" PRIu64 ")", State.gpr[RCX]);
      if (Stos) {
        Check(Data[0x4000 - 100] == 0x5A && Data[0x4000 + 899] == 0x5A, "all 1000 bytes stored");
      }
      fexmac_thread_destroy(Thread);
      fexmac_context_destroy(Context);
    }
  }
}

/* A vector load fault: vector registers before it are reported, after it untouched. */
static void TestFaultVector(void) {
  fexmac_install_fault_handlers();
  for (int TSO = 0; TSO < 2; ++TSO) {
    fexmac_context* Context = CreateContextWithOptions(
      (const fexmac_option[]) {{"TSOENABLED", TSO ? "1" : "0"}, {"VECTORTSOENABLED", TSO ? "1" : "0"}}, 2, NULL, NULL);
    uint8_t* Code = MapCode(Context, VectorFault, sizeof(VectorFault), 1);
    uint8_t* Data = MapData(Context, 0x4000, 0, FEXMAC_PROT_NONE, 1);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RDI] = Addr(Data) + 32;
    State.xmm[2][0] = 0x0000000200000001ULL;
    State.xmm[2][1] = 0x0000000400000003ULL;
    State.xmm[3][0] = 0x000000140000000AULL;
    State.xmm[3][1] = 0x000000280000001EULL;
    State.xmm[0][0] = State.xmm[0][1] = 0x1111111111111111ULL;
    State.xmm[4][0] = State.xmm[4][1] = 0x4444444444444444ULL;
    SetState(Thread, &State);
    fexmac_stop Stop = Run(Thread);
    State = GetState(Thread);
    CheckPageFault(&Stop, Addr(Code) + VECTORFAULT_LOAD, Addr(Data) + 32, FEXMAC_ACCESS_READ, SIGSEGV, SEGV_ACCERR, FEXMAC_PF_USER);
    Check(State.xmm[2][0] == 0x000000160000000BULL && State.xmm[2][1] == 0x0000002C00000021ULL, "paddd before the load is in xmm2");
    Check(State.xmm[0][0] == 0x1111111111111111ULL && State.xmm[4][1] == 0x4444444444444444ULL, "xmm0 and xmm4 unchanged");
    SetProt(Context, Data, 0x4000, 0, FEXMAC_PROT_READ, 1);
    Stop = Run(Thread);
    State = GetState(Thread);
    Check(Stop.reason == FEXMAC_STOP_HALT && State.xmm[0][0] == 0 && State.xmm[4][1] == 0, "the load then completes");
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* push to an inaccessible stack page: rsp is as before the push. A fault two calls deep:
 * after the fix both returns go where they should (call/return stack consistent). */
static void TestFaultStack(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 0, NULL, NULL, NULL);
  uint8_t* Code = MapCode(Context, PushFault, sizeof(PushFault), 1);
  uint8_t* Stack = MapData(Context, 0x8000, 0, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, 1);
  SetProt(Context, Stack, 0x4000, 0, FEXMAC_PROT_NONE, 1);
  fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), Addr(Stack) + 0x4000);
  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state State = GetState(Thread);
  CheckPageFault(&Stop, Addr(Code) + PUSHFAULT_PUSH, Addr(Stack) + 0x4000 - 8, FEXMAC_ACCESS_WRITE, SIGSEGV, SEGV_ACCERR,
                 FEXMAC_PF_USER | FEXMAC_PF_WRITE);
  Check(State.gpr[RSP] == Addr(Stack) + 0x4000 && State.gpr[RAX] == 0x77, "rsp as before the push");
  SetProt(Context, Stack, 0x4000, 0, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, 1);
  Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RSP] == Addr(Stack) + 0x4000 - 8 && *(uint64_t*)(Stack + 0x4000 - 8) == 0x77,
        "the push then completes");
  fexmac_thread_destroy(Thread);

  uint8_t* Chain = MapCode(Context, CallChainFault, sizeof(CallChainFault), 1);
  uint8_t* Data = MapData(Context, 0x4000, 0, FEXMAC_PROT_NONE, 1);
  for (int Round = 0; Round < 3; ++Round) {
    Thread = CreateThreadAt(Context, Addr(Chain), Addr(Stack) + 0x8000);
    State = GetState(Thread);
    State.gpr[RDI] = Addr(Data);
    SetState(Thread, &State);
    SetProt(Context, Data, 0x4000, 0, FEXMAC_PROT_NONE, 1);
    Stop = Run(Thread);
    State = GetState(Thread);
    CheckPageFault(&Stop, Addr(Chain) + CALLCHAINFAULT_G, Addr(Data), FEXMAC_ACCESS_READ, SIGSEGV, SEGV_ACCERR, FEXMAC_PF_USER);
    Check(State.gpr[RSP] == Addr(Stack) + 0x8000 - 16, "two return addresses on the guest stack");
    SetProt(Context, Data, 0x4000, 0, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, 1);
    *(uint64_t*)Data = 1000;
    Stop = Run(Thread);
    State = GetState(Thread);
    Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 1001 && State.gpr[9] == 0x99 && State.gpr[RSP] == Addr(Stack) + 0x8000 &&
            State.rip == Addr(Chain) + 12,
          "both returns land where they should (rax %" PRIu64 ", rip +%" PRIu64 ")", State.gpr[RAX], State.rip - Addr(Chain));
    fexmac_thread_destroy(Thread);
  }
  fexmac_context_destroy(Context);
}

/* A jump to an unmapped and to a non-executable page: fetch faults at the target. The
 * embedder then delivers it by setting a new state (a handler), and in the second case also
 * fixes the page and runs the same rip again. */
static void TestFaultFetch(void) {
  fexmac_install_fault_handlers();
  for (int Kind = 0; Kind < 2; ++Kind) {
    printf("  %s\n", Kind ? "readable and writable, not executable" : "unmapped");
    fexmac_context* Context = MakeContext(1, 0, NULL, NULL, NULL);
    uint8_t* Code = MapCode(Context, JumpTo, sizeof(JumpTo), 1);
    uint8_t* HandlerCode = MapCode(Context, Handler, sizeof(Handler), 1);
    uint8_t* Target = MapGuestMemory(0x4000);
    if (Kind == 0) {
      munmap(Target, 0x4000);
      fexmac_memory_unmap(Context, Addr(Target), 0x4000);
    } else {
      fexmac_memory_map(Context, Addr(Target), 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE);
      memcpy(Target + 0x40, Handler, sizeof(Handler));
    }
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RAX] = Addr(Target) + 0x40;
    SetState(Thread, &State);
    fexmac_stop Stop = Run(Thread);
    State = GetState(Thread);
    CheckPageFault(&Stop, Addr(Target) + 0x40, Addr(Target) + 0x40, FEXMAC_ACCESS_EXECUTE, SIGSEGV, Kind ? SEGV_ACCERR : SEGV_MAPERR,
                   FEXMAC_PF_USER | FEXMAC_PF_INSTRUCTION | (Kind ? FEXMAC_PF_PRESENT : 0));
    Check(State.rip == Addr(Target) + 0x40, "rip at the target");
    /* Deliver: the embedder builds a frame and runs a handler. */
    State.rip = Addr(HandlerCode);
    SetState(Thread, &State);
    Stop = Run(Thread);
    State = GetState(Thread);
    Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[8] == 0x5A, "the handler set as new state runs");
    if (Kind == 1) {
      fexmac_memory_protect(Context, Addr(Target), 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_EXEC);
      State.rip = Addr(Target) + 0x40;
      State.gpr[8] = 0;
      SetState(Thread, &State);
      Stop = Run(Thread);
      State = GetState(Thread);
      Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[8] == 0x5A, "after the page is made executable the code at the target runs");
    }
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

static void TestMemoryArguments(void) {
  fexmac_context* Context = MakeContext(1, 0, NULL, NULL, NULL);
  uint8_t* Data = MapGuestMemory(0x4000);
  CheckResult(fexmac_memory_map(Context, Addr(Data) + 1, 0x1000, FEXMAC_PROT_READ), FEXMAC_ERROR_INVALID_ARGUMENT, "unaligned address");
  CheckResult(fexmac_memory_map(Context, Addr(Data), 0x800, FEXMAC_PROT_READ), FEXMAC_ERROR_INVALID_ARGUMENT, "unaligned size");
  CheckResult(fexmac_memory_map(Context, Addr(Data), 0x1000, 8), FEXMAC_ERROR_INVALID_ARGUMENT, "unknown protection bit");
  CheckResult(fexmac_memory_map(Context, 1ULL << 47, 0x1000, FEXMAC_PROT_READ), FEXMAC_ERROR_INVALID_ARGUMENT, "beyond 2^47");
  CheckResult(fexmac_memory_protect(Context, Addr(Data), 0x4000, FEXMAC_PROT_READ), FEXMAC_ERROR_INVALID_ARGUMENT,
              "protect of a range never mapped");
  CheckResult(fexmac_memory_map(Context, Addr(Data), 0x4000, FEXMAC_PROT_READ), FEXMAC_OK, "map");
  CheckResult(fexmac_memory_protect(Context, Addr(Data), 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE), FEXMAC_OK, "protect");
  CheckResult(fexmac_memory_write_end(Context, Addr(Data), 0x10), FEXMAC_ERROR_INVALID_ARGUMENT, "write_end without begin");
  /* 4K protections the 16K host page cannot enforce are refused unless accepted. */
  CheckResult(fexmac_memory_protect(Context, Addr(Data), 0x1000, FEXMAC_PROT_READ), FEXMAC_ERROR_UNSUPPORTED,
              "a read-only 4K page inside a writable host page");
  uint32_t Prot = 0;
  fexmac_memory_query(Context, Addr(Data), &Prot, NULL);
  Check(Prot == (FEXMAC_PROT_READ | FEXMAC_PROT_WRITE) && fexmac_memory_unenforced_count(Context) == 0, "the refused change changed nothing");
  CheckResult(fexmac_memory_protect(Context, Addr(Data), 0x1000, FEXMAC_PROT_READ | FEXMAC_PROT_ALLOW_UNENFORCED), FEXMAC_OK,
              "the same, accepted with FEXMAC_PROT_ALLOW_UNENFORCED");
  fexmac_memory_query(Context, Addr(Data), &Prot, NULL);
  Check(Prot == FEXMAC_PROT_READ && fexmac_memory_unenforced_count(Context) == 1, "recorded, and counted");
  CheckResult(fexmac_memory_protect(Context, Addr(Data), 0x4000, FEXMAC_PROT_READ), FEXMAC_OK, "the whole host page read-only");
  CheckResult(fexmac_memory_map(NULL, Addr(Data), 0x4000, FEXMAC_PROT_READ), FEXMAC_ERROR_INVALID_HANDLE, "no context");
  CheckResult(fexmac_thread_request_stop(NULL), FEXMAC_ERROR_INVALID_ARGUMENT, "stop request for NULL");
  fexmac_context_destroy(Context);
}

/* ---------------------------------------------------------------------------------
 * Stopping a thread from another thread
 * --------------------------------------------------------------------------------- */

struct Looper {
  fexmac_thread* Thread;
  uint64_t Base;
  int Jcc;
  atomic_int Stops;
  atomic_int Quit;
  atomic_int Bad;
  atomic_int Other;
  uint64_t LastRax;
};

/* Runs the counting loop, answering each requested stop by checking the state and
 * running again. Invariant at every stop: rip at the loop head and rbx == 2 * rax. */
static void* LooperMain(void* Argument) {
  struct Looper* L = Argument;
  while (!atomic_load(&L->Quit)) {
    fexmac_stop Stop = Run(L->Thread);
    if (Stop.reason != FEXMAC_STOP_REQUESTED) {
      atomic_fetch_add(&L->Other, 1);
      continue;
    }
    const fexmac_x86_state State = GetState(L->Thread);
    /* The loop head, or (a loop closed by jmp in one translation unit) the jmp itself. */
    const int AtBoundary = State.rip == L->Base || (!L->Jcc && State.rip == L->Base + 7);
    if (!AtBoundary || State.gpr[RBX] != 2 * State.gpr[RAX] || State.gpr[RAX] < L->LastRax ||
        Stop.instruction_address != State.rip || (L->Jcc && State.gpr[RCX] + State.gpr[RAX] != (1ULL << 62))) {
      atomic_fetch_add(&L->Bad, 1);
    }
    L->LastRax = State.gpr[RAX];
    atomic_fetch_add(&L->Stops, 1);
  }
  return NULL;
}

static void StartLooper(fexmac_context* Context, struct Looper* L, int Jcc) {
  memset(L, 0, sizeof(*L));
  uint8_t* Code = MapCode(Context, Jcc ? CountLoopJcc : CountLoopJmp, Jcc ? sizeof(CountLoopJcc) : sizeof(CountLoopJmp), 0);
  L->Base = Addr(Code);
  L->Jcc = Jcc;
  L->Thread = CreateThreadAt(Context, Addr(Code), 0);
  fexmac_x86_state State = GetState(L->Thread);
  State.gpr[RCX] = 1ULL << 62;
  SetState(L->Thread, &State);
}

static int CompareDoubles(const void* A, const void* B) {
  const double X = *(const double*)A, Y = *(const double*)B;
  return X < Y ? -1 : X > Y;
}

static void* BusyHost(void* Argument) {
  atomic_int* Quit = Argument;
  volatile uint64_t X = 0;
  while (!atomic_load(Quit)) {
    ++X;
  }
  return NULL;
}

/* Thousands of stops of a tight loop, with and without multiblock (a block linked to
 * itself, a loop closed by jmp or by jnz inside one translation unit), while other host
 * threads keep every core busy. Measures the time from the request to the return. */
static void TestStopLatency(void) {
  fexmac_install_fault_handlers();
  pthread_t Busy[8];
  atomic_int BusyQuit = 0;
  for (int i = 0; i < 8; ++i) {
    pthread_create(&Busy[i], NULL, BusyHost, &BusyQuit);
  }
  for (int Variant = 0; Variant < 4; ++Variant) {
    const int Multiblock = Variant & 1, Jcc = Variant >> 1;
    fexmac_context* Context = MakeContext(Multiblock, 0, NULL, NULL, NULL);
    struct Looper L;
    StartLooper(Context, &L, Jcc);
    pthread_t Runner;
    pthread_create(&Runner, NULL, LooperMain, &L);
    enum { Count = 2000 };
    static double Latency[Count];
    int Missed = 0;
    for (int i = 0; i < Count; ++i) {
      const int Before = atomic_load(&L.Stops);
      const double Start = Now();
      fexmac_thread_request_stop(L.Thread);
      while (atomic_load(&L.Stops) == Before) {
        if (Now() - Start > 2.0) {
          ++Missed;
          break;
        }
      }
      Latency[i] = Now() - Start;
    }
    atomic_store(&L.Quit, 1);
    fexmac_thread_request_stop(L.Thread);
    pthread_join(Runner, NULL);
    qsort(Latency, Count, sizeof(double), CompareDoubles);
    printf("  multiblock %d, loop closed by %s: %d stops, latency median %.1f us, 99%% %.1f us, max %.1f us\n", Multiblock,
           Jcc ? "jnz" : "jmp", atomic_load(&L.Stops), Latency[Count / 2] * 1e6, Latency[Count * 99 / 100] * 1e6, Latency[Count - 1] * 1e6);
    Check(Missed == 0, "every request answered within 2 s (%d missed)", Missed);
    Check(atomic_load(&L.Bad) == 0 && atomic_load(&L.Other) == 0, "every stop at an instruction boundary of the loop with rbx == 2*rax (%d bad, %d other)",
          atomic_load(&L.Bad), atomic_load(&L.Other));
    Check(L.LastRax > 0, "the loop made progress between stops (rax %" PRIu64 ")", L.LastRax);
    fexmac_thread_destroy(L.Thread);
    fexmac_context_destroy(Context);
  }
  atomic_store(&BusyQuit, 1);
  for (int i = 0; i < 8; ++i) {
    pthread_join(Busy[i], NULL);
  }
}

static void* RequestAfterDelay(void* Thread) {
  usleep(3000);
  fexmac_thread_request_stop(Thread);
  return NULL;
}

/* A long repe cmpsb is stopped part-way: rip at the instruction, rcx/rsi/rdi show the
 * progress, and running again finishes it. */
static void TestStopRep(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 0, NULL, NULL, NULL);
  uint8_t* Code = MapCode(Context, RepCmps, sizeof(RepCmps), 0);
  const size_t Size = 256 << 20;
  uint8_t* A = MapGuestMemory(Size);
  uint8_t* B = MapGuestMemory(Size);
  fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RSI] = Addr(A);
  State.gpr[RDI] = Addr(B);
  State.gpr[RCX] = Size;
  SetState(Thread, &State);
  int Stops = 0, Bad = 0;
  for (;;) {
    pthread_t Requester;
    pthread_create(&Requester, NULL, RequestAfterDelay, Thread);
    const fexmac_stop Stop = Run(Thread);
    pthread_join(Requester, NULL);
    State = GetState(Thread);
    if (Stop.reason == FEXMAC_STOP_HALT) {
      break;
    }
    if (Stop.reason != FEXMAC_STOP_REQUESTED || State.rip != Addr(Code) || State.gpr[RSI] - Addr(A) != Size - State.gpr[RCX] ||
        State.gpr[RDI] - Addr(B) != Size - State.gpr[RCX]) {
      ++Bad;
    }
    ++Stops;
    if (Stops > 100000) {
      break;
    }
  }
  printf("  %d stops part-way through a 256MB repe cmpsb\n", Stops);
  Check(Stops > 0, "stopped part-way at least once");
  Check(Bad == 0, "each stop at the instruction with rcx, rsi and rdi consistent (%d bad)", Bad);
  Check(State.gpr[RCX] == 0 && State.gpr[RSI] == Addr(A) + Size && (State.rflags & 0x40), "the comparison completed, equal");
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* A thread not running: the request is answered by the next run, at once, and only one
 * stop answers several requests. A stop requested from a signal handler. */
static atomic_int SignalStopDone;
static fexmac_thread* SignalTarget;
static void StopFromSignal(int Signal) {
  (void)Signal;
  atomic_store(&SignalStopDone, fexmac_thread_request_stop(SignalTarget) == FEXMAC_OK ? 1 : 2);
}

static void TestStopNotRunning(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 0, NULL, NULL, NULL);
  uint8_t* Code = MapCode(Context, Handler, sizeof(Handler), 0);
  fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
  for (int i = 0; i < 3; ++i) {
    CheckResult(fexmac_thread_request_stop(Thread), FEXMAC_OK, "request for a thread not running");
  }
  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_REQUESTED && State.rip == Addr(Code) && State.gpr[8] == 0 && Stop.instruction_address == Addr(Code),
        "the next run returns the stop before running anything");
  Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[8] == 0x5A, "three requests, one stop; then the thread runs");

  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_handler = StopFromSignal;
  sigaction(SIGUSR1, &Action, NULL);
  State.rip = Addr(Code);
  SetState(Thread, &State);
  SignalTarget = Thread;
  raise(SIGUSR1);
  Check(atomic_load(&SignalStopDone) == 1, "request from a signal handler accepted");
  Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_REQUESTED, "and answered");
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* A stop requested while the system call callback runs: the callback finishes, the guest
 * resumes and stops at the next block entry. Also: the callback stops the run itself, and
 * the request is answered by the next run. */
static atomic_int CallbackEntered;
static atomic_int CallbackMayReturn;
static int CallbackStops;
static fexmac_syscall_action SlowSyscall(fexmac_thread* Thread, void* UserData) {
  (void)Thread;
  (void)UserData;
  atomic_store(&CallbackEntered, 1);
  while (!atomic_load(&CallbackMayReturn)) {
  }
  /* Also takes the invalidation lock with the interrupt page read-only. */
  fexmac_invalidate(fexmac_thread_context(Thread), 0x10000, 0x1000);
  return CallbackStops ? FEXMAC_SYSCALL_STOP : FEXMAC_SYSCALL_RESUME;
}

static void* RequestWhenEntered(void* Argument) {
  while (!atomic_load(&CallbackEntered)) {
  }
  fexmac_thread_request_stop(Argument);
  usleep(20000);
  atomic_store(&CallbackMayReturn, 1);
  return NULL;
}

static void TestStopDuringCallback(void) {
  fexmac_install_fault_handlers();
  for (CallbackStops = 0; CallbackStops < 2; ++CallbackStops) {
    atomic_store(&CallbackEntered, 0);
    atomic_store(&CallbackMayReturn, 0);
    fexmac_context* Context = MakeContext(1, 0, NULL, SlowSyscall, NULL);
    uint8_t* Code = MapCode(Context, SyscallSpin, sizeof(SyscallSpin), 0);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    pthread_t Requester;
    pthread_create(&Requester, NULL, RequestWhenEntered, Thread);
    fexmac_stop Stop = Run(Thread);
    pthread_join(Requester, NULL);
    fexmac_x86_state State = GetState(Thread);
    if (!CallbackStops) {
      Check(Stop.reason == FEXMAC_STOP_REQUESTED && State.rip == Addr(Code) + SYSCALLSPIN_LOOP,
            "callback resumes: stop at the next block entry (reason %d, rip +%" PRIu64 ")", Stop.reason, State.rip - Addr(Code));
    } else {
      Check(Stop.reason == FEXMAC_STOP_SYSCALL, "callback stops the run itself (reason %d)", Stop.reason);
      Stop = Run(Thread);
      Check(Stop.reason == FEXMAC_STOP_REQUESTED, "the pending request answers the next run (reason %d)", Stop.reason);
    }
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* Eight guest threads looping, stopped round-robin 500 times each; plus a thread that
 * enters and leaves runs constantly (a system call stop each iteration) while requests
 * hammer it. */
static void* Hammer(void* Argument) {
  struct Looper* L = Argument;
  while (!atomic_load(&L->Quit)) {
    fexmac_thread_request_stop(L->Thread);
  }
  return NULL;
}

static void TestStopManyThreads(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 1, NULL, NULL, NULL);
  enum { Threads = 8, Rounds = 500 };
  static struct Looper L[Threads];
  pthread_t Runner[Threads];
  for (int i = 0; i < Threads; ++i) {
    StartLooper(Context, &L[i], i & 1);
    pthread_create(&Runner[i], NULL, LooperMain, &L[i]);
  }
  int Missed = 0;
  for (int Round = 0; Round < Rounds; ++Round) {
    for (int i = 0; i < Threads; ++i) {
      const int Before = atomic_load(&L[i].Stops);
      const double Start = Now();
      fexmac_thread_request_stop(L[i].Thread);
      while (atomic_load(&L[i].Stops) == Before) {
        if (Now() - Start > 2.0) {
          ++Missed;
          break;
        }
      }
    }
  }
  int Bad = 0, Total = 0;
  for (int i = 0; i < Threads; ++i) {
    atomic_store(&L[i].Quit, 1);
    fexmac_thread_request_stop(L[i].Thread);
    pthread_join(Runner[i], NULL);
    Bad += atomic_load(&L[i].Bad) + atomic_load(&L[i].Other);
    Total += atomic_load(&L[i].Stops);
  }
  Check(Missed == 0 && Total >= Threads * Rounds, "%d stops of %d threads, %d missed", Total, Threads, Missed);
  Check(Bad == 0, "every stop consistent (%d bad)", Bad);

  /* Entering and leaving. */
  uint8_t* Code = MapCode(Context, SyscallLoop, sizeof(SyscallLoop), 0);
  struct Looper H;
  memset(&H, 0, sizeof(H));
  H.Thread = CreateThreadAt(Context, Addr(Code), 0);
  pthread_t Hammerer;
  pthread_create(&Hammerer, NULL, Hammer, &H);
  int Requested = 0, Syscalls = 0, Wrong = 0;
  uint64_t Rax = 0;
  for (int i = 0; i < 20000; ++i) {
    const fexmac_stop Stop = Run(H.Thread);
    const fexmac_x86_state State = GetState(H.Thread);
    if (Stop.reason == FEXMAC_STOP_REQUESTED) {
      ++Requested;
    } else if (Stop.reason == FEXMAC_STOP_SYSCALL) {
      ++Syscalls;
    } else {
      ++Wrong;
    }
    if (State.gpr[RAX] < Rax || (State.rip != Addr(Code) && State.rip != Addr(Code) + 3 + 2 && State.rip != Addr(Code) + 3)) {
      ++Wrong;
    }
    Rax = State.gpr[RAX];
  }
  atomic_store(&H.Quit, 1);
  pthread_join(Hammerer, NULL);
  printf("  entering and leaving: %d requested stops, %d system call stops\n", Requested, Syscalls);
  Check(Wrong == 0 && Requested > 0 && Syscalls > 0, "races with entering and leaving runs are answered consistently (%d wrong)", Wrong);
  fexmac_thread_destroy(H.Thread);
  for (int i = 0; i < Threads; ++i) {
    fexmac_thread_destroy(L[i].Thread);
  }
  fexmac_context_destroy(Context);
}

/* ---------------------------------------------------------------------------------
 * Self-modifying code detected by write-protecting translated pages (SMCCHECKS mtrack)
 * --------------------------------------------------------------------------------- */

/* Calls a function, patches its immediate, calls it again, 1000 times; the callee is on
 * its own host page. And the same after the code was written by the guest in the first
 * place (write then execute). */
static void TestSMCPatchLoop(void) {
  fexmac_install_fault_handlers();
  for (int Multiblock = 0; Multiblock < 2; ++Multiblock) {
    fexmac_context* Context = MakeContext(Multiblock, 0, "mtrack", NULL, NULL);
    uint8_t* Code = MapCode(Context, PatchLoop, sizeof(PatchLoop), 1);
    uint8_t* Callee = MapCode(Context, (const uint8_t[]) {0xB8, 0, 0, 0, 0, 0xC3}, 6, 1);
    uint8_t* Stack = MapData(Context, 0x4000, 0, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, 1);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), Addr(Stack) + 0x4000);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RDI] = Addr(Callee);
    SetState(Thread, &State);
    const double Start = Now();
    const fexmac_stop Stop = Run(Thread);
    const double Time = Now() - Start;
    State = GetState(Thread);
    const uint64_t Faults = fexmac_memory_code_write_count(Context);
    printf("  multiblock %d: %" PRIu64 " code writes handled, %.1f us per patch-and-call\n", Multiblock, Faults, Time * 1e6 / 1000);
    Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RBX] == 499500, "every call ran the patched code (sum %" PRIu64 ", expected 499500)",
          State.gpr[RBX]);
    Check(Faults >= 999, "the patches were caught by write protection (%" PRIu64 ")", Faults);
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* A block that writes into its own later instructions (SameBlock.asm): runs the new code,
 * does not loop. */
static void TestSMCSelfPatch(void) {
  fexmac_install_fault_handlers();
  for (int Variant = 0; Variant < 4; ++Variant) {
    const int Multiblock = Variant & 1, TSO = Variant >> 1;
    fexmac_context* Context = MakeContext(Multiblock, TSO, "mtrack", NULL, NULL);
    uint8_t* Code = MapCode(Context, SelfPatch, sizeof(SelfPatch), 1);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    const fexmac_stop Stop = Run(Thread);
    const fexmac_x86_state State = GetState(Thread);
    Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 32, "multiblock %d TSO %d: the patched instruction did not run (eax 0x%" PRIx64 ")",
          Multiblock, TSO, State.gpr[RAX]);
    Check(Code[SELFPATCH_PATCH] == 0x90 && fexmac_memory_code_write_count(Context) >= 1, "the write happened and was caught");
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* Data written in a loop on the same host page as the loop's code: on another 4K guest page
 * of the same 16K host page, and on the same 4K page. Correct, and measured. */
static void TestSMCDataOnCodePage(void) {
  fexmac_install_fault_handlers();
  for (int Variant = 0; Variant < 3; ++Variant) {
    static const uint64_t Offset[] = {0x8000, 0x2000, 0x800};
    static const char* What[] = {"another host page", "another 4K page of the same host page", "the same 4K page"};
    fexmac_context* Context = MakeContext(1, 0, "mtrack", NULL, NULL);
    uint8_t* Code = MapCode(Context, DataOnCodePage, sizeof(DataOnCodePage), 1);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RDI] = Addr(Code) + Offset[Variant];
    SetState(Thread, &State);
    const double Start = Now();
    const fexmac_stop Stop = Run(Thread);
    const double Time = Now() - Start;
    const uint64_t Faults = fexmac_memory_code_write_count(Context);
    printf("  data on %s: 20000 iterations in %.1f ms (%.2f us each), %" PRIu64 " code writes handled\n", What[Variant], Time * 1e3,
           Time * 1e6 / 20000, Faults);
    Check(Stop.reason == FEXMAC_STOP_HALT && *(uint64_t*)(Code + Offset[Variant]) == 20000, "the counter reached 20000 (%" PRIu64 ")",
          *(uint64_t*)(Code + Offset[Variant]));
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* A misaligned atomic on a write-protected code page. */
static void TestSMCMisalignedAtomic(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 0, "mtrack", NULL, NULL);
  uint8_t* Code = MapCode(Context, MisalignedOnCode, sizeof(MisalignedOnCode), 1);
  fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = Addr(Code) + 0x100 + 14;
  SetState(Thread, &State);
  const fexmac_stop Stop = Run(Thread);
  State = GetState(Thread);
  uint32_t Value;
  memcpy(&Value, Code + 0x100 + 14, 4);
  Check(Stop.reason == FEXMAC_STOP_HALT && Value == 1, "lock xadd across 16 bytes on the code page completed (value %u)", Value);
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

/* Several guest threads spin in a loop whose immediate another guest thread patches. */
struct Spin {
  fexmac_thread* Thread;
  fexmac_stop Stop;
};
static void* SpinMain(void* Argument) {
  struct Spin* S = Argument;
  S->Stop = Run(S->Thread);
  return NULL;
}

static void TestSMCCrossThread(void) {
  fexmac_install_fault_handlers();
  atomic_int BusyQuit = 0;
  pthread_t Busy[4];
  for (int i = 0; i < 4; ++i) {
    pthread_create(&Busy[i], NULL, BusyHost, &BusyQuit);
  }
  for (int Round = 0; Round < 40; ++Round) {
    const int Multiblock = Round & 1;
    fexmac_context* Context = MakeContext(Multiblock, 1, "mtrack", NULL, NULL);
    enum { Spinners = 4 };
    struct Spin S[Spinners];
    pthread_t Threads[Spinners];
    uint8_t* SpinCode = MapCode(Context, Spinner, sizeof(Spinner), 1);
    for (int i = 0; i < Spinners; ++i) {
      S[i].Thread = CreateThreadAt(Context, Addr(SpinCode), 0);
      pthread_create(&Threads[i], NULL, SpinMain, &S[i]);
    }
    usleep(100000);
    uint8_t* PatchCode = MapCode(Context, Patcher, sizeof(Patcher), 1);
    fexmac_thread* Writer = CreateThreadAt(Context, Addr(PatchCode), 0);
    fexmac_x86_state State = GetState(Writer);
    State.gpr[RDI] = Addr(SpinCode) + 1;
    SetState(Writer, &State);
    const double Start = Now();
    const fexmac_stop Stop = Run(Writer);
    Check(Stop.reason == FEXMAC_STOP_HALT, "the writer thread's store completed");
    int Done = 0;
    for (int i = 0; i < Spinners; ++i) {
      pthread_join(Threads[i], NULL);
      const fexmac_x86_state After = GetState(S[i].Thread);
      Done += S[i].Stop.reason == FEXMAC_STOP_HALT && After.gpr[RAX] == 7;
      fexmac_thread_destroy(S[i].Thread);
    }
    if (Round < 2 || Done != Spinners) {
      printf("  round %d, multiblock %d: all spinners saw the patch within %.1f ms\n", Round, Multiblock, (Now() - Start) * 1e3);
      Check(Done == Spinners, "every spinning thread left its loop with the new value (%d of %d)", Done, Spinners);
    }
    fexmac_thread_destroy(Writer);
    fexmac_context_destroy(Context);
  }
  Check(1, "40 rounds of 4 spinners and a patching thread, with 4 busy host threads");
  atomic_store(&BusyQuit, 1);
  for (int i = 0; i < 4; ++i) {
    pthread_join(Busy[i], NULL);
  }
}

/* The embedder writes code into a translated page from its system call callback, between
 * fexmac_memory_write_begin and _end (as for a read() into guest memory); the guest then
 * calls the new code. */
static uint8_t* CallbackTarget;
static fexmac_syscall_action WriteCodeSyscall(fexmac_thread* Thread, void* UserData) {
  (void)UserData;
  fexmac_context* Context = fexmac_thread_context(Thread);
  static const uint8_t NewCode[] = {0xB8, 0x2A, 0, 0, 0, 0xC3}; /* mov eax, 42 ; ret */
  fexmac_memory_write_begin(Context, Addr(CallbackTarget), sizeof(NewCode));
  memcpy(CallbackTarget, NewCode, sizeof(NewCode));
  fexmac_memory_write_end(Context, Addr(CallbackTarget), sizeof(NewCode));
  return FEXMAC_SYSCALL_RESUME;
}

static void RunEmbedderWrite(fexmac_syscall_fn Syscall) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 0, "mtrack", Syscall, NULL);
  uint8_t* Code = MapCode(Context, ReadAfterCallback, sizeof(ReadAfterCallback), 1);
  CallbackTarget = MapCode(Context, (const uint8_t[]) {0xB8, 1, 0, 0, 0, 0xC3}, 6, 1);
  uint8_t* Stack = MapData(Context, 0x4000, 0, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, 1);
  /* Translate the old code first, so its page is write-protected. */
  static const uint8_t CallOnce[] = {0xFF, 0xD7, 0xF4};
  uint8_t* Warm = MapCode(Context, CallOnce, sizeof(CallOnce), 1);
  fexmac_thread* Thread = CreateThreadAt(Context, Addr(Warm), Addr(Stack) + 0x4000);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = Addr(CallbackTarget);
  SetState(Thread, &State);
  fexmac_stop Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 1, "the old code ran");
  State.rip = Addr(Code);
  SetState(Thread, &State);
  Shared[0] = 1;
  Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 42, "the code the callback wrote ran (eax %" PRIu64 ")", State.gpr[RAX]);
  Shared[0] = 2;
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
}

static void TestSMCEmbedderWrite(void) {
  RunEmbedderWrite(WriteCodeSyscall);
}

/* The same write without write_begin: the host write faults and is not the library's;
 * the process dies with a message saying what to call. */
static fexmac_syscall_action WriteCodeUnbracketed(fexmac_thread* Thread, void* UserData) {
  (void)Thread;
  (void)UserData;
  CallbackTarget[1] = 0x2A;
  return FEXMAC_SYSCALL_RESUME;
}

static void TestSMCEmbedderWriteUnbracketed(void) {
  RunEmbedderWrite(WriteCodeUnbracketed);
}

/* ---------------------------------------------------------------------------------
 * Misaligned locked operations on memory the guest may not access
 *
 * A locked operation on a word crossing 16 bytes is emulated by the core in the fault
 * handler, under its process-wide split-lock lock. With the word's second half on an
 * inaccessible page, the library reports the guest's page fault before emulating anything
 * (from the outer handler, nothing written, the lock never taken); several threads fault,
 * the embedder fixes the page, every thread runs the same instruction again, and the
 * totals are exact. A further round afterwards shows the lock is free.
 * --------------------------------------------------------------------------------- */

/* clang-format off */
static const uint8_t SplitXadd[] = {
  0x4C, 0x89, 0xC0,                         /*  0: mov rax, r8 */
  0xF0, 0x48, 0x0F, 0xC1, 0x07,             /*  3: lock xadd [rdi], rax */
  0x48, 0xFF, 0xC9,                         /*  8: dec rcx */
  0x75, 0xF3,                               /* 11: jnz 0 */
  0xF4,                                     /* 13: hlt */
};
static const uint8_t SplitCmpxchg[] = {
  0x48, 0x8B, 0x07,                         /*  0: mov rax, [rdi] */
  0x4A, 0x8D, 0x14, 0x00,                   /*  3: lea rdx, [rax + r8] */
  0xF0, 0x48, 0x0F, 0xB1, 0x17,             /*  7: lock cmpxchg [rdi], rdx */
  0x75, 0xF2,                               /* 12: jnz 0 */
  0x48, 0xFF, 0xC9,                         /* 14: dec rcx */
  0x75, 0xED,                               /* 17: jnz 0 */
  0xF4,                                     /* 19: hlt */
};
static const uint8_t SplitXchgLock[] = {
  0xB8, 0x01, 0x00, 0x00, 0x00,             /*  0: mov eax, 1 */
  0x48, 0x87, 0x06,                         /*  5: xchg [rsi], rax */
  0x48, 0x85, 0xC0,                         /*  8: test rax, rax */
  0x75, 0xF3,                               /* 11: jnz 0 */
  0x48, 0xFF, 0x07,                         /* 13: inc qword [rdi] */
  0x48, 0xC7, 0x06, 0x00, 0x00, 0x00, 0x00, /* 16: mov qword [rsi], 0 */
  0x48, 0xFF, 0xC9,                         /* 23: dec rcx */
  0x75, 0xE4,                               /* 26: jnz 0 */
  0xF4,                                     /* 28: hlt */
};
/* clang-format on */

enum { SplitFaultThreads = 4, SplitFaultIterations = 20000 };

struct SplitRun {
  fexmac_context* Context;
  uint64_t Code, Rdi, Rsi, LockedAt, Word;
  atomic_int Faulted;
  atomic_int Fixed;
  atomic_int Bad;
  atomic_int Halted;
};

static void* SplitFaultWorker(void* Argument) {
  struct SplitRun* R = Argument;
  fexmac_thread* Thread = CreateThreadAt(R->Context, R->Code, 0);
  fexmac_x86_state State = GetState(Thread);
  State.gpr[RDI] = R->Rdi;
  State.gpr[RSI] = R->Rsi;
  State.gpr[RCX] = SplitFaultIterations;
  State.gpr[8] = 0x100000001ULL;
  SetState(Thread, &State);
  for (;;) {
    const fexmac_stop Stop = Run(Thread);
    if (Stop.reason == FEXMAC_STOP_HALT) {
      const fexmac_x86_state Final = GetState(Thread);
      if (Final.gpr[RCX] != 0 || Final.gpr[8] != 0x100000001ULL || Final.gpr[RDI] != R->Rdi) {
        printf("  halted with rcx %" PRIu64 " r8 0x%" PRIx64 " rdi 0x%" PRIx64 "\n", Final.gpr[RCX], Final.gpr[8], Final.gpr[RDI]);
        atomic_fetch_add(&R->Bad, 1);
      }
      atomic_fetch_add(&R->Halted, 1);
      break;
    }
    const fexmac_x86_state After = GetState(Thread);
    /* The locked instruction (a write), or, for the cmpxchg loop on a PROT_NONE page, the
     * plain load of the word before it (a read). */
    const int AtLocked = After.rip == R->LockedAt && Stop.access == FEXMAC_ACCESS_WRITE;
    const int AtLoad = After.rip == R->Code && Stop.access == FEXMAC_ACCESS_READ;
    if (Stop.reason != FEXMAC_STOP_GUEST_FAULT || Stop.trap_number != 14 || !(AtLocked || AtLoad) || Stop.fault_address < R->Word ||
        Stop.fault_address >= R->Word + 8) {
      atomic_fetch_add(&R->Bad, 1);
      break;
    }
    atomic_fetch_add(&R->Faulted, 1);
    while (!atomic_load(&R->Fixed)) {
    }
  }
  fexmac_thread_destroy(Thread);
  return NULL;
}

static void TestSplitLockFaults(void) {
  fexmac_install_fault_handlers();
  static const struct {
    const char* Name;
    const uint8_t* Program;
    size_t Size;
    uint64_t LockedAt;
  } Programs[] = {
    {"lock xadd", SplitXadd, sizeof(SplitXadd), 3},
    {"lock cmpxchg", SplitCmpxchg, sizeof(SplitCmpxchg), 7},
    {"xchg spin lock", SplitXchgLock, sizeof(SplitXchgLock), 5},
  };
  for (int Variant = 0; Variant < 12; ++Variant) {
    const int Which = Variant % 3, ReadOnly = (Variant / 3) & 1, Registered = Variant / 6;
    printf("  %s, second half on a %s page, %s\n", Programs[Which].Name, ReadOnly ? "read-only" : "PROT_NONE",
           Registered ? "registered memory" : "nothing registered");
    struct SplitRun R;
    memset(&R, 0, sizeof(R));
    R.Context = MakeContext(1, 0, NULL, NULL, NULL);
    uint8_t* Code = MapCode(R.Context, Programs[Which].Program, Programs[Which].Size, Registered);
    uint8_t* Data = MapData(R.Context, 0x8000, PROT_READ | PROT_WRITE, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, Registered);
    (void)*(volatile uint8_t*)(Data + 0x4000);
    SetProt(R.Context, Data + 0x4000, 0x4000, ReadOnly ? PROT_READ : PROT_NONE, ReadOnly ? FEXMAC_PROT_READ : FEXMAC_PROT_NONE, Registered);
    R.Code = Addr(Code);
    R.LockedAt = Addr(Code) + Programs[Which].LockedAt;
    R.Word = Addr(Data) + 0x4000 - 4;
    if (Which == 2) {
      R.Rsi = R.Word;
      R.Rdi = Addr(Data) + 0x100;
    } else {
      R.Rdi = R.Word;
    }
    pthread_t Workers[SplitFaultThreads];
    for (int i = 0; i < SplitFaultThreads; ++i) {
      pthread_create(&Workers[i], NULL, SplitFaultWorker, &R);
    }
    const double Start = Now();
    while (atomic_load(&R.Faulted) < SplitFaultThreads && atomic_load(&R.Bad) == 0 && Now() - Start < 10) {
    }
    SetProt(R.Context, Data + 0x4000, 0x4000, PROT_READ | PROT_WRITE, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE, Registered);
    atomic_store(&R.Fixed, 1);
    for (int i = 0; i < SplitFaultThreads; ++i) {
      pthread_join(Workers[i], NULL);
    }
    uint64_t Total;
    memcpy(&Total, Which == 2 ? Data + 0x100 : Data + 0x4000 - 4, sizeof(Total));
    const uint64_t Want = (uint64_t)SplitFaultThreads * SplitFaultIterations * (Which == 2 ? 1 : 0x100000001ULL);
    Check(atomic_load(&R.Faulted) == SplitFaultThreads && atomic_load(&R.Bad) == 0,
          "every thread stopped once with a page fault in the word at the locked instruction (or the load before it) (%d faults, %d bad)",
          atomic_load(&R.Faulted), atomic_load(&R.Bad));
    Check(atomic_load(&R.Halted) == SplitFaultThreads && Total == Want, "after the fix every thread finished; total %" PRIu64 " of %" PRIu64,
          Total, Want);
    fexmac_context_destroy(R.Context);
  }
  /* The split-lock lock is free: another round on accessible memory completes. */
  struct SplitRun R;
  memset(&R, 0, sizeof(R));
  R.Context = MakeContext(1, 0, NULL, NULL, NULL);
  uint8_t* Code = MapCode(R.Context, SplitXadd, sizeof(SplitXadd), 0);
  uint8_t* Data = MapGuestMemory(0x8000);
  R.Code = Addr(Code);
  R.Rdi = Addr(Data) + 0x4000 - 4;
  atomic_store(&R.Fixed, 1);
  pthread_t Workers[SplitFaultThreads];
  for (int i = 0; i < SplitFaultThreads; ++i) {
    pthread_create(&Workers[i], NULL, SplitFaultWorker, &R);
  }
  for (int i = 0; i < SplitFaultThreads; ++i) {
    pthread_join(Workers[i], NULL);
  }
  uint64_t Total;
  memcpy(&Total, Data + 0x4000 - 4, sizeof(Total));
  Check(Total == (uint64_t)SplitFaultThreads * SplitFaultIterations * 0x100000001ULL, "a later round of split locked operations completes (no lock left held)");
  fexmac_context_destroy(R.Context);
}

/* The split word on a host page holding the threads' translated code: every emulation
 * meets write protection being added and removed by the other threads. */
static void TestSplitLockOnCodePage(void) {
  fexmac_install_fault_handlers();
  struct SplitRun R;
  memset(&R, 0, sizeof(R));
  R.Context = MakeContext(1, 0, "mtrack", NULL, NULL);
  uint8_t* Code = MapCode(R.Context, SplitXadd, sizeof(SplitXadd), 1);
  R.Code = Addr(Code);
  R.Rdi = Addr(Code) + 0x2000 - 4; /* crosses 16 bytes, same host page as the code */
  atomic_store(&R.Fixed, 1);
  const int NT = SplitFaultThreads;
  pthread_t Workers[SplitFaultThreads];
  for (int i = 0; i < NT; ++i) {
    pthread_create(&Workers[i], NULL, SplitFaultWorker, &R);
  }
  for (int i = 0; i < NT; ++i) {
    pthread_join(Workers[i], NULL);
  }
  uint64_t Total;
  memcpy(&Total, (void*)(uintptr_t)R.Rdi, sizeof(Total));
  const uint64_t Want = (uint64_t)NT * SplitFaultIterations * 0x100000001ULL;
  printf("  %" PRIu64 " code writes handled\n", fexmac_memory_code_write_count(R.Context));
  Check(atomic_load(&R.Bad) == 0 && atomic_load(&R.Halted) == NT && Total == Want,
        "4 threads, lock xadd across 16 bytes on their code page: total %" PRIu64 " of %" PRIu64 " (%d unexpected stops, %d halted)", Total,
        Want, atomic_load(&R.Bad), atomic_load(&R.Halted));
  fexmac_context_destroy(R.Context);
}

/* ---------------------------------------------------------------------------------
 * Races found by the package D review
 * --------------------------------------------------------------------------------- */

/* One thread spins (jmp self) and is stopped 3,000 times while four other guest threads
 * write to code on their own pages (every write discards code and makes the spinner
 * resync) and two requesters ask at once: every request is answered, none lost. */
struct StormThread {
  fexmac_thread* Thread;
  uint64_t Rip;
  int Count;
};
static atomic_long StormAnswered, StormOther;
static atomic_int StormDone;
static void* StormRunner(void* Argument) {
  struct StormThread* T = Argument;
  while (!atomic_load(&StormDone)) {
    const fexmac_stop Stop = Run(T->Thread);
    if (T->Count) {
      if (Stop.reason == FEXMAC_STOP_REQUESTED) {
        atomic_fetch_add(&StormAnswered, 1);
      } else {
        atomic_fetch_add(&StormOther, 1);
      }
    }
  }
  return NULL;
}

static void TestStopResyncStorm(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 0, "mtrack", NULL, NULL);
  uint8_t* Base = MapGuestMemory(0x40000);
  static const uint8_t SpinSelf[] = {0xEB, 0xFE};                                                   /* jmp self */
  static const uint8_t CodeWriter[] = {0x48, 0x83, 0x05, 0x10, 0x00, 0x00, 0x00, 0x01, 0xEB, 0xF6}; /* 1: add qword [rip+16], 1; jmp 1b */
  memcpy(Base, SpinSelf, sizeof(SpinSelf));
  enum { Writers = 4, Requests = 3000 };
  for (int w = 0; w < Writers; ++w) {
    memcpy(Base + 0x4000 * (w + 1), CodeWriter, sizeof(CodeWriter));
  }
  fexmac_memory_map(Context, Addr(Base), 0x40000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE | FEXMAC_PROT_EXEC);
  struct StormThread SpinT = {CreateThreadAt(Context, Addr(Base), 0), Addr(Base), 1};
  struct StormThread W[Writers];
  pthread_t Threads[Writers + 1];
  pthread_create(&Threads[0], NULL, StormRunner, &SpinT);
  for (int w = 0; w < Writers; ++w) {
    W[w] = (struct StormThread) {CreateThreadAt(Context, Addr(Base) + 0x4000 * (w + 1), 0), 0, 0};
    pthread_create(&Threads[w + 1], NULL, StormRunner, &W[w]);
  }
  usleep(50000);
  long Lost = 0;
  for (int i = 0; i < Requests; ++i) {
    const long Before = atomic_load(&StormAnswered);
    double Start = Now();
    fexmac_thread_request_stop(SpinT.Thread);
    fexmac_thread_request_stop(SpinT.Thread);
    while (atomic_load(&StormAnswered) == Before) {
      if (Now() - Start > 1.0) {
        ++Lost;
        fexmac_thread_request_stop(SpinT.Thread);
        Start = Now();
      }
    }
  }
  const uint64_t Writes = fexmac_memory_code_write_count(Context);
  atomic_store(&StormDone, 1);
  fexmac_thread_request_stop(SpinT.Thread);
  for (int w = 0; w < Writers; ++w) {
    fexmac_thread_request_stop(W[w].Thread);
  }
  for (int i = 0; i < Writers + 1; ++i) {
    pthread_join(Threads[i], NULL);
  }
  printf("  %d requests, %ld answered, %" PRIu64 " code writes meanwhile\n", Requests, atomic_load(&StormAnswered), Writes);
  Check(Lost == 0, "no request lost (%ld needed a second request after 1 s)", Lost);
  Check(atomic_load(&StormOther) == 0 && Writes > 1000, "the spinner only ever stopped for requests, under a storm of code writes");
  fexmac_thread_destroy(SpinT.Thread);
  for (int w = 0; w < Writers; ++w) {
    fexmac_thread_destroy(W[w].Thread);
  }
  fexmac_context_destroy(Context);
}

/* Four threads run misaligned lock xadd on a word whose page the main thread unmaps and
 * remaps (fexmac_memory_unmap then a replacing PROT_NONE mmap, mmap then fexmac_memory_map) for 3 seconds:
 * the threads see page faults, the process survives. Died with SIGSEGV within seconds
 * when fexmac_memory_unmap left the host page accessible. */
static fexmac_context* UnmapContext;
static uint8_t* UnmapBase;
static atomic_int UnmapDone;
static atomic_long UnmapFaults;
static fexmac_thread* _Atomic UnmapThreads[4];
static void* UnmapGuest(void* Argument) {
  const long i = (long)Argument;
  static const uint8_t P[] = {0xF0, 0x49, 0x0F, 0xC1, 0x5F, 0x0C, 0x48, 0xFF, 0xC1, 0xEB, 0xF5}; /* 1: lock xadd [r15+12], rbx; inc rcx; jmp 1b */
  (void)P;
  fexmac_thread* T = CreateThreadAt(UnmapContext, Addr(UnmapBase), Addr(UnmapBase) + 0x80000 - i * 0x1000);
  fexmac_x86_state S = GetState(T);
  S.gpr[15] = Addr(UnmapBase) + 0x40000 + i * 64;
  S.gpr[RBX] = 1;
  SetState(T, &S);
  atomic_store(&UnmapThreads[i], T);
  while (!atomic_load(&UnmapDone)) {
    const fexmac_stop Stop = Run(T);
    if (Stop.reason == FEXMAC_STOP_GUEST_FAULT) {
      atomic_fetch_add(&UnmapFaults, 1);
    }
  }
  fexmac_thread_destroy(T);
  return NULL;
}

static void TestSplitLockUnmapRace(void) {
  fexmac_install_fault_handlers();
  UnmapContext = MakeContext(1, 0, NULL, NULL, NULL);
  UnmapBase = MapGuestMemory(0x100000);
  static const uint8_t P[] = {0xF0, 0x49, 0x0F, 0xC1, 0x5F, 0x0C, 0x48, 0xFF, 0xC1, 0xEB, 0xF5};
  memcpy(UnmapBase, P, sizeof(P));
  fexmac_memory_map(UnmapContext, Addr(UnmapBase), 0x40000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE | FEXMAC_PROT_EXEC);
  fexmac_memory_map(UnmapContext, Addr(UnmapBase) + 0x40000, 0xC0000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE);
  pthread_t G[4];
  for (long i = 0; i < 4; ++i) {
    pthread_create(&G[i], NULL, UnmapGuest, (void*)i);
  }
  const double Start = Now();
  long Rounds = 0;
  while (Now() - Start < 3.0) {
    /* As an embedder's munmap, but replacing the mapping instead of removing it: a hole
     * would let another thread's allocator map something there, which the MAP_FIXED
     * below would then destroy. */
    fexmac_memory_unmap(UnmapContext, Addr(UnmapBase) + 0x40000, 0x4000);
    mmap(UnmapBase + 0x40000, 0x4000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    usleep(10);
    mmap(UnmapBase + 0x40000, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    fexmac_memory_map(UnmapContext, Addr(UnmapBase) + 0x40000, 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE);
    usleep(10);
    ++Rounds;
  }
  atomic_store(&UnmapDone, 1);
  for (int i = 0; i < 4; ++i) {
    /* The guest loops until a fault or a stop: ask each to stop. */
    while (!atomic_load(&UnmapThreads[i])) {
    }
    fexmac_thread_request_stop(atomic_load(&UnmapThreads[i]));
    pthread_join(G[i], NULL);
  }
  printf("  %ld unmap/map rounds, %ld guest page faults\n", Rounds, atomic_load(&UnmapFaults));
  Check(Rounds > 100, "the process survived %ld rounds of unmap racing the split-lock emulation", Rounds);
  fexmac_context_destroy(UnmapContext);
}

/* fexmac_thread_request_stop on a destroyed thread returns INVALID_HANDLE and touches
 * nothing; requests racing with destroys from another thread are safe. */
static atomic_int RaceStop;
static fexmac_thread* volatile RaceTarget;
static void* StopRequester(void* Argument) {
  (void)Argument;
  while (!atomic_load(&RaceStop)) {
    fexmac_thread* T = RaceTarget;
    if (T) {
      fexmac_thread_request_stop(T);
    }
  }
  return NULL;
}

static void TestStopAfterDestroy(void) {
  fexmac_context* Context = MakeContext(1, 0, NULL, NULL, NULL);
  fexmac_thread* Thread = CreateThreadAt(Context, 0x10000, 0);
  CheckResult(fexmac_thread_destroy(Thread), FEXMAC_OK, "destroy");
  CheckResult(fexmac_thread_request_stop(Thread), FEXMAC_ERROR_INVALID_HANDLE, "stop request for a destroyed thread");
  pthread_t Requester;
  pthread_create(&Requester, NULL, StopRequester, NULL);
  for (int i = 0; i < 2000; ++i) {
    fexmac_thread* T = CreateThreadAt(Context, 0x10000, 0);
    RaceTarget = T;
    usleep(i % 3 == 0 ? 0 : 20);
    fexmac_thread_destroy(T);
  }
  atomic_store(&RaceStop, 1);
  pthread_join(Requester, NULL);
  Check(1, "2,000 destroys raced with stop requests from another thread");
  CheckResult(fexmac_context_destroy(Context), FEXMAC_OK, "context destroy frees the handles");
}

/* A JIT-style dual mapping: the guest runs code through an executable alias and writes it
 * through a writable alias of the same memory. Write protection only covers the page the
 * code was translated from, so the write through the other alias is not seen; the
 * embedder (which made the alias) must call fexmac_invalidate for the executable alias,
 * as documented. With that, the new code runs. */
static void TestSMCAliasNeedsInvalidate(void) {
  fexmac_install_fault_handlers();
  fexmac_context* Context = MakeContext(1, 0, "mtrack", NULL, NULL);
  char Name[64];
  snprintf(Name, sizeof(Name), "/fexmac-alias-%d", (int)getpid());
  const int Fd = shm_open(Name, O_RDWR | O_CREAT | O_EXCL, 0600);
  shm_unlink(Name);
  if (Fd < 0 || ftruncate(Fd, 0x4000) != 0) {
    Check(0, "shared memory");
    return;
  }
  uint8_t* Exec = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  uint8_t* Write = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  static const uint8_t Old[] = {0xB8, 1, 0, 0, 0, 0xF4}; /* mov eax, 1 ; hlt */
  memcpy(Exec, Old, sizeof(Old));
  fexmac_memory_map(Context, Addr(Exec), 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_EXEC);
  fexmac_memory_map(Context, Addr(Write), 0x4000, FEXMAC_PROT_READ | FEXMAC_PROT_WRITE);
  /* The writer: mov byte [rdi+1], 42 ; hlt */
  static const uint8_t Patch[] = {0xC6, 0x47, 0x01, 0x2A, 0xF4};
  uint8_t* PatchCode = MapCode(Context, Patch, sizeof(Patch), 1);
  fexmac_thread* Thread = CreateThreadAt(Context, Addr(Exec), 0);
  fexmac_stop Stop = Run(Thread);
  fexmac_x86_state State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 1, "the old code ran through the executable alias");
  State.rip = Addr(PatchCode);
  State.gpr[RDI] = Addr(Write);
  SetState(Thread, &State);
  Stop = Run(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && Exec[1] == 42, "the guest wrote the code through the writable alias");
  State = GetState(Thread);
  State.rip = Addr(Exec);
  SetState(Thread, &State);
  Stop = Run(Thread);
  State = GetState(Thread);
  printf("  without fexmac_invalidate the executable alias ran eax %" PRIu64 " (1: stale)\n", State.gpr[RAX]);
  CheckResult(fexmac_invalidate(Context, Addr(Exec), 0x4000), FEXMAC_OK, "the embedder invalidates the executable alias");
  State.rip = Addr(Exec);
  SetState(Thread, &State);
  Stop = Run(Thread);
  State = GetState(Thread);
  Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 42, "after fexmac_invalidate the new code runs (eax %" PRIu64 ")", State.gpr[RAX]);
  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
  close(Fd);
}

/* The self-modifying-code defaults (no SMCCHECKS option): with no memory registered the
 * patch in SelfPatch's block is not seen (eax -1: the stale instruction ran), as before
 * guest memory registration existed; with the code registered it is (eax 32), by the
 * per-instruction check, not by write protection (no code write handled). */
static void TestSMCDefaultPolicy(void) {
  fexmac_install_fault_handlers();
  for (int Registered = 0; Registered < 2; ++Registered) {
    fexmac_context* Context = CreateContext(NULL, NULL);
    uint8_t* Code = MapCode(Context, SelfPatch, sizeof(SelfPatch), Registered);
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    const fexmac_stop Stop = Run(Thread);
    const fexmac_x86_state State = GetState(Thread);
    if (Registered) {
      Check(Stop.reason == FEXMAC_STOP_HALT && State.gpr[RAX] == 32 && fexmac_memory_code_write_count(Context) == 0,
            "registered memory, default: the write is seen by the full check (eax 0x%" PRIx64 ", %" PRIu64 " code writes)", State.gpr[RAX],
            fexmac_memory_code_write_count(Context));
    } else {
      Check(Stop.reason == FEXMAC_STOP_HALT && (State.gpr[RAX] & 0xFFFFFFFF) == 0xFFFFFFFF,
            "nothing registered, default: nothing detected, as before (eax 0x%" PRIx64 ")", State.gpr[RAX]);
    }
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
}

/* Stop requests during long rep movs/stos: every stop is at the rep instruction with
 * rcx, rsi and rdi describing exactly the elements done; running again continues; the
 * final memory is what x86 gives, including a forward overlapping byte copy (the pattern
 * replicates) and a backward one. */
struct RepRun {
  const char* Name;
  uint8_t Program[4];
  size_t ProgramSize;
  size_t Element;
  int Backward;
  int Copy;
  long Overlap; /* rdi - rsi for copies */
};

static void* RequestEvery100us(void* Argument) {
  fexmac_thread* T = Argument;
  while (!atomic_load(&StormDone)) {
    usleep(100);
    fexmac_thread_request_stop(T);
  }
  return NULL;
}

static void TestStopRepMovs(void) {
  fexmac_install_fault_handlers();
  static const struct RepRun Runs[] = {
    {"rep movsb, 32MB apart", {0xF3, 0xA4, 0xF4}, 3, 1, 0, 1, 32 << 20},
    {"rep movsq, 32MB apart", {0xF3, 0x48, 0xA5, 0xF4}, 4, 8, 0, 1, 32 << 20},
    {"rep stosq", {0xF3, 0x48, 0xAB, 0xF4}, 4, 8, 0, 0, 0},
    {"rep movsb overlapping forward by 1 (replicates)", {0xF3, 0xA4, 0xF4}, 3, 1, 0, 1, 1},
    {"rep movsb overlapping forward by 24", {0xF3, 0xA4, 0xF4}, 3, 1, 0, 1, 24},
    {"std; rep movsb overlapping backward by 3", {0xF3, 0xA4, 0xF4}, 3, 1, 1, 1, -3},
  };
  const size_t Bytes = 32 << 20;
  uint8_t* Area = MapGuestMemory(Bytes * 3);
  uint8_t* Expect = malloc(Bytes * 3);
  for (size_t r = 0; r < sizeof(Runs) / sizeof(Runs[0]); ++r) {
    const struct RepRun* Run_ = &Runs[r];
    fexmac_context* Context = MakeContext(1, (int)(r & 1), "none", NULL, NULL);
    uint8_t* Code = MapCode(Context, Run_->Program, Run_->ProgramSize, 0);
    for (size_t i = 0; i < Bytes * 3; ++i) {
      Area[i] = (uint8_t)(i * 131 + 7);
    }
    memcpy(Expect, Area, Bytes * 3);
    const size_t Count = Bytes / Run_->Element;
    uint8_t* Dst = Area + Bytes;
    uint8_t* Src = Dst - Run_->Overlap;
    if (Run_->Backward) {
      /* rdi and rsi name the last element; the copy runs down. */
      Dst = Area + 2 * Bytes - Run_->Element;
      Src = Dst - Run_->Overlap;
    }
    /* The x86 result, element by element in order. */
    for (size_t i = 0; i < Count; ++i) {
      const ptrdiff_t Step = Run_->Backward ? -(ptrdiff_t)(i * Run_->Element) : (ptrdiff_t)(i * Run_->Element);
      uint8_t* E = Expect + (Dst - Area) + Step;
      if (Run_->Copy) {
        memmove(E, Expect + (Src - Area) + Step, Run_->Element);
      } else {
        memset(E, 0x5A, Run_->Element);
      }
    }
    fexmac_thread* Thread = CreateThreadAt(Context, Addr(Code), 0);
    fexmac_x86_state State = GetState(Thread);
    State.gpr[RCX] = Count;
    State.gpr[RDI] = Addr(Dst);
    State.gpr[RSI] = Addr(Src);
    State.gpr[RAX] = 0x5A5A5A5A5A5A5A5AULL;
    State.rflags = Run_->Backward ? 0x602 : 0x202;
    SetState(Thread, &State);
    atomic_store(&StormDone, 0);
    pthread_t Requester;
    pthread_create(&Requester, NULL, RequestEvery100us, Thread);
    int Stops = 0, Bad = 0;
    for (;;) {
      const fexmac_stop Stop = Run(Thread);
      State = GetState(Thread);
      if (Stop.reason == FEXMAC_STOP_HALT) {
        break;
      }
      if (Stop.reason != FEXMAC_STOP_REQUESTED) {
        ++Bad;
        break;
      }
      const uint64_t Done = Count - State.gpr[RCX];
      const int64_t Moved = Run_->Backward ? -(int64_t)(Done * Run_->Element) : (int64_t)(Done * Run_->Element);
      if ((State.rip != Addr(Code) && !(State.gpr[RCX] == Count && State.rip == Addr(Code))) || State.gpr[RDI] != Addr(Dst) + Moved ||
          (Run_->Copy && State.gpr[RSI] != Addr(Src) + Moved)) {
        ++Bad;
      }
      ++Stops;
    }
    atomic_store(&StormDone, 1);
    pthread_join(Requester, NULL);
    printf("  %s: %d stops part-way\n", Run_->Name, Stops);
    Check(Bad == 0, "%s: every stop at the rep instruction with rcx, rsi, rdi exact (%d bad)", Run_->Name, Bad);
    Check(Stops > 3, "%s: stopped part-way (%d times)", Run_->Name, Stops);
    Check(State.gpr[RCX] == 0 && memcmp(Area, Expect, Bytes * 3) == 0, "%s: after all the resumes, memory is x86's result", Run_->Name);
    fexmac_thread_destroy(Thread);
    fexmac_context_destroy(Context);
  }
  free(Expect);
}

const struct Subtest FaultSubtests[] = {
  {"fault-mid-block", TestFaultMidBlock, 0, NULL, "fexmac:", 0},
  {"fault-flags", TestFaultFlags, 0, NULL, "fexmac:", 0},
  {"fault-store-loop", TestFaultStoreLoop, 0, NULL, "fexmac:", 0},
  {"fault-kinds", TestFaultKinds, 0, NULL, "fexmac:", 0},
  {"fault-rep", TestFaultRep, 0, NULL, "fexmac:", 0},
  {"fault-vector", TestFaultVector, 0, NULL, "fexmac:", 0},
  {"fault-stack", TestFaultStack, 0, NULL, "fexmac:", 0},
  {"fault-fetch", TestFaultFetch, 0, NULL, "fexmac:", 0},
  {"memory-arguments", TestMemoryArguments, 0, NULL, NULL, 0},
  {"stop-latency", TestStopLatency, 0, NULL, "fexmac:", 0},
  {"stop-rep-movs", TestStopRepMovs, 0, NULL, "fexmac:", 0},
  {"stop-rep", TestStopRep, 0, NULL, "fexmac:", 0},
  {"stop-not-running", TestStopNotRunning, 0, NULL, "fexmac:", 0},
  {"stop-during-callback", TestStopDuringCallback, 0, NULL, "fexmac:", 0},
  {"stop-many-threads", TestStopManyThreads, 0, NULL, "fexmac:", 0},
  {"smc-patch-loop", TestSMCPatchLoop, 0, NULL, "fexmac:", 0},
  {"smc-self-patch", TestSMCSelfPatch, 0, NULL, "fexmac:", 0},
  {"smc-data-on-code-page", TestSMCDataOnCodePage, 0, NULL, "fexmac:", 0},
  {"smc-misaligned-atomic", TestSMCMisalignedAtomic, 0, NULL, "fexmac:", 0},
  {"smc-cross-thread", TestSMCCrossThread, 0, NULL, "fexmac:", 0},
  {"split-lock-faults", TestSplitLockFaults, 0, NULL, "fexmac:", 0},
  {"split-lock-on-code-page", TestSplitLockOnCodePage, 0, NULL, "fexmac:", 0},
  {"stop-resync-storm", TestStopResyncStorm, 0, NULL, "fexmac:", 0},
  {"split-lock-unmap-race", TestSplitLockUnmapRace, 0, NULL, "fexmac:", 0},
  {"stop-after-destroy", TestStopAfterDestroy, 0, NULL, "fexmac:", 0},
  {"smc-default-policy", TestSMCDefaultPolicy, 0, NULL, "fexmac:", 0},
  {"smc-alias-needs-invalidate", TestSMCAliasNeedsInvalidate, 0, NULL, "fexmac:", 0},
  {"smc-embedder-write", TestSMCEmbedderWrite, 0, NULL, "fexmac:", 2},
  {"smc-embedder-write-unbracketed-dies", TestSMCEmbedderWriteUnbracketed, SIGBUS, "fexmac_memory_write_begin", NULL, 1},
};
const size_t FaultSubtestCount = sizeof(FaultSubtests) / sizeof(FaultSubtests[0]);
