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
| `Library.cpp` | Contexts, threads, guest state, running guest code, system calls, invalidation. |
| `Options.cpp` | Checks and converts configuration options. |
| `Faults.cpp` | Host fault handling and the signal-handler contract. |
| `fexmac_testing.h` | A test-only hook, not part of the interface. |
| `tests/CApiTest.c` | The C test program (see "Tests"). |

## What the library does for the embedder

Each duty below was found by writing the test tools; each used to be done by every tool
and is now done once, in the library.

| Duty | Why | Where |
|---|---|---|
| Set up the core's configuration from the embedder's options, check every option | The core's own loaders turn a bad number into 0 and can stop the process later on a value of the wrong kind | `fexmac_context_create`, `Options.cpp` |
| Detect host features, create the context, give it a system call handler and a signal delegator, call `InitCore` | Required by the core | `fexmac_context_create` |
| Provide descriptor tables (GDT and LDT, 8192 null entries) and a 64-bit code segment | Segment loads read them | one table per context |
| Create a thread state per guest thread, allocate its call and return stack with guard pages | Guest `call` and `ret` crash without the stack | `fexmac_thread_create` |
| Call the allocator's per-thread initialisation on each host thread that creates or runs guest threads | Required before a thread allocates | `fexmac_thread_create`, `fexmac_thread_run` |
| Set `SynchronousExceptionHandler` and turn what it reports into a stop reason | The core calls it for `hlt`, `ud2`, `int3` and the like | `fexmac_thread_run` |
| Advance `rip` past `syscall` | The core leaves `rip` on the instruction; the handler must skip it | system call handler |
| Discard stale translations, holding the core's invalidation lock, for every thread | Self-modifying code loops forever otherwise; the core asks through `InvalidateGuestCodeRange` | `fexmac_invalidate` and the core's callback |
| Catch host alignment faults in translated code and pass them to the core | Misaligned atomic operations fault on ARM64 and the core rewrites them | `fexmac_handle_fault` |
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

The header documents every function. Points that differ from the sketch this page used
to carry:

- **Every call returns a result code.** The sketch returned handles and `void`. A C
  caller has no other way to learn about a bad option, a busy thread or a dead handle.
- **Configuration is a list of name/value options**, named as the core's environment
  variables without `FEX_` (`MULTIBLOCK`) or as its configuration file (`Multiblock`).
  A sized struct (`struct_size`, filled by `fexmac_config_init`) leaves room for later
  fields. Unknown names, values that do not parse, booleans other than 0 or 1, unknown
  named values and 32-bit mode are refused with `FEXMAC_ERROR_BAD_CONFIG` before
  anything process-wide changes. The library sets `DISABLETELEMETRY=1` unless told
  otherwise. Configuration files and environment variables are not read: the embedder
  decides what reaches the core (the instruction-test runner forwards `FEX_*`
  variables itself).
- **System calls: a callback, with stopping as an option.** The sketch had
  `fexmac_thread_run` return for every system call. The library supports that (no
  callback set: every `syscall` stops the run with `FEXMAC_STOP_SYSCALL`, `rip` after
  the instruction, and running again resumes) and also a callback that answers the call
  in place and returns `FEXMAC_SYSCALL_RESUME`, or `FEXMAC_SYSCALL_STOP` to leave. The
  callback avoids leaving and re-entering the dispatcher for every call.
- **Leaving the dispatcher uses the core's own exit**, not `longjmp`. The dispatcher
  saves its stack pointer on entry (`ReturningStackLocation`) and has a stop handler
  that restores the host's registers and returns from `ExecuteThread`; Linux FEX uses it
  to stop threads. The library resets the stack to the saved location and jumps to the
  stop handler. This is only done from the system call and synchronous exception
  handlers, where the core has already written the guest registers back to the frame,
  so the stop handler's non-spilling entry is correct. The test tools no longer
  `longjmp` out of translated code.
- **Stop reasons:** `FEXMAC_STOP_SYSCALL`, `FEXMAC_STOP_HALT` (a user-mode `hlt`, which
  the core reports as a general protection fault at the instruction) and
  `FEXMAC_STOP_GUEST_FAULT` (an exception the core detects itself, with vector, signal,
  `si_code`, error code and address as the core reports them). Stopping a thread from
  another thread and guest faults caught as host faults are package D.
- **Guest state** (`fexmac_x86_state`, fixed layout for version 1): general registers,
  `rip`, `rflags` as `pushf` would see it, FS and GS bases, MXCSR, x87 control, status
  and abridged tag words, MM0 to MM7 in physical order, XMM and the upper halves of
  YMM. Reading and writing go through the core's own reconstruction of flags and vector
  registers. Get, set, get gives identical state (tested).
- **One context per process.** The core's configuration, its log hooks and the fault
  handlers are process-wide, so a second `fexmac_context_create` returns
  `FEXMAC_ERROR_BUSY` until the first context is destroyed. A context can be destroyed
  and created again.
- **Handles are checked against tables of live handles** before use, so a destroyed or
  never-created handle gives `FEXMAC_ERROR_INVALID_HANDLE` instead of a crash. This
  holds until the memory is reused for a new handle of the same kind; C cannot do
  better without handle indirection, which would cost a lookup in the fault handler.
- **Threads:** a guest thread may run on any host thread, one at a time
  (`FEXMAC_ERROR_BUSY` otherwise). State may be read and written while it is stopped
  or from its own system call callback. From a callback, running, creating or
  destroying threads returns `FEXMAC_ERROR_BUSY`. Destroying a context with threads
  left returns `FEXMAC_ERROR_BUSY`.
- **`fexmac_cpuid`** is new: the test runner needs the guest's view of CPUID to decide
  which tests can run, and an embedder will need it too.

## Host faults and signal handlers

Translated code takes some host faults on purpose (misaligned atomics, the scratch
buffer's end). The library handles them only when they come from a host thread that is
inside `fexmac_thread_run`, so it never claims a fault that belongs to the host. The
current guest thread is kept in a pthread key rather than a C++ `thread_local`, because
on macOS the first access to a `thread_local` on a thread can allocate memory, which
must not happen inside a signal handler.

There are two ways to connect the library, one per process:

1. **The library installs the handlers** (`fexmac_install_fault_handlers`), for SIGBUS
   and SIGSEGV, with `SA_SIGINFO | SA_ONSTACK | SA_NODEFER`, and keeps whatever was
   installed before. A fault that is not the library's goes to that previous handler
   (called with the same three arguments, without its own `sa_mask` being applied); if
   the previous action was the default or "ignore", the default is put back and the
   instruction faults again, so the process dies with the usual signal. A host that
   installs handlers after the library must call `fexmac_handle_fault` from them or
   chain to the action it replaced.
2. **The host owns the signals** (a console compatibility layer with its own handlers)
   and calls `fexmac_handle_fault(signal, info, ucontext)` first in its handlers for
   SIGBUS and SIGSEGV. A return of 1 means the library has edited the context and the
   host must return at once.

`fexmac_handle_fault` is async-signal-safe: it takes no locks other than the core's own
patching lock (taken only for a misaligned atomic in translated code) and allocates
nothing.

**A fault inside the handler crashes instead of hanging.** On macOS a synchronous fault
while its signal is blocked hangs the thread; by default a signal is blocked while its
handler runs, so a bug in the handler used to look like a hang (found by package A). The
library installs its handler with `SA_NODEFER`, so a nested fault re-enters it; the
handler sees that it is nested (a per-thread depth count), writes a message to standard
error, restores the default action and returns, and the fault repeats and kills the
process. A small reproducer on the M4 shows the difference: without `SA_NODEFER` the
process was still hanging when an alarm killed it after 5 seconds, with it the process
died with SIGBUS at once. A host using way 2 should install its own handlers with
`SA_NODEFER` too.

### Where later packages extend the fault handling

`fexmac_handle_fault` runs a fixed sequence of checks (`Faults.cpp`). Package D adds to
that sequence: the interrupt fault page (stopping a thread from another thread, which
will reuse the same dispatcher exit as the system call stop and add a stop reason),
write faults on pages protected for self-modifying-code detection (together with
`SyscallHandler::QueryGuestExecutableRange` and `MarkGuestExecutableRange` in
`Internal.h`, which today report all memory as executable and writable), and guest
faults with reconstructed guest state, reported as a new stop reason. Package F (cached
code loading) configures the code cache where `fexmac_context_create` sets up the
configuration, and answers `LookupExecutableFileSection`. None of these exist yet; there
are no functions that pretend to do them.

## The library and how to link it

Three forms of the same code (`Source/Embedding/CMakeLists.txt`):

| Target | Form | For |
|---|---|---|
| `fexmac` | static library | targets in this build; CMake adds the core and its libraries |
| `fexmac_bundle` → `build/Lib/libfexmac_bundle.a` | one prelinked object in an archive | projects outside this build: `clang ... libfexmac_bundle.a -lc++` |
| `fexmac_shared` → `libfexmac.dylib` | dylib exporting only `fexmac_*` | an embedder that signs it and itself with one Team ID |

The standalone archive is the main deliverable. It is made with `ld -r` (Apple's
single-object prelink) from the library and every archive the core needs, with an
exported-symbols list, so only the `fexmac_*` functions stay global; the core, its
bundled `fmt`, `xxhash` and the rest become private and cannot clash with the embedder's
own copies. A program linked with it depends on `libSystem` and `libc++` only.

Why static first: under the hardened runtime, a program signed ad hoc cannot load a
separately signed dylib. Library validation compares Team IDs and an ad hoc signature
has none; on this Mac dyld refuses it ("mapping process and mapped file (non-platform)
have different Team IDs"). The test tools and the C test are signed ad hoc, so they link
statically. An embedder with a developer identity can use the dylib.

## Tests

- `fexmac_c_test` (`tests/CApiTest.c`, C11, `-Wall -Wextra -Werror -pedantic`) uses
  only `fexmac.h` (plus the test hook) and links the standalone archive as an outside
  project would. Each subtest runs in its own child process, killed by an alarm if it
  hangs:
  - `run`: a guest loop with two system calls, one answered by the callback (the result
    reaches the guest), one stopping the run; running again continues to `hlt`; calls
    the callback must not make return `FEXMAC_ERROR_BUSY`.
  - `state`: defaults, a lossless get/set round trip, registers, flags and XMM set from
    the host and read by the guest, `ud2` and `int3` reported as guest faults, CPUID.
  - `invalidate`: changed guest code runs after `fexmac_invalidate` (and the stale
    translation runs without it).
  - `faults-installed`, `faults-chained`, `faults-hosted`: a misaligned `lock xadd`
    across a 16-byte boundary gives the x86 result with the library's handlers, with the
    library's handlers chained over the host's (the host's own fault still reaches the
    host), and with the host's handler forwarding to `fexmac_handle_fault`.
  - `errors`: missing arguments, a wrong struct size, unknown options, unparsable and
    out-of-range values, 32-bit mode, a second context, destroying a context with
    threads, double destroy and use after destroy of threads and contexts, a context
    created again after destruction.
  - `unclaimed-fault-dies`: a fault nobody handles kills the process normally.
  - `fault-inside-handler-dies`: with the test hook making the library's handler fault,
    the process dies with SIGBUS instead of hanging.
- `FEXCoreSmokeTest` and `FEXCoreThreadTest` use only `fexmac.h`; they link `fexmac`
  and nothing else, so the core's headers are not even on their include path.
- `FEXCoreASMTestRunner` uses `fexmac.h` for the context, threads, state, running,
  faults and invalidation. It still uses the core's C++ headers for harness work only:
  - `HarnessHelpers.h` reads the test files and compares registers laid out as the
    core's `CPUState`; the runner copies `fexmac_x86_state` into one for it.
  - `Common/HostFeatures.h` and `Common/X86Features.h` decide whether the host can run a
    test (the latter now accepts a CPUID function, fed by `fexmac_cpuid`).
  - `fextl::fmt` and `LogMan::DebugLevelStr` for its output.

## Constraints specific to macOS on Apple silicon

- **No guest memory below 4GB.** The system reserves the first 4GB of every 64-bit
  process and refuses to start a program that asks for less. A guest that needs low
  addresses (32-bit code, or a binary linked at a low fixed address) cannot be given them
  directly. Relocatable guest code is unaffected. The library refuses 32-bit mode.
- **A large range is reserved for the GPU**, seen from 64GB to 448GB on the hosted Mac.
- **Pages are 16K.** Guest programs assume 4K. Protections and mappings can only be set
  per 16K page.
- **Translated code needs the JIT entitlement** when the program uses the hardened
  runtime (`Data/macOS/jit.entitlements`).

## Not covered yet

- Self-modifying code detected by write-protecting pages (package D). Only the
  check-on-every-block mode and explicit invalidation work.
- Stopping a thread from another thread through the interrupt fault page (package D).
- Guest faults caught as host faults, with reconstructed guest state (package D).
  Until then a guest access to unmapped memory is an ordinary host crash.
- Loading cached translations (package F).
- Several contexts in one process (the core's configuration is process-wide).
