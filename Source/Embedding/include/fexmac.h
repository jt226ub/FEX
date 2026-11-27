/* SPDX-License-Identifier: MIT */
/*
 * fexmac: the C interface for programs that embed the FEX x86-64 translator core on
 * macOS (Apple silicon).
 *
 * The core translates and runs x86-64 code. The embedder loads the guest program, maps
 * its memory (guest addresses are host addresses), answers its system calls and decides
 * what to do when it stops. This interface hides the core's C++ types behind opaque
 * handles; it uses only C types and <signal.h>, and works from C and C++.
 *
 * See docs/macOS-embedding-interface.md for the duties the library takes over from the
 * embedder, the signal-handling contract and what is not covered yet.
 *
 * Threading: a context is shared by all threads. A fexmac_thread is one guest thread. It
 * may be run on any host thread, but by one host thread at a time. All functions may be
 * called from any host thread at the same time; each one checks its handles against
 * tables of live handles under a lock before using them, so a handle that was destroyed
 * gives FEXMAC_ERROR_INVALID_HANDLE (as long as its memory has not been reused for a new
 * handle of the same kind) and a destroy that races with a use is either refused
 * (FEXMAC_ERROR_BUSY) or happens wholly before it.
 *
 * Process-wide state: the core's configuration, its log hooks and the fault handlers are
 * process-wide, so at most one context exists at a time.
 *
 * Structures the caller allocates (fexmac_config, fexmac_x86_state, fexmac_stop) start
 * with struct_size, which the caller sets with the *_init functions (or to sizeof of the
 * structure as this header declares it). A library accepts only sizes of layouts it
 * knows and reads or writes nothing otherwise (FEXMAC_ERROR_INVALID_ARGUMENT), so an
 * uninitialised structure is refused, not written past. A later minor version that grows
 * a structure keeps accepting the older sizes; a program built against a newer header
 * than the library it runs with gets FEXMAC_ERROR_INVALID_ARGUMENT for a structure that
 * grew, so check fexmac_version() first.
 *
 * Versions: a program must refuse to run if fexmac_version() >> 16 differs from
 * FEXMAC_VERSION_MAJOR, and should check that the minor part is at least the one it was
 * built with. 2.0 is the first version with struct_size fields; nothing built against the
 * 1.x headers of the development branch is compatible with it.
 */
#ifndef FEXMAC_H
#define FEXMAC_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define FEXMAC_EXPORT __attribute__((visibility("default")))
#else
#define FEXMAC_EXPORT
#endif

/* Version of this header. fexmac_version() returns the version the library was built
 * with; see "Versions" above. */
#define FEXMAC_VERSION_MAJOR 2
#define FEXMAC_VERSION_MINOR 0
#define FEXMAC_VERSION ((FEXMAC_VERSION_MAJOR << 16) | FEXMAC_VERSION_MINOR)

typedef struct fexmac_context fexmac_context;
typedef struct fexmac_thread fexmac_thread;

typedef enum fexmac_result {
  FEXMAC_OK = 0,
  /* A required pointer was NULL, or a struct_size is not a size this library knows. */
  FEXMAC_ERROR_INVALID_ARGUMENT = 1,
  /* The handle is not a live context or thread (never created, or already destroyed). */
  FEXMAC_ERROR_INVALID_HANDLE = 2,
  /* An option is not one the library supports, or its value is not valid for it. */
  FEXMAC_ERROR_BAD_CONFIG = 3,
  /* The object is in use: a context already exists, a context still has threads or a
   * call in progress, or a thread is running. */
  FEXMAC_ERROR_BUSY = 4,
  /* The core could not be initialised. */
  FEXMAC_ERROR_INIT_FAILED = 5,
  FEXMAC_ERROR_OUT_OF_MEMORY = 6,
  /* The request needs something this library version or this context does not provide
   * (for example flushing the code cache of a context that has none). */
  FEXMAC_ERROR_UNSUPPORTED = 7,
  /* The core returned without a recorded stop reason. Indicates a bug in the library. */
  FEXMAC_ERROR_INTERNAL = 8,
} fexmac_result;

/* A short English description of a result, never NULL. */
FEXMAC_EXPORT const char* fexmac_result_string(fexmac_result result);

FEXMAC_EXPORT uint32_t fexmac_version(void);

/* ---------------------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------------------- */

/* One core option. The library accepts a fixed list of options whose effect on macOS it
 * has checked (see docs/macOS-embedding-interface.md, "Options"); any other name is
 * refused. A name is the core's environment-variable name without "FEX_" (MULTIBLOCK)
 * or its configuration-file name (Multiblock). Values: booleans are 0 or 1; numbers are
 * decimal, or hexadecimal with 0x (a leading 0 is not octal: it is refused); named values
 * as listed for each option. Later entries override earlier ones. */
typedef struct fexmac_option {
  const char* name;
  const char* value;
} fexmac_option;

/* What the system call callback wants to happen next. */
typedef enum fexmac_syscall_action {
  /* Continue after the system call instruction, with any guest state the callback set
   * (typically rax, the return value). */
  FEXMAC_SYSCALL_RESUME = 0,
  /* Leave fexmac_thread_run, which returns FEXMAC_STOP_SYSCALL. rip points after the
   * system call instruction, so running again continues the program. */
  FEXMAC_SYSCALL_STOP = 1,
  /* Execute the system call instruction again, at once, with the guest state the
   * callback set: rip is moved back to the instruction (as a kernel does to restart an
   * interrupted call). */
  FEXMAC_SYSCALL_RESTART = 2,
} fexmac_syscall_action;

/* Called on the guest thread's host thread when guest code executes `syscall`. The
 * guest registers are in the thread's state: read them with fexmac_thread_get_state and
 * set the result with fexmac_thread_set_state. rip is already past the instruction.
 *
 * The callback may: read and write this thread's state; create threads (for a guest
 * `clone`) and set their state; destroy other threads that are not running; invalidate;
 * call fexmac_cpuid. It may not run any thread (that would nest a second dispatcher on
 * this host stack) and may not destroy its own thread; both return FEXMAC_ERROR_BUSY. */
typedef fexmac_syscall_action (*fexmac_syscall_fn)(fexmac_thread* thread, void* user_data);

typedef enum fexmac_log_level {
  FEXMAC_LOG_ASSERT = 1,
  FEXMAC_LOG_ERROR = 2,
  FEXMAC_LOG_DEBUG = 3,
  FEXMAC_LOG_INFO = 4,
} fexmac_log_level;

/* Receives the core's log messages. May be called on any host thread, never from inside
 * a signal handler: a message the core produces while the library handles a fault is
 * written to standard error with write(2) instead. It must not call fexmac: the core can
 * log while the library holds its locks, so such calls are refused (FEXMAC_ERROR_BUSY;
 * NULL or no effect for the functions without a result). */
typedef void (*fexmac_log_fn)(fexmac_log_level level, const char* message, void* user_data);

typedef struct fexmac_config {
  /* sizeof(fexmac_config). Set by fexmac_config_init. */
  uint32_t struct_size;
  uint32_t reserved;

  const fexmac_option* options;
  size_t option_count;

  /* NULL: every system call stops fexmac_thread_run with FEXMAC_STOP_SYSCALL. */
  fexmac_syscall_fn syscall;
  void* syscall_user_data;

  /* NULL: messages go to standard error. */
  fexmac_log_fn log;
  void* log_user_data;

  /* Cached translations (the on-disk code cache; docs/macOS-code-cache-design.md).
   * code_cache_enabled 1: translations are looked up in, and saved to, the directory
   * code_cache_directory (NULL: ~/Library/Caches/fex-emu/DiskCache). 0: no cache, and
   * code_cache_directory must be NULL. Other values are refused (FEXMAC_ERROR_BAD_CONFIG).
   *
   * The directory must be owned by this user and writable by nobody else (no group or
   * other write bit, no ACL entry granting write); the library creates it 0700 if it is
   * missing and never changes an existing one. Whatever goes wrong with the cache, the
   * context is still created and runs: fexmac_context_get_counters says what the cache is
   * doing (see fexmac_cache_state). A cache file that may not be trusted is removed and
   * started again (counted in untrusted_files_replaced). A cache file is code that will
   * run: anything running as this user can write it, and the library's integrity checks
   * do not stop that.
   *
   * Size: each cache file is limited to DISKCACHEMAXFILESIZE bytes (default 256MB, set by
   * the library). When the current file is full a new generation starts; the previous one
   * is still read and older ones are deleted. Every process reads the whole index of the
   * generations it uses when it starts (about 40% of the cache file for small blocks),
   * which is what the default bounds.
   *
   * With the cache on, register the guest's memory with fexmac_memory_map: cached code
   * is matched against the guest bytes of executable registered pages only.
   *
   * The core's own options for this (DISKCACHE, DISKCACHEPATH) are refused; these fields
   * are the interface. DISKCACHEMAXFILESIZE and DISKCACHEVALIDATION are accepted options. */
  const char* code_cache_directory;
  uint32_t code_cache_enabled;
  uint32_t reserved2;
} fexmac_config;

/* Fills a config with defaults: no options, no callbacks. */
FEXMAC_EXPORT void fexmac_config_init(fexmac_config* config);

/* 1 if `name` is an option fexmac_context_create accepts, 0 otherwise. */
FEXMAC_EXPORT int fexmac_option_known(const char* name);

/* ---------------------------------------------------------------------------------
 * Context
 * --------------------------------------------------------------------------------- */

/* Creates the translator context: applies the options, detects host features, emits
 * the dispatcher and prepares the shared descriptor table. 64-bit guests only. On
 * failure nothing process-wide is left changed (log hooks included). */
FEXMAC_EXPORT fexmac_result fexmac_context_create(const fexmac_config* config, fexmac_context** out_context);

/* Destroys a context with no threads left and no call in progress on another host
 * thread (FEXMAC_ERROR_BUSY otherwise). */
FEXMAC_EXPORT fexmac_result fexmac_context_destroy(fexmac_context* context);

/* Runs the guest CPUID instruction as the guest would see it. out: eax, ebx, ecx, edx. */
FEXMAC_EXPORT fexmac_result fexmac_cpuid(fexmac_context* context, uint32_t function, uint32_t leaf, uint32_t out[4]);

/* Discards translations of guest code in [address, address + size). Call it after
 * guest code is changed or unmapped from outside guest execution (for example by a
 * loader). Safe from any host thread, including from a system call callback. Takes the
 * core's code invalidation lock and applies to every thread of the context. On a host
 * thread that is not running guest code, asynchronous signals are blocked while the lock
 * is held; SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP and SIGSYS are never blocked. */
FEXMAC_EXPORT fexmac_result fexmac_invalidate(fexmac_context* context, uint64_t address, uint64_t size);

/* ---------------------------------------------------------------------------------
 * The code cache
 *
 * Which guest memory is code comes from the guest memory map (fexmac_memory_map with
 * FEXMAC_PROT_EXEC, below): with the cache on, cached code is matched against the guest
 * bytes of executable registered pages only. (The 2.x development branch had
 * fexmac_add_code_range / fexmac_remove_code_range for this; they are replaced by the
 * memory map, which is the one registry of guest memory.)
 * --------------------------------------------------------------------------------- */

typedef enum fexmac_cache_state {
  /* code_cache_enabled was 0. */
  FEXMAC_CACHE_OFF = 0,
  /* Translations are loaded from and saved to the directory. Saving can still stop for
   * the rest of the run (another process kept the database locked past the wait, or it
   * could not grow): cache_entries_dropped counts what was not saved. */
  FEXMAC_CACHE_ACTIVE = 1,
  /* Loaded but not saved: the child of a fork, or a database that another process with a
   * larger size limit filled past this process's limit (it is used, never emptied). */
  FEXMAC_CACHE_READ_ONLY = 2,
  /* Not used because it may not be trusted: the directory (or the bucket directory in
   * it) is not owned by this user, can be written by others (mode or ACL), or is a
   * symbolic link. The reason is logged. */
  FEXMAC_CACHE_UNTRUSTED = 3,
  /* Not used for another reason: the database stayed locked by another process when it
   * was opened, the directory cannot be created or written (read-only, no space, I/O
   * error), or the core's build cannot be identified. The reason is logged. */
  FEXMAC_CACHE_UNAVAILABLE = 4,
} fexmac_cache_state;

typedef struct fexmac_translation_counters {
  /* sizeof(fexmac_translation_counters). Set by fexmac_translation_counters_init. */
  uint32_t struct_size;
  fexmac_cache_state cache_state;
  /* Since the context was created: */
  uint64_t blocks_compiled;        /* blocks the translator compiled */
  uint64_t blocks_loaded;          /* blocks copied from the code cache instead */
  uint64_t cache_entries_rejected; /* cache entries that failed a check and were ignored */
  uint64_t cache_entries_queued;   /* compiled blocks handed to the cache writer */
  uint64_t cache_entries_dropped;  /* queued entries that were not saved */
  uint32_t untrusted_files_replaced; /* untrusted cache files removed and started again */
  uint32_t damaged_databases_reset;  /* damaged databases emptied and started again */
} fexmac_translation_counters;

/* Sets struct_size and zeroes the rest. */
FEXMAC_EXPORT void fexmac_translation_counters_init(fexmac_translation_counters* counters);

FEXMAC_EXPORT fexmac_result fexmac_context_get_counters(fexmac_context* context, fexmac_translation_counters* counters);

/* Waits until every translation queued for the code cache is written (or dropped), then
 * flushes the cache files to stable storage. Call it before the process exits:
 * translations still queued then are lost (never half-written). Waits for another
 * process's lock at most once per process (about 5 s); after that, entries are dropped.
 * FEXMAC_ERROR_UNSUPPORTED if the context does not save translations (cache off,
 * untrusted, unavailable or read-only, including in a child after fork()). */
FEXMAC_EXPORT fexmac_result fexmac_context_flush_cache(fexmac_context* context);

/* fork() with a context: call fexmac_before_fork just before fork() and
 * fexmac_after_fork in both processes just after it (child = 1 in the child). No other
 * fexmac call may run meanwhile on another host thread, and no guest thread may be
 * running. In the child only the forking host thread exists: threads created before the
 * fork must not be run there (destroy them or leave them), new ones may be created and
 * run. The child keeps loading from the code cache but saves nothing (it shares the
 * parent's open cache files); its cache state is FEXMAC_CACHE_READ_ONLY. The cache also
 * notices a fork made without these calls and turns read-only in the child, but the
 * library's own locks are only safe with them. */
FEXMAC_EXPORT fexmac_result fexmac_before_fork(fexmac_context* context);
FEXMAC_EXPORT fexmac_result fexmac_after_fork(fexmac_context* context, int child);

/* ---------------------------------------------------------------------------------
 * Threads
 * --------------------------------------------------------------------------------- */

/* Creates a guest thread with the default x86-64 user state (64-bit code segment, all
 * registers zero, rip and rsp zero: set them before running). Allocates its call and
 * return stack and points it at the context's descriptor tables. */
FEXMAC_EXPORT fexmac_result fexmac_thread_create(fexmac_context* context, fexmac_thread** out_thread);

/* Destroys a thread that is not running. */
FEXMAC_EXPORT fexmac_result fexmac_thread_destroy(fexmac_thread* thread);

FEXMAC_EXPORT fexmac_context* fexmac_thread_context(fexmac_thread* thread);
FEXMAC_EXPORT void fexmac_thread_set_user_data(fexmac_thread* thread, void* user_data);
FEXMAC_EXPORT void* fexmac_thread_get_user_data(fexmac_thread* thread);

/* Guest register state. */
typedef struct fexmac_x86_state {
  uint32_t struct_size;
  uint32_t reserved0;
  /* In x86 encoding order: rax rcx rdx rbx rsp rbp rsi rdi r8 ... r15. */
  uint64_t gpr[16];
  uint64_t rip;
  /* RFLAGS as the guest's pushf would see it. */
  uint64_t rflags;
  uint64_t fs_base;
  uint64_t gs_base;
  /* Segment selectors. Setting cs, ds, es or ss also reloads that segment's base from
   * the context's descriptor table (always 0 in 64-bit code); fs and gs bases are set
   * through fs_base and gs_base only. */
  uint16_t cs, ds, es, fs, gs, ss;
  uint16_t reserved1[2];
  uint32_t mxcsr;
  /* The MXCSR bits the guest may set. Reported as 0xFFFF; ignored when set. */
  uint32_t mxcsr_mask;
  uint16_t fcw;
  /* x87 status word: exception flags, condition codes and TOP. */
  uint16_t fsw;
  /* x87 tag word in the abridged (FXSAVE) form: bit i set means register i is valid. */
  uint8_t ftw;
  uint8_t reserved2;
  /* x87 last opcode and data pointer: the core does not track them. Always 0; ignored
   * when set. fip is the last x87 instruction pointer as the core keeps it. */
  uint16_t fop;
  uint64_t fip;
  uint64_t fdp;
  /* x87/MMX registers in physical order (MM0..MM7, not ST(i)); the 80-bit value is in
   * the low 10 bytes of each 16-byte entry. */
  uint64_t mm[8][2];
  /* Low 128 bits of xmm0..xmm15. */
  uint64_t xmm[16][2];
  /* High 128 bits of ymm0..ymm15. Zero, and ignored when set, if guest AVX is off. */
  uint64_t ymm_high[16][2];
} fexmac_x86_state;

/* Sets struct_size and zeroes the rest. */
FEXMAC_EXPORT void fexmac_x86_state_init(fexmac_x86_state* state);

/* Reads the state of a thread that is stopped, or of the thread whose system call
 * callback is running (from that callback). */
FEXMAC_EXPORT fexmac_result fexmac_thread_get_state(fexmac_thread* thread, fexmac_x86_state* state);
FEXMAC_EXPORT fexmac_result fexmac_thread_set_state(fexmac_thread* thread, const fexmac_x86_state* state);

typedef enum fexmac_stop_reason {
  FEXMAC_STOP_NONE = 0,
  /* The system call callback returned FEXMAC_SYSCALL_STOP, or there is no callback.
   * rip points after the `syscall` instruction; instruction_address at it. */
  FEXMAC_STOP_SYSCALL = 1,
  /* The guest executed `hlt`. rip and instruction_address point at the `hlt`. */
  FEXMAC_STOP_HALT = 2,
  /* The guest raised an exception: an invalid instruction (ud2), int3, int n, a
   * privileged instruction and so on, which the core detects itself, or a page fault: a
   * load, store or instruction fetch the guest's memory does not allow (see
   * "Guest memory faults" below). The details are in the stop record. Signal numbers are
   * the host's (<signal.h> on macOS; SIGBUS is 10 here, 7 on Linux). */
  FEXMAC_STOP_GUEST_FAULT = 3,
  /* The translator cannot translate the instruction at instruction_address, although
   * x86 may define it: one FEX does not implement (`int 0x80` in 64-bit mode, `mov` to
   * fs or gs, `o16 iret`, ...) or bytes it cannot decode. A real CPU might run it; this
   * is not the guest's #UD. Filled like an invalid-opcode fault (signal SIGILL, trap 6,
   * si_code 0, fault_address = instruction_address) so a host that only knows
   * GUEST_FAULT can still deliver it as one. */
  FEXMAC_STOP_UNSUPPORTED_INSTRUCTION = 4,
  /* fexmac_thread_request_stop asked the thread to stop. The guest state is at an
   * instruction boundary (or part-way through a rep cmps/scas/lods, with rcx, rsi and rdi
   * showing the progress made, as x86 leaves them when it interrupts one); rip and
   * instruction_address are where running again continues. */
  FEXMAC_STOP_REQUESTED = 5,
} fexmac_stop_reason;

/* Access kinds of a guest page fault (fexmac_stop.access). */
#define FEXMAC_ACCESS_READ 1u
#define FEXMAC_ACCESS_WRITE 2u
#define FEXMAC_ACCESS_EXECUTE 4u

/* x86 page-fault error code bits (fexmac_stop.error_code for trap_number 14). */
#define FEXMAC_PF_PRESENT 1u /* the page is mapped with some access: a protection violation */
#define FEXMAC_PF_WRITE 2u
#define FEXMAC_PF_USER 4u
#define FEXMAC_PF_INSTRUCTION 16u

typedef struct fexmac_stop {
  uint32_t struct_size;
  fexmac_stop_reason reason;
  /* The guest address of the instruction that stopped the run (the `syscall`, the `hlt`
   * or the faulting instruction). */
  uint64_t instruction_address;
  /* For FEXMAC_STOP_GUEST_FAULT and FEXMAC_STOP_HALT: */
  int32_t signal;       /* host signal number to deliver (SIGILL, SIGTRAP, SIGSEGV, SIGBUS) */
  uint32_t trap_number; /* x86 exception vector, e.g. 6 for #UD, 3 for #BP, 13 for #GP, 14 for #PF */
  int32_t si_code;
  uint32_t error_code;
  uint64_t fault_address;
  /* For a page fault (trap_number 14): FEXMAC_ACCESS_READ, _WRITE or _EXECUTE. 0 for
   * other stops. */
  uint32_t access;
  uint32_t reserved;
} fexmac_stop;

/* Sets struct_size and zeroes the rest. */
FEXMAC_EXPORT void fexmac_stop_init(fexmac_stop* stop);

/* Runs guest code on the calling host thread, from the thread's rip, until it stops.
 * `stop` may be NULL. Returns FEXMAC_ERROR_BUSY if the thread is already running or the
 * call comes from a system call callback. Translated code relies on the fault handlers
 * (see below): install them or forward faults to fexmac_handle_fault before running.
 *
 * After any stop the thread's state is consistent and the host stack is back where
 * fexmac_thread_run started: run again to continue from rip, or set a new state first
 * (for example a signal frame the embedder built for a guest handler). */
FEXMAC_EXPORT fexmac_result fexmac_thread_run(fexmac_thread* thread, fexmac_stop* stop);

/* Asks a thread to stop. May be called from any host thread, including from a signal
 * handler (it is async-signal-safe: an atomic store and an mprotect), and any number of
 * times. The thread must stay alive for the duration of the call (this function takes no
 * lock and cannot check the handle; it returns FEXMAC_ERROR_INVALID_ARGUMENT only for
 * NULL).
 *
 * Every request is answered by one FEXMAC_STOP_REQUESTED stop; requests made before it
 * is delivered are merged into it.
 *  - Running translated code: the thread stops at the next block entry or loop back-edge
 *    (translated code writes the thread's interrupt fault page there, which this call
 *    makes read-only). Within a block code runs straight through, so the wait is at
 *    most one block of up to MAXINST instructions, or one `rep movs`/`rep stos`, which is
 *    a single host loop over rcx elements. A system call callback in progress finishes
 *    first; if it returns RESUME the thread stops at the next block entry.
 *  - Not running, or the run ends for another reason first (a system call stop, a guest
 *    fault): the request stays pending and the next fexmac_thread_run returns
 *    FEXMAC_STOP_REQUESTED at once, before any guest instruction runs. */
FEXMAC_EXPORT fexmac_result fexmac_thread_request_stop(fexmac_thread* thread);

/* ---------------------------------------------------------------------------------
 * Guest memory
 *
 * Guest addresses are host addresses and the embedder maps guest memory itself. These
 * functions tell the library what the guest's view of that memory is, which it needs for
 * three things:
 *  - Guest page faults: whether an address is mapped (x86 #PF present bit, SEGV_MAPERR
 *    vs SEGV_ACCERR) and whether the guest was allowed the access.
 *  - Instruction fetch: the translator only reads code from pages registered as
 *    executable. A jump to anything else stops with a page fault (access EXECUTE) at
 *    that address, instead of the translator crashing on unmapped memory.
 *  - Self-modifying code, only if the embedder opts in with the option SMCCHECKS mtrack:
 *    when code on a writable page is translated, the library removes host write access
 *    from that page; a guest write to it then faults, the library discards the
 *    translations of that host page for every thread, gives write access back and lets
 *    the write happen. THE DEFAULT IS SMCCHECKS full: the translator checks the guest
 *    bytes of each instruction before running it, which is slower but has no known gap.
 *    mtrack is opt-in because guest state can be wrong when a write to translated code
 *    is re-run (see "Guest memory faults": the same restart limits apply) and writes
 *    through aliased mappings are not seen. none detects nothing (the embedder calls
 *    fexmac_invalidate).
 *
 * Until the first fexmac_memory_map the library knows nothing about guest memory and
 * behaves as before: every address is executable, nothing is write-protected, and an
 * embedder that changes code must call fexmac_invalidate.
 *
 * Granularity. Guest pages are 4K; addresses and sizes must be multiples of 4K. Host
 * pages are 16K and carry one protection: the library sets each host page to the union
 * of the protections of its four guest pages (minus write access while it holds
 * translated code). So a guest protection is only enforced where all four guest pages of
 * a host page agree (same protection, all mapped). fexmac_memory_map and
 * fexmac_memory_protect refuse (FEXMAC_ERROR_UNSUPPORTED, nothing changed) a change that
 * would leave a host page with guest pages that disagree, unless the protection carries
 * FEXMAC_PROT_ALLOW_UNENFORCED: then the host page gets the union and the guest can read
 * a PROT_NONE 4K page or write a read-only one next to a more permissive one, without a
 * fault (fexmac_memory_unenforced_count counts such changes). Lay guest memory out on
 * 16K boundaries to keep every protection enforced.
 *
 * After registering, the embedder must change protections only with these functions,
 * not with mprotect (which would undo the write protection without discarding
 * translations). It maps and unmaps memory itself (mmap/munmap) and reports it here.
 *
 * Writes by the embedder. A write by host code (the embedder, or a kernel call it makes
 * for the guest, such as read() into guest memory) to a write-protected page would fault
 * (or fail with EFAULT) and is not handled. Bracket such writes with
 * fexmac_memory_write_begin and fexmac_memory_write_end, from any host thread: between
 * them the host pages in the range are writable and are not write-protected again, and
 * _end discards translations made from the range meanwhile. Without memory tracking or
 * with SMCCHECKS other than mtrack (the default) both return at once.
 * --------------------------------------------------------------------------------- */

#define FEXMAC_PROT_NONE 0u
#define FEXMAC_PROT_READ 1u
#define FEXMAC_PROT_WRITE 2u
#define FEXMAC_PROT_EXEC 4u
/* Accept a protection the host cannot enforce (see "Granularity"); without it such a
 * fexmac_memory_map or fexmac_memory_protect is refused. Not a protection: never
 * stored, never reported by fexmac_memory_query. */
#define FEXMAC_PROT_ALLOW_UNENFORCED 0x100u

/* Records that [address, address + size) is mapped with the guest protection `prot`
 * (FEXMAC_PROT_* bits) and sets the host protection of the host pages it covers (see
 * "Granularity"). Discards translations of those host pages. Also used for a guest
 * mprotect. FEXMAC_ERROR_INVALID_ARGUMENT for unaligned or out-of-range arguments
 * (addresses must be below 2^47), FEXMAC_ERROR_UNSUPPORTED if the host refuses the
 * protection. */
FEXMAC_EXPORT fexmac_result fexmac_memory_map(fexmac_context* context, uint64_t address, uint64_t size, uint32_t prot);
/* As fexmac_memory_map for a range that is already registered as mapped (a guest
 * mprotect); FEXMAC_ERROR_INVALID_ARGUMENT if any page of it is not. */
FEXMAC_EXPORT fexmac_result fexmac_memory_protect(fexmac_context* context, uint64_t address, uint64_t size, uint32_t prot);
/* Records that the range is no longer mapped and discards translations of its host pages.
 * Call it before mapping something else there; the library does not munmap. */
FEXMAC_EXPORT fexmac_result fexmac_memory_unmap(fexmac_context* context, uint64_t address, uint64_t size);
/* The registered guest protection of the page holding `address`, and whether it is
 * mapped (out values may be NULL). */
FEXMAC_EXPORT fexmac_result fexmac_memory_query(fexmac_context* context, uint64_t address, uint32_t* prot, int* mapped);
/* See "Writes by the embedder". Calls must pair up with the same range; they nest. */
FEXMAC_EXPORT fexmac_result fexmac_memory_write_begin(fexmac_context* context, uint64_t address, uint64_t size);
FEXMAC_EXPORT fexmac_result fexmac_memory_write_end(fexmac_context* context, uint64_t address, uint64_t size);
/* Number of changes so far that left a host page with guest pages of different
 * protections (accepted with FEXMAC_PROT_ALLOW_UNENFORCED, or a partial unmap): on those
 * host pages the guest's 4K protections are not enforced. */
FEXMAC_EXPORT uint64_t fexmac_memory_unenforced_count(fexmac_context* context);
/* Number of guest writes to write-protected code the library has handled. */
FEXMAC_EXPORT uint64_t fexmac_memory_code_write_count(fexmac_context* context);

/* ---------------------------------------------------------------------------------
 * Host fault handling
 *
 * Translated code takes some host faults on purpose, and the library handles them:
 *  - ARM64 atomic instructions raise an alignment fault on a misaligned address that
 *    crosses 16 bytes, where x86 allows it; the core rewrites them.
 *  - The translator learns that its scratch buffer was too small by running into the
 *    page after it, and must be resumed at its restart point.
 *  - The core's call/return stack overflows or underflows into its guard pages (deep
 *    recursion, unmatched call or ret); its pointer is reset to the middle.
 *  - A thread asked to stop (fexmac_thread_request_stop) writes its read-only interrupt
 *    fault page at the next block entry; the library leaves the dispatcher there.
 *  - A guest write to a page the library write-protected for self-modifying-code
 *    detection (see "Guest memory").
 *  - A guest memory access fault in translated code: the library reconstructs the guest
 *    state as it was before the faulting instruction (registers from the host context,
 *    rip from the block's instruction map, flags) and fexmac_thread_run returns
 *    FEXMAC_STOP_GUEST_FAULT with trap_number 14 (see "Guest memory faults").
 * Faults are told apart by the data abort's fault status code (ESR_EL1.DFSC, which macOS
 * passes in the signal context), not by si_code: macOS reports a protection fault and
 * an alignment fault alike as SIGBUS with si_code 1. The library claims only faults from
 * a host thread that is running guest code, and among those only the cases above. Every
 * other fault (a bug in host code, the embedder's callback touching bad memory, a host
 * write to a write-protected code page outside fexmac_memory_write_begin/_end) is not
 * the library's.
 *
 * Guest memory faults. signal, si_code and error_code are what an x86 kernel would
 * report: SIGSEGV with SEGV_MAPERR and no FEXMAC_PF_PRESENT for an unmapped page,
 * SEGV_ACCERR for a page whose protection forbids the access (FEXMAC_PF_PRESENT unless
 * the page is PROT_NONE, as Linux reports it), FEXMAC_PF_WRITE for a write (an atomic
 * read-modify-write counts as a write), FEXMAC_PF_INSTRUCTION for a fetch, and always
 * FEXMAC_PF_USER. SIGBUS with BUS_ADRERR when the registered protection allows the access
 * but the host refused it (a file mapping past its end, for example). Without registered
 * memory the library goes by the host fault: SIGSEGV from the host is an unmapped page,
 * SIGBUS a protected one. The guest registers are those before the faulting instruction,
 * with two limits of the translator: a `rep movs` or `rep stos` that faults part-way
 * reports its registers from before the instruction (the core runs it as one host loop;
 * running it again redoes it from the start), and flags that the next instructions of the
 * block overwrite without reading may be stale.
 *
 * Two ways to connect it, pick one per process:
 *
 *  1. fexmac_install_fault_handlers(): the library installs SIGBUS and SIGSEGV
 *     handlers (SA_SIGINFO | SA_ONSTACK | SA_NODEFER, asynchronous signals blocked while
 *     they run) and remembers whatever was installed before. A fault that is not the
 *     library's is passed to that previous handler, with its sa_mask and SA_RESETHAND
 *     honoured; SIGSEGV, SIGBUS and the other synchronous fault signals are never
 *     blocked around it (even without SA_NODEFER), because on macOS a fault while its
 *     signal is blocked hangs the thread. The previous handler may leave with
 *     siglongjmp. If it returns, the faulting instruction runs again, every time, as it
 *     would with only that handler installed: the library cannot know whether the
 *     handler fixed the cause (a handler that maps the page and returns is the common
 *     case), so a handler that returns without fixing it makes the instruction fault
 *     forever, exactly as it does without the library (macOS re-delivers a fault on
 *     return; checked on the M4). Resolving the fault or leaving is that handler's
 *     responsibility; the library adds no retry limit. If the previous action was the
 *     default: a fault is let
 *     happen again with the default action restored, so the process dies with the usual
 *     signal; a signal sent with kill() is re-sent with the default action, so it does
 *     the same. If the previous action was "ignore": a sent signal is ignored; a fault,
 *     which cannot be ignored, kills the process as with the default. A host that
 *     installs its own handlers later must call fexmac_handle_fault from them (or chain
 *     to the action it replaced).
 *
 *  2. The host owns SIGBUS/SIGSEGV: it does not call fexmac_install_fault_handlers and
 *     calls fexmac_handle_fault first in its own handler for both signals. If that
 *     returns 1 the host must return from its handler at once without touching the
 *     context; the library has already edited it.
 *
 * A fault inside the library's own handling code is reported on standard error and
 * turns into an ordinary crash rather than a hang: with SA_NODEFER the nested fault
 * re-enters the handler, which knows it is inside its own code (a per-thread depth that
 * code keeps; it never leaves by a jump), restores the default action and lets the
 * fault happen again.
 *
 * The library does not police handlers it chains to. While a previous handler runs, the
 * library is not handling anything: a fault inside that handler re-enters, is not the
 * library's, and is passed to the previous handler again, as if the library were not
 * installed. If that repeats, the stack overflows and the process dies (macOS ends it
 * with SIGILL, SIGSEGV or SIGBUS); it does not hang.
 *
 * A host using way 2 should install its handlers with SA_NODEFER; without it a fault
 * while the signal is blocked hangs the thread on macOS. If such a host leaves a fault
 * inside fexmac_handle_fault itself with siglongjmp, the library's depth stays raised on
 * that thread and later log messages from it are written to standard error.
 * --------------------------------------------------------------------------------- */

/* Installs the library's handlers once per process. Returns FEXMAC_OK if they are
 * installed (also on a repeated call). */
FEXMAC_EXPORT fexmac_result fexmac_install_fault_handlers(void);

/* Handles a SIGBUS or SIGSEGV if it is one the library expects. `ucontext` is the third
 * argument of an SA_SIGINFO handler. Returns 1 if handled (return from the signal
 * handler immediately), 0 if the fault is not the library's.
 *
 * Async-signal-safe: it allocates nothing, takes no lock that code outside a fault
 * handler holds, and never calls the log callback. It can take two spin locks, each held
 * only by another thread's fault handler and only for one bounded step: the core's
 * per-block patch lock (the few instructions of a patch) and the core's process-wide
 * split-lock lock (one attempt at one emulated locked operation on an operand that
 * crosses 16 bytes; it makes those operations atomic with each other and cannot be
 * switched off). While the core may hold either, this function blocks asynchronous
 * signals itself (everything but SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP and SIGSYS) and
 * restores the caller's mask afterwards, so a host's asynchronous handler cannot run
 * then. The split-lock lock is held while guest memory is accessed. The library checks
 * that memory first and reports an inaccessible page as the guest's page fault before
 * the emulation starts, and keeps its own protection changes out of the way of an
 * emulation in progress, so no fault is taken with the lock held. If one is anyway (the
 * embedder changed the page with a raw mprotect or munmap at that moment, which the
 * "Guest memory" contract excludes), this function claims it, writes a message and
 * restores the default action: the process ends, and the fault never reaches the host's
 * handler with the lock held. If the core meets an atomic instruction it cannot rewrite
 * (which would be a core bug), the core formats an error message, which allocates; the
 * library writes it with write(2) and the fault is not claimed. */
FEXMAC_EXPORT int fexmac_handle_fault(int signal, siginfo_t* info, void* ucontext);

#ifdef __cplusplus
}
#endif

#endif /* FEXMAC_H */
