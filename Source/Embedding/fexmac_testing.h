/* SPDX-License-Identifier: MIT */
/*
 * Test-only hooks of the fexmac library. Not part of the interface in fexmac.h: no
 * stability promise, and no use outside the library's own tests.
 */
#ifndef FEXMAC_TESTING_H
#define FEXMAC_TESTING_H

#include "fexmac.h"

#ifdef __cplusplus
extern "C" {
#endif

/* While enabled, the library's own SIGBUS/SIGSEGV handler deliberately writes to an
 * inaccessible page on entry, standing in for a bug inside the handler. Used to show
 * that such a bug crashes the process instead of hanging it. */
FEXMAC_EXPORT void fexmac_testing_fault_inside_handler(int enable);

#ifdef __cplusplus
}
#endif

#endif /* FEXMAC_TESTING_H */
