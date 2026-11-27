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
 * may be run on any host thread, but by one host thread at a time. Every function
 * returns FEXMAC_ERROR_INVALID_HANDLE for a handle that was destroyed, as long as the
 * memory has not been reused for a new handle of the same kind (handles are checked
 * against a table of live handles, not dereferenced first).
 *
 * Process-wide state: the core's configuration, its log hooks and the fault handlers are
 * process-wide, so at most one context exists at a time.
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
 * with; a program should refuse to run if the major part differs. */
#define FEXMAC_VERSION_MAJOR 1
#define FEXMAC_VERSION_MINOR 0
#define FEXMAC_VERSION ((FEXMAC_VERSION_MAJOR << 16) | FEXMAC_VERSION_MINOR)

typedef struct fexmac_context fexmac_context;
typedef struct fexmac_thread fexmac_thread;

typedef enum fexmac_result {
  FEXMAC_OK = 0,
  /* A required pointer was NULL, or a size field did not match. */
  FEXMAC_ERROR_INVALID_ARGUMENT = 1,
  /* The handle is not a live context or thread (never created, or already destroyed). */
  FEXMAC_ERROR_INVALID_HANDLE = 2,
  /* An option name is unknown, its value does not parse, or it is not supported here. */
  FEXMAC_ERROR_BAD_CONFIG = 3,
  /* The object is in use: a context already exists, a context still has threads, or a
   * thread is running (or is being run by its own system call callback). */
  FEXMAC_ERROR_BUSY = 4,
  /* The core could not be initialised. */
  FEXMAC_ERROR_INIT_FAILED = 5,
  FEXMAC_ERROR_OUT_OF_MEMORY = 6,
  /* The operation needs fault handling that is not installed or not supported. */
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

/* One core option. The name is either the core's environment-variable name without the
 * "FEX_" prefix ("MULTIBLOCK", "MAXINST", "TSOENABLED") or its configuration-file name
 * ("Multiblock", "MaxInst", "TSOEnabled"). Values are strings as in the environment:
 * numbers in decimal or 0x hex, booleans as 0 or 1, named values as the core accepts
 * them (for example SMCCHECKS=full). Later entries override earlier ones. */
typedef struct fexmac_option {
  const char* name;
  const char* value;
} fexmac_option;

/* What the system call callback wants to happen next. */
typedef enum fexmac_syscall_action {
  /* Continue running guest code after the system call instruction. Any guest state the
   * callback set (typically rax, the return value) takes effect. */
  FEXMAC_SYSCALL_RESUME = 0,
  /* Leave fexmac_thread_run, which returns FEXMAC_STOP_SYSCALL. rip already points
   * after the system call instruction, so running again continues the program. */
  FEXMAC_SYSCALL_STOP = 1,
} fexmac_syscall_action;

/* Called on the guest thread's host thread when guest code executes `syscall`. The
 * guest registers are in the thread's state: read them with fexmac_thread_get_state and
 * set the result with fexmac_thread_set_state. The callback must not run, destroy or
 * create threads, and must not destroy the context. */
typedef fexmac_syscall_action (*fexmac_syscall_fn)(fexmac_thread* thread, void* user_data);

typedef enum fexmac_log_level {
  FEXMAC_LOG_ASSERT = 1,
  FEXMAC_LOG_ERROR = 2,
  FEXMAC_LOG_DEBUG = 3,
  FEXMAC_LOG_INFO = 4,
} fexmac_log_level;

/* Receives the core's log messages. May be called on any thread. */
typedef void (*fexmac_log_fn)(fexmac_log_level level, const char* message, void* user_data);

typedef struct fexmac_config {
  /* Must be sizeof(fexmac_config). Set by fexmac_config_init. Lets later versions add
   * fields at the end without breaking programs built against this one. */
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
} fexmac_config;

/* Fills a config with defaults: no options, no callbacks. */
FEXMAC_EXPORT void fexmac_config_init(fexmac_config* config);

/* 1 if `name` is an option fexmac_context_create accepts, 0 otherwise. */
FEXMAC_EXPORT int fexmac_option_known(const char* name);

/* ---------------------------------------------------------------------------------
 * Context
 * --------------------------------------------------------------------------------- */

/* Creates the translator context: applies the options, detects host features, emits
 * the dispatcher and prepares the shared descriptor table. 64-bit guests only. */
FEXMAC_EXPORT fexmac_result fexmac_context_create(const fexmac_config* config, fexmac_context** out_context);

/* Destroys a context with no threads left (FEXMAC_ERROR_BUSY otherwise). */
FEXMAC_EXPORT fexmac_result fexmac_context_destroy(fexmac_context* context);

/* Runs the guest CPUID instruction as the guest would see it. out: eax, ebx, ecx, edx. */
FEXMAC_EXPORT fexmac_result fexmac_cpuid(fexmac_context* context, uint32_t function, uint32_t leaf, uint32_t out[4]);

/* Discards translations of guest code in [address, address + size). Call it after
 * guest code is changed or unmapped from outside guest execution (for example by a
 * loader). Safe from any host thread, including from a system call callback. Takes the
 * core's code invalidation lock and applies to every thread of the context. */
FEXMAC_EXPORT fexmac_result fexmac_invalidate(fexmac_context* context, uint64_t address, uint64_t size);

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

/* Guest register state. Layout fixed for version 1. */
typedef struct fexmac_x86_state {
  /* In x86 encoding order: rax rcx rdx rbx rsp rbp rsi rdi r8 ... r15. */
  uint64_t gpr[16];
  uint64_t rip;
  /* RFLAGS as the guest's pushf would see it. */
  uint64_t rflags;
  uint64_t fs_base;
  uint64_t gs_base;
  uint32_t mxcsr;
  uint16_t fcw;
  /* x87 status word: exception flags, condition codes and TOP. */
  uint16_t fsw;
  /* x87 tag word in the abridged (FXSAVE) form: bit i set means register i is valid. */
  uint8_t ftw;
  uint8_t reserved[7];
  /* x87/MMX registers in physical order (MM0..MM7, not ST(i)); the 80-bit value is in
   * the low 10 bytes of each 16-byte entry. */
  uint64_t mm[8][2];
  /* Low 128 bits of xmm0..xmm15. */
  uint64_t xmm[16][2];
  /* High 128 bits of ymm0..ymm15. Zero, and ignored when set, if guest AVX is off. */
  uint64_t ymm_high[16][2];
} fexmac_x86_state;

/* Reads the state of a thread that is stopped, or of the thread whose system call
 * callback is running (from that callback). */
FEXMAC_EXPORT fexmac_result fexmac_thread_get_state(fexmac_thread* thread, fexmac_x86_state* state);
FEXMAC_EXPORT fexmac_result fexmac_thread_set_state(fexmac_thread* thread, const fexmac_x86_state* state);

typedef enum fexmac_stop_reason {
  FEXMAC_STOP_NONE = 0,
  /* The system call callback returned FEXMAC_SYSCALL_STOP, or there is no callback.
   * rip points after the `syscall` instruction. */
  FEXMAC_STOP_SYSCALL = 1,
  /* The guest executed `hlt`. rip points at the `hlt` instruction. */
  FEXMAC_STOP_HALT = 2,
  /* The guest raised an exception the core detects without a host fault: an invalid
   * instruction (ud2), int3, int n, a privileged instruction and so on. The details are
   * in the stop record, using Linux signal numbering as the core reports them. */
  FEXMAC_STOP_GUEST_FAULT = 3,
  /* Values for later versions: a stop requested by another thread, and guest faults
   * caught as host faults (guest access to unmapped memory). Until then such a host fault
   * is passed on as described under fault handling. */
} fexmac_stop_reason;

typedef struct fexmac_stop {
  fexmac_stop_reason reason;
  /* For FEXMAC_STOP_GUEST_FAULT and FEXMAC_STOP_HALT, as the core reports them: */
  int32_t signal;       /* host signal number the fault maps to (SIGILL, SIGTRAP, SIGSEGV) */
  uint32_t trap_number; /* x86 exception vector, e.g. 6 for #UD, 3 for #BP, 13 for #GP */
  int32_t si_code;
  uint32_t error_code;
  uint64_t fault_address;
} fexmac_stop;

/* Runs guest code on the calling host thread, from the thread's rip, until it stops.
 * `stop` may be NULL. Returns FEXMAC_ERROR_BUSY if the thread is already running.
 * Translated code relies on the fault handlers (see below): install them or forward
 * faults to fexmac_handle_fault before running guest code that uses misaligned atomic
 * operations or very large blocks. */
FEXMAC_EXPORT fexmac_result fexmac_thread_run(fexmac_thread* thread, fexmac_stop* stop);

/* ---------------------------------------------------------------------------------
 * Host fault handling
 *
 * Translated code takes some host faults on purpose: ARM64 atomic instructions fault on
 * misaligned addresses (SIGBUS, BUS_ADRALN) and the core rewrites them; the translator
 * learns that its scratch buffer was too small by running into the page after it
 * (SIGBUS or SIGSEGV) and must be resumed at its restart point. The library handles
 * these. It never claims a fault that does not come from a running fexmac thread.
 *
 * Two ways to connect it, pick one per process:
 *
 *  1. fexmac_install_fault_handlers(): the library installs SIGBUS and SIGSEGV
 *     handlers (with SA_SIGINFO | SA_ONSTACK | SA_NODEFER) and remembers whatever was
 *     installed before. A fault that is not the library's is passed to that previous
 *     handler; if it was the default action or "ignore", the default action is restored
 *     and the faulting instruction re-executes, so the process dies with the usual
 *     signal. A host that installs its own handlers later must call
 *     fexmac_handle_fault from them (or chain to the action it replaced).
 *
 *  2. The host owns SIGBUS/SIGSEGV: it does not call fexmac_install_fault_handlers and
 *     calls fexmac_handle_fault first in its own handler for both signals. If that
 *     returns 1 the host must return from its handler at once without touching the
 *     context; the library has already edited it.
 *
 * A fault inside the library's handler itself is reported on standard error and turns
 * into an ordinary crash rather than a hang: with SA_NODEFER the nested fault re-enters
 * the handler, which detects the nesting, restores the default action and lets the
 * fault happen again. A host using way 2 should install its handlers with SA_NODEFER
 * too; without it a fault while the signal is blocked hangs the thread on macOS.
 * --------------------------------------------------------------------------------- */

/* Installs the library's handlers once per process. Returns FEXMAC_OK if they are
 * installed (also on a repeated call). */
FEXMAC_EXPORT fexmac_result fexmac_install_fault_handlers(void);

/* Handles a SIGBUS or SIGSEGV if it is one the library expects. `ucontext` is the third
 * argument of an SA_SIGINFO handler. Returns 1 if handled (return from the signal
 * handler immediately), 0 if the fault is not the library's. Async-signal-safe. */
FEXMAC_EXPORT int fexmac_handle_fault(int signal, siginfo_t* info, void* ucontext);

#ifdef __cplusplus
}
#endif

#endif /* FEXMAC_H */
