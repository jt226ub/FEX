/* SPDX-License-Identifier: MIT */
/*
 * A host signal handler with a bug (it faults), compiled without frame pointers
 * (-fomit-frame-pointer), for fexmac_c_test's chained-handler-faults subtests.
 */
#include "TestSupport.h"

#include <signal.h>

extern uint8_t* FaultingHandlerPage(void);

void FaultingHandlerNoFramePointer(int Signal, siginfo_t* Info, void* Context);
void FaultingHandlerNoFramePointer(int Signal, siginfo_t* Info, void* Context) {
  (void)Signal;
  (void)Info;
  (void)Context;
  Shared[0] = 1;
  *(volatile int*)FaultingHandlerPage() = 1;
}
