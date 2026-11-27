# macOS: embedding the core through fexmac

The translator core (`FEXCore`) does not run programs on its own. Something has to load
the guest program, give it memory, answer its system calls and catch its faults. On Linux
that is FEX's process emulation layer, which this port does not build. On macOS the
embedder is whatever wants x86-64 code translated: the test tools today, a console
compatibility layer such as AnyPS5 later.

`fexmac` is the C interface for that embedder. It lives in `Source/Embedding/`:

| File | What it is |
|---|---|
| `include/fexmac.h` | The public header. C and C++, opaque handles, only C types and `<signal.h>`. |
| `Library.cpp` | Contexts, threads, guest state, running guest code, system calls, invalidation, locking. |
| `Options.cpp` | The supported options and their value rules. |
| `Faults.cpp` | Host fault classification and handling, and the signal-handler contract. |
| `fexmac_testing.h` | Test-only hooks, compiled only into the `fexmac_testing` build. |
| `tests/` | `CApiTest.c` (`fexmac_c_test`), `HookTest.c` (`fexmac_hook_test`), `TestSupport.c`. |

## What the library does for the embedder

| Duty | Why | Where |
|---|---|---|
| Set up the core's configuration from the embedder's options, checking every option against a supported list | The core's own loaders turn a bad number into 0, and some options stop the process without FEX's Linux front end | `fexmac_context_create`, `Options.cpp` |
| Detect host features, create the context, give it a system call handler and a signal delegator, call `InitCore` | Required by the core | `fexmac_context_create` |
| Provide descriptor tables (GDT and LDT, 8192 null entries) and a 64-bit code segment | Segment loads read them | one table per context |
| Create a thread state per guest thread, allocate its call and return stack with guard pages | Guest `call` and `ret` crash without the stack | `fexmac_thread_create` |
| Reset the call/return stack pointer when it runs into a guard page | Deep recursion and unmatched `call`/`ret` otherwise crash | `fexmac_handle_fault` |
| Call the allocator's per-thread initialisation on each host thread that creates or runs guest threads | Required before a thread allocates | `fexmac_thread_create`, `fexmac_thread_run` |
| Set `SynchronousExceptionHandler` and turn what it reports into a stop reason | The core calls it for `hlt`, `ud2`, `int3` and the like | `fexmac_thread_run` |
| Move `rip` past `syscall`, prefixes included | The core leaves `rip` on the instruction | system call handler |
| Discard stale translations, holding the core's invalidation lock, for every thread | Self-modifying code loops forever otherwise; the core asks through `InvalidateGuestCodeRange` | `fexmac_invalidate` and the core's callback |
| Catch alignment faults in translated code and pass them to the core | Misaligned atomic operations fault on ARM64 and the core rewrites them | `fexmac_handle_fault` |
| Resume the translator at its restart point when it runs into the end of its scratch buffer | Large blocks crash otherwise | `fexmac_handle_fault` |

Still the embedder's job: mapping guest memory at the addresses the guest expects (guest
addresses are host addresses; the library does not allocate guest memory), loading the
program, and answering system calls.

## The interface

```c
fexmac_result fexmac_context_create(const fexmac_config*, fexmac_context**);
fexmac_result fexmac_context_destroy(fexmac_context*);
fexmac_result fexmac_cpuid(fexmac_context*, uint32_t function, uint32_t leaf, uint32_t out[4]);
fexmac_result fexmac_invalidate(fexmac_context*, uint64_t address, uint64_t size);

fexmac_result fexmac_thread_create(fexmac_context*, fexmac_thread**);
fexmac_result fexmac_thread_destroy(fexmac_thread*);
fexmac_result fexmac_thread_get_state(fexmac_thread*, fexmac_x86_state*);
fexmac_result fexmac_thread_set_state(fexmac_thread*, const fexmac_x86_state*);
fexmac_result fexmac_thread_run(fexmac_thread*, fexmac_stop*);

fexmac_result fexmac_install_fault_handlers(void);
int           fexmac_handle_fault(int signal, siginfo_t*, void* ucontext);
```

The header documents every function. The design decisions:

- **Every call returns a result code.** A C caller has no other way to learn about a bad
  option, a busy thread or a dead handle.
- **Structures the caller allocates carry `struct_size`** (`fexmac_config`,
  `fexmac_x86_state`, `fexmac_stop`, each with an `_init` function). Any size from the
  first version's up to the library's own is accepted; fields beyond the caller's size
  are not touched. A newer, larger structure is accepted if the fields the library does
  not know are zero on input (otherwise `FEXMAC_ERROR_UNSUPPORTED`), and they are zeroed
  on output.
- **Guest state** (`fexmac_x86_state`): general registers, `rip`, `rflags` as `pushf`
  would see it, FS and GS bases, the six segment selectors (setting `cs`, `ds`, `es` or
  `ss` reloads that segment's base from the descriptor table), MXCSR and `mxcsr_mask`
  (0xFFFF), x87 control, status and abridged tag words, `fip`, MM0 to MM7 in physical
  order, XMM and the upper halves of YMM. `fop` and `fdp` are not tracked by the core:
  always 0, ignored on set. Get, set, get gives identical state (tested).
- **System calls: a callback, with stopping and restarting.** Without a callback every
  `syscall` stops the run with `FEXMAC_STOP_SYSCALL`. A callback answers the call in
  place and returns `RESUME`, `STOP` (leave the run; running again continues), or
  `RESTART` (run the same `syscall` again with the state the callback set, as a kernel
  restarts an interrupted call). The callback sees `rip` after the instruction. The core
  leaves `rip` at the start of the instruction and has just stored the next instruction's
  address in `rcx`, as `syscall` does; the library takes the length from there, so a
  prefixed `syscall` (`48 0F 05`) resumes correctly. `fexmac_stop.instruction_address`
  names the instruction that stopped the run.
- **What a callback may do.** Read and write its own thread's state; create threads (the
  intended embedder handles `clone` there) and set their state; destroy other threads
  that are not running; invalidate; call `fexmac_cpuid`. It may not run any thread (that
  would nest a second dispatcher on the same host stack) or destroy its own thread
  (it is running); both return `FEXMAC_ERROR_BUSY`.
- **Leaving the dispatcher uses the core's own exit**, not `longjmp`. The dispatcher
  saves its stack pointer on entry (`ReturningStackLocation`) and has a stop handler that
  restores the host's registers and returns from `ExecuteThread`; Linux FEX uses it to
  stop threads. The library resets the stack to the saved location and jumps there, only
  from the system call and synchronous exception handlers, where the core has already
  written the guest registers back.
- **Stop reasons:** `SYSCALL`, `HALT` (a user-mode `hlt`, which the core reports as a
  general protection fault at the instruction) and `GUEST_FAULT` (an exception the core
  detects itself, with vector, signal, `si_code`, error code and address; also code the
  translator cannot translate at all, such as `int 0x80` in 64-bit mode, which arrives as
  SIGILL, vector 6, at the instruction instead of crashing the host). Stopping a
  thread from another thread and guest memory faults are package D.
- **One context per process.** The core's configuration, its log hooks and the fault
  handlers are process-wide, so a second `fexmac_context_create` returns `BUSY` until the
  first context is destroyed. A context can be destroyed and created again. A failed
  create leaves nothing process-wide changed, log hooks included.
- **Locking.** One registry lock guards the tables of live handles, every thread's
  running and in-callback flags and every context's count of calls in progress, so a
  handle check and the state change that follows it are one step: a destroy that races
  with a run either waits for the run to end (`BUSY`) or happens wholly before it (the
  run then gets `INVALID_HANDLE`), and a context is never destroyed while
  `fexmac_thread_create`, `fexmac_cpuid` or `fexmac_invalidate` uses it. A context's
  thread-list lock is taken after the registry lock, never before. A handle is
  recognised as dead until its memory is reused for a new handle of the same kind.
- **Logging.** The log callback is called on any host thread but never from a signal
  handler: a core message produced while the library handles a fault is written to
  standard error with `write(2)` instead.

## Options

Only these options are accepted; any other name, and any value that does not fit the
option's rule, gives `FEXMAC_ERROR_BAD_CONFIG`. Names are the core's environment-variable
name without `FEX_`, or its configuration-file name. Numbers are decimal or `0x`
hexadecimal; a leading 0 is refused rather than read as octal.

| Option | Values | Notes |
|---|---|---|
| `MULTIBLOCK` | 0, 1 | |
| `MAXINST` | 1 to 2^31-1 | instructions per block |
| `TSOENABLED`, `VECTORTSOENABLED`, `MEMCPYSETTSOENABLED`, `HALFBARRIERTSOENABLED` | 0, 1 | x86 memory-ordering emulation (software; macOS has no hardware mode for ordinary processes) |
| `SMCCHECKS` | `none`, `mtrack`, `full` (or 0, 1, 2) | `mtrack` needs page-protection tracking (package D); until then it detects nothing and the embedder calls `fexmac_invalidate` |
| `X87REDUCEDPRECISION`, `HIDEHYPERVISORBIT` | 0, 1 | |
| `HOSTFEATURES` | comma-separated core names, each known | |
| `SOFTWARERNG` | 0, 1 | RDRAND and RDSEED from `arc4random_buf`; no Apple chip so far has FEAT_RNG, so without it the guest CPUID offers neither |
| `SILENTLOG`, `DUMPGPRS`, `DISABLETELEMETRY` | 0, 1 | `DISABLETELEMETRY` defaults to 1 |
| `IS64BIT_MODE` | 1 | 32-bit guests cannot have the memory they need on macOS |

Everything else is refused on purpose, for example `DISKCACHE` (context creation aborts
without FEX's Linux front end; package F brings cached translations through the
reserved `code_cache_*` config fields), `STRICTINPROCESSSPLITLOCKS` (makes the fault
handler spin on a lock), `GDBSERVER`, `SINGLESTEP`, `JITFEATURES`. To add one, give it
the narrowest rule in `Options.cpp` and list it here.

## Host faults and signal handlers

### Telling faults apart

macOS reports a misaligned atomic access and an access to a protected page alike as
SIGBUS with `si_code` 1 (`BUS_ADRALN`), so `si_code` cannot tell them apart (measured on
the M4). The kernel passes the data abort's syndrome (`ESR_EL1`) in the signal context
(`uc_mcontext->__es.__esr`); its fault status code (DFSC, bits 5:0) can. Measured on the
M4 (JITWriteProbe's "Fault status" cases record it, as built and hardened):

| Access | Signal, si_code | DFSC | Kind |
|---|---|---|---|
| load from a PROT_NONE page | SIGBUS, 1 | 0x07 | translation fault, level 3 |
| store to a read-only page (already read) | SIGBUS, 1 | 0x0F | permission fault, level 3 |
| store to a read-only page never touched | SIGBUS, 1 | 0x07 | translation fault |
| atomic add on a PROT_NONE page | SIGBUS, 1 | 0x07 | translation fault |
| load from an unmapped address | SIGSEGV, 2 | 0x05 | translation fault, level 1 |
| misaligned atomic across 16 bytes | SIGBUS, 1 | 0x21 | alignment fault |
| misaligned atomic on a PROT_NONE page | SIGBUS, 1 | 0x21 | alignment fault (checked before permissions) |

So only DFSC 0x21 is a misaligned access. Translation and permission faults both mean
"the guest touched memory it may not"; which page state caused it must come from the
embedder's own records, not from the fault kind. Bit 6 (WnR) says whether the access was
a write, but an atomic read-modify-write on a PROT_NONE page reports a read.

A signal sent with `kill` or `pthread_kill` also arrives with `si_code` 2 (SIGSEGV) or 1
(SIGBUS) and `si_addr` 0. It is told from a fault by the syndrome: a real fault was a
synchronous abort (exception class 0x20 to 0x26 with the fault address equal to
`si_addr`), while a signal the thread sent itself arrives after its `kill` system call
(class 0x15). A signal sent by another thread leaves this thread's last syndrome in
place, so the address is compared too; the one case misread is a signal sent while the
thread's last exception was a fault at address 0.

### What the library claims

Only faults on a host thread that is inside `fexmac_thread_run`, and among those:

1. a translation or permission fault inside that thread's translator scratch-buffer
   guard: the thread resumes at the translator's restart point;
2. a translation or permission fault inside its call/return stack allocation (guard
   pages): the stack pointer (`x25`, `REG_CALLRET_SP`) is reset to its starting
   position and the instruction retried, as FEX's Linux front end does;
3. an alignment fault with the PC in translated code: the core rewrites the access.

Everything else is not the library's. A guest load or store to an unmapped or protected
page in translated code is passed on: with the library's handlers installed and nothing
installed before, the library writes `fexmac: SIGBUS: guest memory access fault in
translated code at address ...` to standard error and the process dies with the signal;
with a host handler (installed before, or owning the signals) the host receives it.
Delivering it to the guest with reconstructed state is package D.

`fexmac_handle_fault` allocates nothing, takes no lock that code outside a fault handler
holds and never calls the log callback. The one lock it can take is the core's per-block
patch spin lock, held only by another thread's fault handler for the few instructions of
a patch (the option that would add another, `STRICTINPROCESSSPLITLOCKS`, is refused). If
the core meets an atomic instruction it cannot rewrite (a core bug), it formats an error
message, which allocates; the library writes it with `write(2)` and the fault is not
claimed.

### Connecting the library

One way per process:

1. **The library installs the handlers** (`fexmac_install_fault_handlers`), for SIGBUS
   and SIGSEGV, with `SA_SIGINFO | SA_ONSTACK | SA_NODEFER` and every signal blocked
   while they run except the synchronous fault signals (SIGSEGV, SIGBUS, SIGILL, SIGFPE,
   SIGTRAP, SIGSYS), and keeps whatever was installed before. A fault that is not the
   library's goes to that previous handler, with its `sa_mask` applied, its own signal
   blocked unless it had `SA_NODEFER`, and its action reset first if it had
   `SA_RESETHAND`. If the previous action was the default: a fault happens again with the
   default action restored, so the process dies with the usual signal; a sent signal is
   re-sent with the default action, which ends the process the same way. If it was
   "ignore": a sent signal is ignored and the library stays installed; a fault cannot be
   ignored and kills the process as with the default. A host that installs handlers after
   the library must call `fexmac_handle_fault` from them or chain to the action it
   replaced.
2. **The host owns the signals** (a console compatibility layer with its own handlers)
   and calls `fexmac_handle_fault(signal, info, ucontext)` first in its handlers for
   SIGBUS and SIGSEGV. A return of 1 means the library has edited the context and the
   host must return at once.

### A fault inside a handler crashes instead of hanging

On macOS a synchronous fault while its signal is blocked hangs the thread; by default a
signal is blocked while its handler runs, so a bug in a handler used to look like a hang
(found by package A; a reproducer on the M4 hangs without `SA_NODEFER` and dies with
SIGBUS at once with it). The library's handler is installed with `SA_NODEFER`, so a
nested fault re-enters it. Because every asynchronous signal is blocked while it runs,
the only thing that can interrupt it on its thread is a fault it causes, so it knows a
re-entry is a nested fault. It records whether it is in its own code or in the previous
handler it chained to, writes `fexmac: fault inside the fault handler` (or `... inside
the previously installed handler ...`), restores the default action and returns; the
fault repeats and kills the process.

What it cannot fix: a previous handler installed without `SA_NODEFER` that faults runs
with its signal blocked, as the kernel would have run it, and hangs; a host using way 2
should install its handlers with `SA_NODEFER`.

### Invalidation and signals

`fexmac_invalidate` on a guest thread defers signals while it holds the core's
invalidation lock (the core's own mechanism). On any other host thread it blocks the
asynchronous signals instead, never the synchronous fault signals, so a fault while
waiting for or holding the lock still reaches its handler (tested with a fault taken
inside the locked region).

### Where later packages extend the fault handling

`fexmac_handle_fault` runs a fixed sequence of checks (`Faults.cpp`). Package D adds to
that sequence: the interrupt fault page (stopping a thread from another thread, reusing
the dispatcher exit of the system call stop with a new stop reason), write faults on
pages protected for self-modifying-code detection, and guest memory faults (classified
"access" with the PC in translated code) with reconstructed guest state, reported as a
new stop reason.

## Extension points for cached translations (package F)

Reserved now, implemented when package F's core work lands on `macos/v1`:

- `fexmac_config.code_cache_directory` and `code_cache_enabled`: must be NULL and 0
  today; anything else gives `FEXMAC_ERROR_UNSUPPORTED`. The core's `DISKCACHE` and
  `DISKCACHEPATH` options stay refused.
- Planned functions, named in the header but not declared until they exist: a cache
  flush for a context, a query for translation counters (compiled, loaded, rejected),
  and a way for the embedder to register its mapped guest ranges so that the core's
  `QueryGuestExecutableRange` reports them (today every address is reported as
  executable and writable; see `Internal.h`). Package D needs the last one too.

## The library and how to link it

| Target | Form | For |
|---|---|---|
| `fexmac` | static library | targets in this build; CMake adds the core and its libraries |
| `fexmac_bundle` → `build/Lib/libfexmac_bundle.a` | one prelinked object in an archive | projects outside this build: `clang ... libfexmac_bundle.a -lc++` |
| `fexmac_shared` → `libfexmac.dylib` | dylib exporting only `fexmac_*` | an embedder that signs it and itself with one Team ID |
| `fexmac_testing` | static library with the test hooks | `fexmac_hook_test` only |

The standalone archive is made with `ld -r` (Apple's single-object prelink) from the
library and every archive the core needs, with an exported-symbols list, so only the
`fexmac_*` functions stay global. A program linked with it depends on `libSystem` and
`libc++` only. The build checks that none of the production forms exports a test hook
(`check_no_test_hooks.sh`).

Why static first: under the hardened runtime, a program signed ad hoc cannot load a
separately signed dylib. Library validation compares Team IDs and an ad hoc signature
has none; on this Mac dyld refuses it ("mapping process and mapped file (non-platform)
have different Team IDs").

## Tests

`fexmac_c_test` (C11, `-Wall -Wextra -Wshadow -Werror -pedantic`, only `fexmac.h`,
linked against the standalone archive). Each subtest runs in its own child process; the
driver checks how the child ended (exit 0, or death by one named signal), what it wrote
to standard error, and a value it recorded in shared memory, and reports a hang when an
alarm kills it.

| Subtest | Checks |
|---|---|
| `run` | two system calls, one answered by the callback, one stopping the run; running again continues to `hlt`; running or destroying the thread from its own callback is `BUSY` |
| `syscall-prefix` | `48 0F 05` resumes at the next instruction |
| `syscall-restart` | `RESTART` runs the call again with the state the callback set |
| `state` | defaults, lossless round trip, registers, flags, XMM and `ds` set from the host and read by the guest, `ud2` and `int3` as guest faults, CPUID |
| `abi` | `struct_size` rules for state, stop records and config; the reserved cache fields |
| `invalidate` | changed code runs after `fexmac_invalidate` |
| `faults-installed`, `faults-chained`, `faults-hosted` | misaligned `lock xadd` across 16 bytes in each handler arrangement; the host's own fault still reaches the host |
| `callret-recursion`, `callret-overflow`, `callret-underflow` | recursion 100,000 deep; 200,000 calls never returned; 200,000 returns never called |
| `guest-load-prot-none-dies`, `guest-lock-xadd-prot-none-dies`, `guest-lock-cmpxchg-prot-none-dies`, `guest-store-read-only-dies` | a guest access to a protected page dies with SIGBUS and the library's message, never loops and never claims to be a fault inside the handler |
| `guest-fault-hosted` | with the host owning the signals, the host receives the guest's fault at the guest's address |
| `callbacks` | from a callback: create a thread, destroy another stopped thread, invalidate; running another thread is `BUSY` |
| `errors` | missing arguments; unsupported options (`DISKCACHE`, `DISKCACHEPATH`, `JITFEATURES`, `GDBSERVER`, `SINGLESTEP`, `ENABLECODECACHINGWIP`, `FORCESVEWIDTH`, `STRICTINPROCESSSPLITLOCKS`); bad values (`010`, `-5`, `0`, out of range, `true`, unknown names); a second context; destroy with threads; double destroy and use after destroy; a context created again |
| `chained-mask`, `chained-resethand-dies`, `chained-handler-faults-dies` | the previous handler's mask, `SA_RESETHAND`, and a fault inside it |
| `sent-signal-ignored`, `sent-signal-default-dies` | `kill(SIGSEGV)` with the previous action "ignore" or the default |
| `unclaimed-fault-dies` | a host fault outside guest code dies silently with SIGBUS |
| `invalidate-concurrent` | 8 guest threads invalidating in every one of 20,000 system calls each while the main thread invalidates in a loop, at least 12 runs; a progress watchdog fails it within 5 seconds of a hang. It hung in each of 3 runs before package E's reader-writer lock fix and passed 6 of 6 after it |
| `lifetime-races` | runs racing with destroys of the same thread; context destroy racing with CPUID, invalidation and thread creation |

`fexmac_hook_test` (linked against `fexmac_testing`): `fault-inside-handler-dies` (the
hook fired exactly once, the process died with SIGBUS, the message was written),
`no-log-callback-in-handler`, `log-restored-after-failed-create`, `invalidation-mask`.

The tools: `FEXCoreSmokeTest` and `FEXCoreThreadTest` use only `fexmac.h` and link only
`fexmac`. `FEXCoreASMTestRunner` uses `fexmac.h` for the context, threads, state,
running, faults and invalidation; it still uses the core's C++ headers for harness work
only: `HarnessHelpers.h` (test files, register comparison on `CPUState`), the host
feature checks (`Common/HostFeatures.h`, `Common/X86Features.h`, the latter fed by
`fexmac_cpuid`), and `fextl::fmt` for its output. It forwards `FEX_*` environment
variables for supported options only.

## Constraints specific to macOS on Apple silicon

- **No guest memory below 4GB.** The system reserves the first 4GB of every 64-bit
  process and refuses to start a program that asks for less. The library refuses 32-bit
  mode.
- **A large range is reserved for the GPU**, seen from 64GB to 448GB on the hosted Mac.
- **Pages are 16K.** Guest programs assume 4K. Protections and mappings can only be set
  per 16K page.
- **Translated code needs the JIT entitlement** under the hardened runtime
  (`Data/macOS/jit.entitlements`).

## Not covered yet

- Self-modifying code detected by write-protecting pages (package D).
- Stopping a thread from another thread through the interrupt fault page (package D).
- Guest memory faults delivered as stops with reconstructed guest state (package D).
- Cached translations (package F; reserved fields above).
- Several contexts in one process (the core's configuration is process-wide).
- `FEXCoreOrderingTest` (package E) still uses the superseded `CoreThreadSetup.h` and
  `MacOSHostFaults.h` rather than `fexmac.h`; `MacOSHostFaults.h` now uses the same
  syndrome check for alignment faults.
