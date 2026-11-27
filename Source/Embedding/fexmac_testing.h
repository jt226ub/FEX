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
 * stack, and the page after its translator scratch buffer while a translation is being
 * emitted (0 otherwise: the core disarms it when a block is emitted). */
FEXMAC_EXPORT void fexmac_testing_guard_pages(fexmac_thread* thread, uint64_t* callret_low, uint64_t* callret_high, uint64_t* scratch);

/* The translator's scratch buffer on a thread: how many times a translation restarted
 * because the buffer was too small (in total since the thread was created), and the
 * largest buffer, in bytes, a translation may ask for before the core stops trying and
 * reports the block untranslatable (0: the core's default). */
FEXMAC_EXPORT uint64_t fexmac_testing_scratch_overflows(fexmac_thread* thread);
FEXMAC_EXPORT void fexmac_testing_set_scratch_limit(fexmac_thread* thread, uint64_t limit);
/* The scratch-buffer bytes per IR node the translator starts each translation with
 * (0: the core's default, 24). A small value makes translations overflow many times. */
FEXMAC_EXPORT void fexmac_testing_set_scratch_start(fexmac_thread* thread, uint32_t bytes_per_node);

/* The guard page after the code buffer the thread is writing to (0 if none), and a
 * deliberate overrun of that buffer: with the calling thread switched to writable, as
 * the translator is when it copies code in, a 64-byte copy starting 32 bytes before the
 * buffer's end. Call it on the thread's own host thread while it runs (from a system
 * call callback), as the core would; it does not return. */
FEXMAC_EXPORT uint64_t fexmac_testing_code_buffer_guard(fexmac_thread* thread);
FEXMAC_EXPORT void fexmac_testing_overrun_code_buffer(fexmac_thread* thread);

/* Logs a core message at info level, outside any fault handler. */
FEXMAC_EXPORT void fexmac_testing_log(const char* message);

/* Code generation state that the configuration does not carry, for the code cache tests:
 * a thunk trampoline (a custom IR handler) at `entry` jumping to `target`; the host
 * claiming hardware x86 memory ordering; instructions forced to TSO. */
FEXMAC_EXPORT fexmac_result fexmac_testing_add_thunk_trampoline(fexmac_context* context, uint64_t entry, uint64_t target);
FEXMAC_EXPORT fexmac_result fexmac_testing_set_hardware_tso(fexmac_context* context, int supported);
FEXMAC_EXPORT fexmac_result fexmac_testing_force_tso(fexmac_context* context, uint64_t begin, uint64_t end, uint64_t instruction);

/* Called (NULL to stop) where the core reports guest code it starts executing, which is
 * after a block is compiled and before it is stored in the code cache. */
FEXMAC_EXPORT void fexmac_testing_set_mark_executable_hook(void (*hook)(uint64_t start, uint64_t length, void* user_data), void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* FEXMAC_TESTING_H */
