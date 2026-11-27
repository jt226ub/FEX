/* SPDX-License-Identifier: MIT */
/*
 * Shared by the fexmac C tests: checks, guest memory, and a driver that runs each
 * subtest in its own child process and checks how it ended.
 */
#ifndef FEXMAC_TEST_SUPPORT_H
#define FEXMAC_TEST_SUPPORT_H

#include "fexmac.h"

#include <stddef.h>
#include <stdint.h>

/* Register indexes in fexmac_x86_state.gpr. */
enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7 };

extern int Failures;
extern long PageSize;
/* A page shared between the driver and the child it runs, zeroed before each subtest.
 * A subtest records facts in it that the driver checks after the child ended. */
extern volatile uint32_t* Shared;

void Check(int Condition, const char* Format, ...) __attribute__((format(printf, 2, 3)));
void CheckResult(fexmac_result Got, fexmac_result Expected, const char* What);

uint8_t* MapGuestMemory(size_t Size);
fexmac_context* CreateContext(fexmac_syscall_fn Syscall, void* UserData);
fexmac_context* CreateContextWithOptions(const fexmac_option* Options, size_t Count, fexmac_syscall_fn Syscall, void* UserData);
fexmac_thread* CreateThreadAt(fexmac_context* Context, uint64_t RIP, uint64_t StackPointer);
fexmac_x86_state GetState(fexmac_thread* Thread);
void SetState(fexmac_thread* Thread, const fexmac_x86_state* State);
fexmac_stop Run(fexmac_thread* Thread);

struct Subtest {
  const char* Name;
  void (*Run)(void);
  /* 0: the child must exit 0. -1: it must die by SIGSEGV, SIGBUS or SIGILL
   * (how macOS ends a process whose stack is exhausted while delivering a signal). Otherwise it must
   * die by exactly this signal. */
  int ExpectSignal;
  /* If set, the child's standard error must contain this text. */
  const char* ExpectStderr;
  /* If set, the child's standard error must not contain this text. */
  const char* ForbidStderr;
  /* If non-zero, Shared[0] must have this value when the child ended. */
  uint32_t ExpectShared0;
};

/* Runs every subtest (or the one named in argv[1], in this process). Returns the exit
 * status for main. */
int RunSubtests(int argc, char** argv, const struct Subtest* Subtests, size_t Count, unsigned TimeoutSeconds);

#endif
