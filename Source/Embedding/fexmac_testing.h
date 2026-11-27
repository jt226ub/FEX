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

/* Logs a core message at info level, outside any fault handler. */
FEXMAC_EXPORT void fexmac_testing_log(const char* message);

#ifdef __cplusplus
}
#endif

#endif /* FEXMAC_TESTING_H */
