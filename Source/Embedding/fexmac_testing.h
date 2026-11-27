/* SPDX-License-Identifier: MIT */
/*
 * Test-only hooks of the fexmac library. They exist only in the fexmac_testing build of
 * the library (compiled with FEXMAC_TESTING), never in the production static library,
 * standalone archive or dylib.
 */
#ifndef FEXMAC_TESTING_H
#define FEXMAC_TESTING_H

#include "fexmac.h"

#ifdef __cplusplus
extern "C" {
#endif

/* While enabled, the library's own SIGBUS/SIGSEGV handler adds 1 to *fired (if not
 * NULL) and then deliberately writes to an inaccessible page on entry, standing in for a
 * bug inside the handler. */
FEXMAC_EXPORT void fexmac_testing_fault_inside_handler(int enable, volatile uint32_t* fired);

/* While enabled, the library's handler logs a core message ("test message from the
 * fault handler") while it handles a fault, to show that such messages never reach the
 * log callback. */
FEXMAC_EXPORT void fexmac_testing_log_from_fault_handler(int enable);

/* Called inside fexmac_invalidate while the core's invalidation lock is held (NULL to
 * stop), to observe the signal mask there. */
FEXMAC_EXPORT void fexmac_testing_set_invalidation_hook(void (*hook)(void));

/* The inaccessible guard pages of a thread: the low and high guard of its call/return
 * stack, and the page after its translator scratch buffer (0 before its first
 * translation). */
FEXMAC_EXPORT void fexmac_testing_guard_pages(fexmac_thread* thread, uint64_t* callret_low, uint64_t* callret_high, uint64_t* scratch);

/* While page is not NULL, the library makes that host page inaccessible (mprotect
 * PROT_NONE) right after checking a misaligned atomic's memory and before the core
 * emulates it: the race with a raw mprotect by the embedder that the contract excludes. */
FEXMAC_EXPORT void fexmac_testing_protect_before_emulation(void* page);

/* Logs a core message at info level, outside any fault handler. */
FEXMAC_EXPORT void fexmac_testing_log(const char* message);

#ifdef __cplusplus
}
#endif

#endif /* FEXMAC_TESTING_H */
