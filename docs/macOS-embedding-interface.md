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

fexmac_result fexmac_add_code_range(fexmac_context*, uint64_t address, uint64_t size, int writable);
fexmac_result fexmac_remove_code_range(fexmac_context*, uint64_t address, uint64_t size);
fexmac_result fexmac_context_get_counters(fexmac_context*, fexmac_translation_counters*);
fexmac_result fexmac_context_flush_cache(fexmac_context*);
fexmac_result fexmac_before_fork(fexmac_context*);
fexmac_result fexmac_after_fork(fexmac_context*, int child);
```

The header documents every function. The design decisions:

- **Every call returns a result code.** A C caller has no other way to learn about a bad
  option, a busy thread or a dead handle.
- **Structures the caller allocates carry `struct_size`** (`fexmac_config`,
  `fexmac_x86_state`, `fexmac_stop`, each with an `_init` function). The library accepts
  only sizes of layouts it knows and otherwise returns `FEXMAC_ERROR_INVALID_ARGUMENT`
  having read and written nothing, so an uninitialised structure is refused rather than
  written past. A later minor version that grows a structure keeps accepting the older
  sizes; a program built against a newer header than its library gets
  `INVALID_ARGUMENT` for a structure that grew.
- **Versions.** A program must refuse to run if `fexmac_version() >> 16` differs from
  `FEXMAC_VERSION_MAJOR`. The current interface is 2.0, the first with `struct_size`
  fields; the 1.x headers of the development branch had different layouts and nothing
  built against them is compatible.
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
  detects itself, with vector, signal, `si_code`, error code and address) and
  `UNSUPPORTED_INSTRUCTION`: the translator cannot translate the instruction at
  `instruction_address` although x86 may define it (`int 0x80` in 64-bit mode, `mov` to
  fs or gs, `o16 iret`, ...), as opposed to a guest #UD such as `ud2`, which is a
  `GUEST_FAULT` with si_code 2. It is filled like #UD (SIGILL, vector 6, si_code 0) so a
  host can still deliver it as one. Before this the host crashed at address 0. Stopping a
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
  standard error with `write(2)` instead. The callback must not call the library: the
  core can log while the library holds its registry lock, so calls from inside the
  callback are refused (`BUSY`, or NULL and no effect for the functions without a
  result) instead of deadlocking.

## Options

Only these 17 options are accepted; any other name, and any value that does not fit the
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
| `HOSTFEATURES` | `off`, or comma-separated `disable*` names the core knows | `enable*` names are refused: they force a feature on whether the host has it or not (`enablesve` dies with SIGILL on an M4) |
| `SOFTWARERNG` | 0, 1 | RDRAND and RDSEED from `arc4random_buf`; no Apple chip so far has FEAT_RNG, so without it the guest CPUID offers neither |
| `DISKCACHEMAXFILESIZE` | 1 to 2^63-1 | size limit of each code cache file (default 256MB, set by the library); only with the cache on |
| `DISKCACHEVALIDATION` | 0, 1 | compiles each cached block as well and logs differences; diagnosis only |
| `SILENTLOG`, `DUMPGPRS`, `DISABLETELEMETRY` | 0, 1 | `DISABLETELEMETRY` defaults to 1 |
| `IS64BIT_MODE` | 1 | 32-bit guests cannot have the memory they need on macOS |

Everything else is refused on purpose, for example `DISKCACHE` and `DISKCACHEPATH` (the
code cache is switched on with the `code_cache_*` config fields, below),
`STRICTINPROCESSSPLITLOCKS` (always on: see "Fault handler locks"), `GDBSERVER`,
`SINGLESTEP`, `JITFEATURES`. To add one, give it
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

1. a fault inside that thread's translator scratch-buffer guard while the translator is
   emitting a block on the thread. On macOS the core arms the guard when it starts
   emitting a block and disarms it when the block is emitted (a macOS-only line in
   `Arm64JITCore::EmitJITCode`), so an armed guard is a real "translating now" marker;
   guest code and callbacks do not run on the thread then. The thread resumes at the
   translator's restart point, and the guard is disarmed until the core arms it again for
   the retry. Guest addresses are host addresses, so without the marker a guest access or
   a callback bug touching the page would jump to a stale restart point. The guard is
   one 16K host page and the address the core records is exactly its first byte; only
   that page is claimed. The core disarms it as soon as the emitter stops writing,
   before the block is copied into the code buffer. A block that keeps overflowing is
   given up at 64MB of scratch buffer: the run then stops with
   `FEXMAC_STOP_UNSUPPORTED_INSTRUCTION` at the block's first instruction, with the core
   message `Block at ... still overflows its translation buffer`, and the thread can be
   used again;
2. a fault inside its call/return stack allocation (guard pages) raised by a load/store
   pair whose base register is `x25` (`REG_CALLRET_SP`), in translated code or the
   dispatcher, at an address within the 16 bytes it moves: the call/return stack's own
   push or pop. The pointer is reset to its starting position and the instruction
   retried, as FEX's Linux front end does. A guest access to the guard page (a wild
   pointer, a memory scan) or a host bug reading it is not claimed: resetting `x25` then
   would corrupt a host register and retry the access forever;
3. an alignment fault with the PC in translated code: the core rewrites the access. If
   the rewrite itself faults on the target (a misaligned atomic on an inaccessible page),
   that is reported as the guest's memory fault, not as a fault inside the handler.

Everything else is not the library's. A guest load or store to an unmapped or protected
page in translated code is passed on: with the library's handlers installed and nothing
installed before, the library writes `fexmac: SIGBUS: guest memory access fault in
translated code at address ...` to standard error and the process dies with the signal;
with a host handler (installed before, or owning the signals) the host receives it.
Delivering it to the guest with reconstructed state is package D.

A fault in the inaccessible page after one of the thread's code buffers is a write (or
read) running past the end of the buffer: a core bug, never the guest's doing, since
every write into a code buffer is bounds-checked first. It is not claimed either, but
the library always writes `fexmac: SIGBUS: write past the end of a code buffer, at
address ... (its guard page) from host pc ...` to standard error first, also when a
host handler receives it next.

`fexmac_handle_fault` allocates nothing, takes no lock that code outside a fault handler
holds and never calls the log callback. If the core meets an atomic instruction it cannot
rewrite (a core bug), it formats an error message, which allocates; the library writes it
with `write(2)` and the fault is not claimed.

### Fault handler locks

The handler can take two spin locks, both the core's, and nothing else:

- **The per-block patch lock** (in the block's tail), around the rewrite of a misaligned
  acquire load or release store. Held for the few instructions of a patch; no guest
  memory is touched while it is held.
- **The split-lock lock** (`StrictInProcessSplitLocks`, one word per context, so per
  process since there is one context), around each attempt of the emulation of one
  locked operation (`lock xadd`, `lock add`, `cmpxchg`, `cmpxchg8b`, `xchg`, ...) on an
  operand that crosses 16 bytes. The core emulates those with two compare-and-swaps;
  without the lock two emulations interleave and lose updates (`fexmac_c_test
  split-locks-concurrent` fails 3 of 3 runs without it). On macOS the lock is released
  and taken again between attempts, so an attempt (bounded: two compare-and-swaps, plus
  at most 16 rewrites of the lower half when only neighbouring bytes changed) is all it
  covers; plain stores that keep changing the word make the emulating thread retry, not
  hold the lock. The library always sets it on; the option is not one an embedder can
  set, because switching it off only loses updates and buys nothing measurable.

Why holding them cannot deadlock or hang, from the code (`HandleUnalignedAccess`,
`HandleAtomicMemOp`, `HandleCASAL`, `HandleCASPAL` and the `DoCAS*` helpers in
`FEXCore/Source/Utils/ArchHelpers/Arm64.cpp`):

- Each lock is taken and released within one call of the handler (a scoped guard); no
  code path leaves the handler between the two. The library never leaves its handler by
  a jump from these paths, and the only other way out of the handler, the scratch-buffer
  restart, is a different branch that takes neither lock.
- Only the fault handler takes them. A guest stop (`hlt`, a guest exception, a system
  call) is reached from the dispatcher, never from inside the handler, so neither lock is
  held when `fexmac_thread_run` returns or a callback runs.
- No asynchronous signal can interrupt the holder, in either arrangement:
  `fexmac_handle_fault` blocks every asynchronous signal (all but SIGSEGV, SIGBUS,
  SIGILL, SIGFPE, SIGTRAP and SIGSYS) itself, with `pthread_sigmask`, around the core's
  handling of a misaligned access (the only path that takes either lock or switches the
  thread to writing code), and restores the previous mask afterwards. With the library's
  own handlers they were already blocked; with way 2 the host's handler runs with the
  host's mask, and an asynchronous handler that left by a jump, or stopped the thread,
  while a lock was held would have left it held (and the thread in write mode).
  `fexmac_c_test split-locks-signal-storm` interrupts a guest thread doing split locked
  operations with a signal handler that waits for another thread's progress: with the
  blocking removed it stalls in 3 of 3 runs, with it never. A signal that arrives while
  blocked is delivered when the mask is restored, after the locks are released.
- The holder cannot need the same lock again: the emulation's own accesses are aligned
  atomics, which cannot take an alignment fault, and the patch path touches only code.
- The one fault possible while a lock is held is the split-lock emulation's access to
  guest memory that is not accessible (a misaligned locked operation on a protected or
  unmapped page). With the library's handlers this fault is reported as the guest's and
  the process dies (`fexmac_c_test guest-lock-xadd-prot-none-dies`); the lock dies with
  it. With way 2 the host's handler receives that fault while the lock is held (the
  library does not claim it): if the host returns after fixing the page, the emulation
  finishes and releases the lock; if it ends the process, nothing waits. **A host must
  not leave such a fault with `siglongjmp`**: the lock would stay held and every later
  split locked operation in the process would spin forever (the library's per-thread
  depth stays raised too, as already documented).

A thread spinning for either lock is in a fault handler with asynchronous signals
blocked; the holder is another thread doing one bounded step (a patch, or one attempt of
an emulation), so the wait ends unless the holder's process is dying.

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

### A fault inside the library's handler crashes instead of hanging

On macOS a synchronous fault while its signal is blocked hangs the thread; by default a
signal is blocked while its handler runs, so a bug in a handler used to look like a hang
(found by package A; a reproducer on the M4 hangs without `SA_NODEFER` and dies with
SIGBUS at once with it). The library's handler is installed with `SA_NODEFER`, so a
nested fault re-enters it.

Nesting is tracked only for the library's own code: a per-thread depth (a pthread key)
is raised on entry to the library's claim-and-handle section and in
`fexmac_handle_fault`, and lowered before the library calls a previous handler or
returns. That code never leaves by a jump, so the depth cannot go stale. A fault while
it is raised is a fault inside the library's handler: the library writes `fexmac: fault
inside the fault handler` (or, if the core was rewriting a misaligned atomic on an
inaccessible page, the guest memory fault message), restores the default action and
returns; the fault repeats and kills the process. The same depth diverts log messages to
`write(2)`.

A previous handler that returns gets the faulting instruction run again, and if it did
not fix the cause, the same fault again, every time. That is what macOS does with only
that handler installed (a small program whose handler returns without fixing the page
faulted 893,300 times in 2 seconds on the M4), and the library cannot know whether a
handler fixed the cause; mapping the page and returning is the common, legitimate
pattern. So the library adds no retry limit; resolving the fault or leaving is the
previous handler's responsibility, as it is without the library (`chained-retry` checks
that every retry reaches the handler).

The library does not police handlers it chains to. While a previous handler runs the
depth is zero, so a fault inside it re-enters, is not the library's, and goes to the
previous handler again, exactly as if the library were not installed. A previous handler
may leave with `siglongjmp` (the usual memory-probe idiom): the library holds no state it
would skip. A previous handler that faults on every entry recurses until the stack is
exhausted and macOS ends the process (with SIGILL when it cannot deliver the next
signal, measured: under 0.1 s); it does not hang, because the library never blocks
SIGSEGV or SIGBUS around a previous handler, even one installed without `SA_NODEFER`.
That is the one place where the library does not reproduce the kernel's behaviour, on
purpose.

Earlier designs tried to also detect faults inside the previous handler, with a flag
cleared on return (stale after `siglongjmp`), then by stack depth, then by walking the
frame-pointer chain to a recorded frame with a marker; the last gave a false "nested"
verdict in 257 of 17,990 stack layouts, where later code had a live frame record at
exactly the abandoned frame's address. Tracking only the library's own code cannot be
wrong that way.

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

## Cached translations and guest code ranges

The on-disk code cache (design, keys, checks and trust model:
`docs/macOS-code-cache-design.md`):

- **Switching it on:** `fexmac_config.code_cache_enabled = 1`, and
  `code_cache_directory` the directory (NULL: `~/Library/Caches/fex-emu/DiskCache`).
  `code_cache_enabled` other than 0 or 1, a directory with the cache off, or an empty
  name give `FEXMAC_ERROR_BAD_CONFIG`. The core's `DISKCACHE` and `DISKCACHEPATH`
  options stay refused: the fields are the interface.
- **Problems are not errors:** whatever goes wrong with the cache, the context is
  created and runs; `fexmac_context_get_counters` says what the cache is doing:
  `ACTIVE`, `READ_ONLY` (a fork child, or a database another process filled past this
  process's size limit), `UNTRUSTED` (the directory is not private to the user, has an ACL
  entry granting write, or is a link), `UNAVAILABLE` (the database stayed locked at open,
  the directory cannot be written, I/O error, no build identity). An untrusted cache file
  in a trusted directory is removed and started again: the state stays `ACTIVE` and
  `untrusted_files_replaced` counts it. The reason is always logged.
- **Counters:** `fexmac_context_get_counters` fills `fexmac_translation_counters`
  (versioned by `struct_size`): the cache state, blocks compiled, blocks loaded from the
  cache, entries rejected, entries queued, entries dropped (not saved: the database stayed
  locked past one wait, or could not grow), untrusted files replaced, damaged databases
  emptied.
- **Size:** each cache file is limited by `DISKCACHEMAXFILESIZE`, 256MB by default; a full
  file continues in a new generation, the previous one is still read, older ones are
  deleted. See the design document for why.
- **Flush:** `fexmac_context_flush_cache` waits for the cache writer and syncs the files
  (`F_FULLFSYNC`). Call it before exiting; `FEXMAC_ERROR_UNSUPPORTED` if the context does
  not write a cache. It waits for another process's lock at most once per process.
- **Guest code ranges:** `fexmac_add_code_range` / `fexmac_remove_code_range` tell the
  library which memory holds guest code (and whether the guest can write it). With any
  range registered, the core's `QueryGuestExecutableRange` reports only registered
  ranges: decoding stops at their ends, executing outside them stops with a guest fault,
  and the code cache hashes and patches guest bytes inside them only. With none, every
  address counts (the behaviour before). Register the guest's code when the cache is on.
  Package D will use the writability for page-protection self-modifying-code detection.
- **fork:** `fexmac_before_fork` / `fexmac_after_fork` around `fork()`. They hold the
  library's and the core's locks (the cache's included) across it; the child loads from
  the cache but does not write it (state `READ_ONLY`), and must not run threads created
  before the fork.
- **Not cached, by design:** code at or reaching an address with a custom IR handler
  (thunk trampolines), code overlapping instructions forced to TSO, and blocks the
  translator cannot translate (they stop with `FEXMAC_STOP_UNSUPPORTED_INSTRUCTION` every
  time). The test-only hooks (`fexmac_testing.h`) set these up for `FEXCoreCacheTest`.

## The library and how to link it

| Target | Form | For |
|---|---|---|
| `fexmac` | static library | targets in this build; CMake adds the core and its libraries |
| `fexmac_bundle` → `build/Lib/libfexmac_bundle.a` | one prelinked object in an archive | projects outside this build: `clang ... libfexmac_bundle.a -lc++` |
| `fexmac_shared` → `libfexmac.dylib` | dylib exporting only `fexmac_*` | an embedder that signs it and itself with one Team ID |
| `fexmac_testing` | static library with the test hooks | `fexmac_hook_test` and `FEXCoreCacheTest` only |

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
| `abi` | sizes 0, smaller, larger and garbage are refused for state, stop records, config and translation counters, with nothing written inside or past the structure (the reviewer's `garb.c`); bad cache fields and a set reserved field; version 2.0 |
| `invalidate` | changed code runs after `fexmac_invalidate` |
| `faults-installed`, `faults-chained`, `faults-hosted` | misaligned `lock xadd` across 16 bytes in each handler arrangement; the host's own fault still reaches the host |
| `callret-recursion`, `callret-deep-recursion`, `callret-overflow`, `callret-underflow` | recursion 100,000 deep; 1,000,000 deep counting every return (`rec.c`); 200,000 calls never returned; 200,000 returns never called |
| `guest-load-prot-none-dies`, `guest-lock-xadd-prot-none-dies`, `guest-lock-cmpxchg-prot-none-dies`, `guest-store-read-only-dies` | a guest access to a protected page dies with SIGBUS and the library's message, never loops and never claims to be a fault inside the handler |
| `guest-misaligned-atomic-prot-none-dies` | a misaligned atomic on a PROT_NONE page dies as a guest memory fault, not as a fault inside the handler |
| `guest-fault-hosted` | with the host owning the signals, the host receives the guest's fault at the guest's address |
| `callbacks` | from a callback: create a thread, destroy another stopped thread, invalidate; running another thread is `BUSY` |
| `errors` | missing arguments; unsupported options (`DISKCACHE`, `DISKCACHEPATH`, `JITFEATURES`, `GDBSERVER`, `SINGLESTEP`, `ENABLECODECACHINGWIP`, `FORCESVEWIDTH`, `STRICTINPROCESSSPLITLOCKS`); bad values (`010`, `-5`, `0`, out of range, `true`, unknown names); a second context; destroy with threads; double destroy and use after destroy; a context created again |
| `chained-retry` | a previous handler that returns without fixing the fault sees every retry (1,000) until it fixes the page |
| `chained-mask`, `chained-resethand-dies` | the previous handler's mask (with SIGSEGV/SIGBUS not blocked) and `SA_RESETHAND` |
| `chained-handler-faults-dies`, `chained-handler-faults-deferring-dies`, `chained-handler-faults-no-frame-pointer-dies` | a previous handler that faults, with and without `SA_NODEFER` and with and without frame pointers: the process dies (stack exhausted) within the subtest's time limit, and is not reported as a fault inside the library's handler |
| `chained-siglongjmp` | a previous handler that leaves with `siglongjmp`, three times in a row (`ljmp.c`): no fault is taken for nested, a misaligned atomic is still handled, the log callback is still called |
| `chained-siglongjmp-layouts` | the reviewer's `ljmp4` sequence over 306 stack layouts (a reduced `ljmp2` grid): probe, abandoned handler frame, later probes above, below and at its depth; every layout survives |
| `sent-signal-ignored`, `sent-signal-default-dies` | `kill(SIGSEGV)` with the previous action "ignore" or the default |
| `unclaimed-fault-dies` | a host fault outside guest code dies silently with SIGBUS |
| `scratch-overflow-sizes` | blocks of 96 to 416 `rep movsq`, one instruction longer each time, each in a fresh context (a fresh, exactly sized scratch buffer), across the size where the first translation starts to overflow, then blocks of 2,048 to 8,192; every result exact |
| `scratch-overflow-threads` | 8 threads translating and running 12 large blocks each at the same time, sharing the code buffer |
| `code-buffer-switch`, `code-buffer-switch-cached` | one large block translated 160 times (invalidated in between), well past the first 16MB code buffer, so blocks that do not fit in what is left move to new, larger buffers; the second with the code cache on, so the copies are loaded from the cache by copy |
| `invalidate-concurrent` | 8 guest threads invalidating in every one of 20,000 system calls each while the main thread invalidates in a loop, at least 12 runs; a progress watchdog fails it within 5 seconds of a hang. It hung in each of 3 runs before package E's reader-writer lock fix and passed 6 of 6 after it |
| `split-locks-concurrent` | 8 threads at once run 30,000 `lock xadd` each on one qword crossing 16 bytes, a 128-byte line and the 16K page, a `lock cmpxchg` increment loop across 16 bytes (each adding 0x100000001, which changes both halves), and a spin lock taken with `xchg` across 16 bytes and released by a plain store; every total must be exact. With the split-lock lock switched off in a scratch build, the three `lock xadd` totals came out short in 3 of 3 runs |
| `split-locks-every-form` | every form the core emulates across 16 bytes, from 8 threads at once, 10,000 operations each: `lock xadd` of 16, 32 and 64 bits, `lock add`, `sub`, `xor`, `inc`, `dec`, `not`, `neg`, `or` then `and`, a `cmpxchg8b` increment loop, a `cmpxchg8b` spin lock with a plain release and an `xchg` spin lock whose value spans both halves; operands change both halves where the operation allows it; a host thread meanwhile writes the neighbouring bytes that share the operand's compare-and-swap units with plain stores. Every result exact. Before the split-lock fixes of package E's third round, 7 of the 13 forms came out wrong in each of 2 runs |
| `split-locks-signal-storm` | the host owns the fault signals with an empty mask and a signal handler keeps interrupting one of 8 threads doing split `lock xadd`; the handler waits for another thread's progress, which cannot come if the interrupted thread holds the split-lock lock. No stall, exact total. With `fexmac_handle_fault`'s signal blocking removed: a stall in 3 of 3 runs |
| `lifetime-races` | runs racing with destroys of the same thread; context destroy racing with CPUID, invalidation and thread creation |
| `cache-off` | without the cache: state `OFF`, blocks compiled, nothing loaded or queued, flush `UNSUPPORTED` |
| `cache-cold-warm` | a cold run queues and flushes; a second context in the same process loads everything and compiles nothing (code at a different address) |
| `cache-cross-process` | a child process fills the cache; the parent then loads everything and compiles nothing |
| `cache-untrusted` | a directory writable by others: the context runs with state `UNTRUSTED`, nothing loaded or queued, flush `UNSUPPORTED`, the directory untouched |
| `cache-untrusted-file` | an index file opened up to others: removed and started again, `untrusted_files_replaced` 1, state `ACTIVE` |
| `cache-unavailable` | a directory that cannot be written, and a database another process keeps locked: state `UNAVAILABLE`, the context runs |
| `cache-options` | `DISKCACHE` and `DISKCACHEPATH` refused; `DISKCACHEMAXFILESIZE` and `DISKCACHEVALIDATION` accepted with their rules |
| `cache-fork` | `fexmac_before_fork` / `fexmac_after_fork`: a second `before` is `BUSY`; the child runs guest code with state `READ_ONLY` and flush refused; the parent carries on and flushes |
| `code-ranges` | empty and wrapping ranges refused; jumping to an unregistered page stops with a guest fault there; registering it lets the program finish; removing it again (with its translations) faults again |

`fexmac_hook_test` (linked against `fexmac_testing`, whose hooks also report a thread's
guard-page addresses and scratch-buffer overflows, set the scratch buffer's first size
and limit, and overrun a code buffer on purpose):

| Subtest | Checks |
|---|---|
| `guest-load-callret-low-guard-dies`, `guest-store-callret-high-guard-dies` | a guest load or store on a call/return guard page dies as a guest memory fault; no reset, no loop (`guard2.c`) |
| `scratch-guard-disarmed-after-translation` | the scratch-buffer guard is disarmed once a block is emitted, so no later access to that page can be taken for the translator's overflow |
| `scratch-overflow-many` | with the default first size, 96 `rep movsq` fit and 416 overflow (the premise of `scratch-overflow-sizes`); starting at 1, 6 and 12 bytes per IR node, translations overflow 0, 1, 2 and up to 5 times and every block runs correctly with the guard disarmed afterwards |
| `scratch-limit-stops` | with a 64K limit a block that does not fit stops with `UNSUPPORTED_INSTRUCTION` at its start after several overflows, with the core's message; the guard is disarmed; with the limit lifted the same thread translates and runs it |
| `code-buffer-guard-pages` | three code buffers in turn (filled by one large block translated again and again) each end in an inaccessible page with executable code before it |
| `code-buffer-overrun-dies`, `code-buffer-overrun-chained` | a write running past the end of the code buffer with the thread switched to writable (from the system call callback) faults on the guard page; the process dies with the library's message, or, with a host handler installed before the library, the message is written and the host receives the fault at the guard address |
| `callback-callret-guard-passed-on` | the system call callback reading a call/return guard page: the host's handler receives the fault at that address |
| `log-callback-reentry` | fexmac calls from inside the log callback return `BUSY`; afterwards they work |
| `fault-inside-handler-dies` | the hook fired exactly once, the process died with SIGBUS, the message was written |
| `no-log-callback-in-handler`, `log-restored-after-failed-create`, `invalidation-mask` | as named |

The tools: `FEXCoreSmokeTest` and `FEXCoreThreadTest` use only `fexmac.h` and link only
`fexmac`. `FEXCoreASMTestRunner` uses `fexmac.h` for the context, threads, state,
running, faults and invalidation; it still uses the core's C++ headers for harness work
only: `HarnessHelpers.h` (test files, register comparison on `CPUState`), the host
feature checks (`Common/HostFeatures.h`, `Common/X86Features.h`, the latter fed by
`fexmac_cpuid`), and `fextl::fmt` for its output. It forwards `FEX_*` environment
variables for supported options only. With `FEX_DISKCACHE=1` it switches the code cache on through the
`code_cache_*` fields (directory from `FEX_DISKCACHEPATH`), registers every page it
mapped as a code range, flushes before exiting and prints the translation counters.
`FEXCoreCacheTest` uses `fexmac.h` and the test hooks of `fexmac_testing.h` only.

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
- Before resuming a guest from the core's synchronous-fault entry (package D's guest
  fault delivery), reset the host stack: the route the core takes there for untranslatable
  code from `ExitFunctionLink` (`UntranslatableFault` in `Core.cpp`) does not, unlike
  `DEF_OP(Break)`, so call/return entries stay on it. Harmless today because the handler
  never returns.
- The fault-handler contract does not say what happens when the library chains to a
  previously installed handler and that handler returns without resolving the fault: the
  library's handler returns too and the instruction faults again, forever. It should end
  in death by the original signal. The test runner avoids it by owning the signals (way 2).
- Several contexts in one process (the core's configuration is process-wide).
