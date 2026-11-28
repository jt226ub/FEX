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

/* Called (NULL to stop) in fexmac_handle_fault just before the core handles a
 * misaligned access (and may take its patch or split-lock lock), with the signal mask that
 * handling runs with. */
FEXMAC_EXPORT void fexmac_testing_set_misaligned_hook(void (*hook)(void));

/* Called inside fexmac_invalidate while the core's invalidation lock is held (NULL to
 * stop), to observe the signal mask there. */
FEXMAC_EXPORT void fexmac_testing_set_invalidation_hook(void (*hook)(void));

/* The inaccessible guard pages of a thread: the low and high guard of its call/return
 * stack, and the page after its translator scratch buffer while a translation is being
 * emitted (0 otherwise: the core disarms it when a block is emitted). */
FEXMAC_EXPORT void fexmac_testing_guard_pages(fexmac_thread* thread, uint64_t* callret_low, uint64_t* callret_high, uint64_t* scratch);

/* While page is not NULL, the library makes that host page inaccessible (mprotect
 * PROT_NONE) right after checking a misaligned atomic's memory and before the core
 * emulates it: the race with a raw mprotect by the embedder that the contract excludes. */
FEXMAC_EXPORT void fexmac_testing_protect_before_emulation(void* page);

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
/* Bytes used and usable in the code buffer the thread writes to; setting the bytes used
 * (a multiple of 16, at most the usable size), as if that much code were there. */
FEXMAC_EXPORT void fexmac_testing_code_buffer_use(fexmac_thread* thread, uint64_t* used, uint64_t* usable);
FEXMAC_EXPORT void fexmac_testing_set_code_buffer_used(fexmac_thread* thread, uint64_t used);
/* The next `count` code buffers the context tries to make fail, as if mmap had. */
FEXMAC_EXPORT fexmac_result fexmac_testing_fail_code_buffers(fexmac_context* context, int count);
FEXMAC_EXPORT void fexmac_testing_overrun_code_buffer(fexmac_thread* thread);

/* Logs a core message at info level, outside any fault handler. */
FEXMAC_EXPORT void fexmac_testing_log(const char* message);

/* Code generation state that the configuration does not carry, for the code cache tests:
 * a thunk trampoline (a custom IR handler) at `entry` jumping to `target`; the host
 * claiming hardware x86 memory ordering; instructions forced to TSO. */
FEXMAC_EXPORT fexmac_result fexmac_testing_add_thunk_trampoline(fexmac_context* context, uint64_t entry, uint64_t target);
FEXMAC_EXPORT fexmac_result fexmac_testing_set_hardware_tso(fexmac_context* context, int supported);

/* The RCPC levels the context's host features record (FEAT_LRCPC, FEAT_LRCPC2,
 * FEAT_LRCPC3), each 0 or 1. LRCPC3 is recorded only; no code uses it. */
FEXMAC_EXPORT fexmac_result fexmac_testing_rcpc_features(fexmac_context* context, int* rcpc, int* rcpc2, int* rcpc3);
FEXMAC_EXPORT fexmac_result fexmac_testing_force_tso(fexmac_context* context, uint64_t begin, uint64_t end, uint64_t instruction);

/* Called (NULL to stop) where the core reports guest code it starts executing, which is
 * after a block is compiled and before it is stored in the code cache. */
FEXMAC_EXPORT void fexmac_testing_set_mark_executable_hook(void (*hook)(uint64_t start, uint64_t length, void* user_data), void* user_data);

/* The ordering switch (option TSODEFERRED): the TSO emulation the core translates with
 * now (bit 0 scalar, 1 vector, 2 string instructions), and counts since the context was
 * created: stats[0] switches, [1] threads that acknowledged at a resync, [2] at a system
 * call or the end of a run, [3] at the start of a run, [4] waits by
 * fexmac_thread_run or fexmac_context_mark_memory_shared for acknowledgements. */
FEXMAC_EXPORT fexmac_result fexmac_testing_ordering_state(fexmac_context* context, uint32_t* effective, uint64_t stats[5]);

/* Deliberately wrong ordering switches, to show the switch tests can fail (0: correct).
 * NO_WAIT: running threads are neither asked to resync nor waited for, so a new thread
 * may run while another keeps running code made without ordering. NO_DISCARD: the
 * translations are kept. KEEP_CALLRET: call/return stacks are not cleared. */
#define FEXMAC_TESTING_ORDERING_NO_WAIT 1u
#define FEXMAC_TESTING_ORDERING_NO_DISCARD 2u
#define FEXMAC_TESTING_ORDERING_KEEP_CALLRET 4u
FEXMAC_EXPORT void fexmac_testing_break_ordering_switch(uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* FEXMAC_TESTING_H */
