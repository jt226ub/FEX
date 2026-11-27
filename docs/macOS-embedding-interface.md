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
| `Memory.cpp` | The guest memory map, executable ranges for the translator, write protection of translated code, page-fault details. |
| `fexmac_testing.h` | Test-only hooks, compiled only into the `fexmac_testing` build. |
| `tests/` | `CApiTest.c` and `FaultHandlingTest.c` (`fexmac_c_test`), `HookTest.c` (`fexmac_hook_test`), `TestSupport.c`. |

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
| Turn a guest memory access fault in translated code into a `GUEST_FAULT` stop, with the guest state written back from the host context and x86 page-fault details | The process died otherwise | `fexmac_handle_fault`, `Memory.cpp` |
| Keep the guest's view of its memory (protections per 4K guest page) and answer the core's executable-range queries from it | Fetch faults, #PF details, write protection of code | `fexmac_memory_*` |
| Write-protect translated pages and discard their code when the guest writes them (`SMCCHECKS mtrack`) | Self-modifying code without a check in every block | `fexmac_handle_fault`, `Memory.cpp` |
| Stop a running thread at a safe point when another thread asks | Embedders suspend threads (signals, garbage collection, debugging) | `fexmac_thread_request_stop` |

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
fexmac_result fexmac_thread_request_stop(fexmac_thread*);          /* async-signal-safe */

fexmac_result fexmac_memory_map(fexmac_context*, uint64_t address, uint64_t size, uint32_t prot);
fexmac_result fexmac_memory_protect(fexmac_context*, uint64_t address, uint64_t size, uint32_t prot);
fexmac_result fexmac_memory_unmap(fexmac_context*, uint64_t address, uint64_t size);
fexmac_result fexmac_memory_query(fexmac_context*, uint64_t address, uint32_t* prot, int* mapped);
fexmac_result fexmac_memory_write_begin(fexmac_context*, uint64_t address, uint64_t size);
fexmac_result fexmac_memory_write_end(fexmac_context*, uint64_t address, uint64_t size);
uint64_t      fexmac_memory_code_write_count(fexmac_context*);

fexmac_result fexmac_install_fault_handlers(void);
int           fexmac_handle_fault(int signal, siginfo_t*, void* ucontext);

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
  host can still deliver it as one. Before this the host crashed at address 0.
  `GUEST_FAULT` with trap 14 is a guest page fault (see "Guest memory faults" below) and
  `REQUESTED` answers `fexmac_thread_request_stop`. The stop record (`fexmac_stop`, 48
  bytes) has `access` (`FEXMAC_ACCESS_READ`, `_WRITE`, `_EXECUTE` for a page fault, 0
  otherwise) after `fault_address`; it was added to the 2.0 layout before 2.0 shipped,
  and the 40-byte draft size is refused (`abi`).
- **Every stop leaves the dispatcher the same way**, through its stop handler with the
  stack pointer reset to where the dispatcher saved it on entry, and every run enters it
  afresh. So after any stop the host stack and the call/return stack are consistent and
  the embedder may run again from `rip`, or set any state first (a signal frame it
  built). This is also why the core's untranslatable-code route, which reaches the
  synchronous-fault entry without resetting the host stack (`UntranslatableFault` in
  `Core.cpp`), needs nothing more: the library never resumes in place from that entry.
- **One context per process.** The core's configuration, its log hooks and the fault
  handlers are process-wide, so a second `fexmac_context_create` returns `BUSY` until the
  first context is destroyed. A context can be destroyed and created again. A failed
  create leaves nothing process-wide changed, log hooks included.
- **Code buffers.** Translated code goes into the context's code buffers (16MB first,
  each next one twice the last, up to 128MB, each ending in an inaccessible 16K page).
  The first is made with the context's first `fexmac_thread_create`; if it cannot be
  made (`mmap` of `MAP_JIT` memory fails, or the guard page cannot be placed), that call
  returns `FEXMAC_ERROR_OUT_OF_MEMORY` with the reason logged (`Couldn't make a 16 MiB
  code buffer: couldn't ... (errno ...)`) and a later call can try again. Later buffers
  are made while translating, where there is no way back to the embedder: a failure then
  ends the process with that message at assert level. One block may have at most 64MB
  of host code (half the largest buffer, so it always fits a fresh one): the translator
  gives up on larger blocks, a code-cache entry claiming more is rejected (and the
  block compiled instead), and the core never retries buffers for a size that cannot
  fit.
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
| `SMCCHECKS` | `none`, `mtrack`, `full` (or 0, 1, 2) | default `mtrack`: with no guest memory registered nothing is detected (the embedder calls `fexmac_invalidate`, as on `macos/v1`); with memory registered, translated pages are write-protected (see "Self-modifying code"). `full`: a check of the guest bytes before every instruction, everywhere. `none`: nothing |
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
3. a write to the thread's interrupt fault page, which another thread made read-only to
   stop this one (see "Stopping a thread from another thread");
4. an alignment fault with the PC in translated code: the core rewrites the access. The
   memory is checked first: if the access touches a page holding write-protected code,
   that is item 5; if the host does not allow it, it is the guest's page fault (item 6),
   reported from this handler before anything is written (see "Fault handler locks");
5. a guest write, from translated code, to a page the library write-protected because
   code on it was translated (see "Self-modifying code");
6. any other memory access fault (translation or permission fault) with the PC in
   translated code: the guest's own page fault, delivered as a stop.

Everything else is not the library's: faults in host code (the embedder, its system call
callback, a bug in the core), including a host write to a write-protected code page,
for which the library first writes `fexmac: SIGBUS: host code wrote guest address ...
bracket such writes with fexmac_memory_write_begin and fexmac_memory_write_end`. With
the library's handlers and nothing installed before, the process dies with the signal;
with a host handler (installed before, or owning the signals) the host receives it.

A fault in the inaccessible page after one of the thread's code buffers, from the core's
own code (not translated code or the dispatcher), is a write (or read) running past the
end of the buffer: a core bug, since every write into a code buffer is bounds-checked
first. It is not claimed either. With the library's handlers installed, the library
writes `fexmac: SIGBUS: write past the end of a code buffer, at address ... (its guard
page) from host pc ...` to standard error first, also when a handler installed before
the library's receives it next. A host that owns the signals and calls
`fexmac_handle_fault` gets no such line. A guest access from translated code that
happens to hit the page (guest addresses are host addresses) is the guest's fault and
is reported as any other guest memory fault.

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
  `fexmac_hook_test misaligned-blocks-async-signals` checks it deterministically: a test
  hook sends the thread a signal at the point where the core is about to take its locks,
  and that signal's handler must not run before the handling ends (without the blocking
  it runs inside every time). `fexmac_c_test split-locks-signal-storm` is the stress
  version; it rarely catches the missing blocking, since a signal sent to a running thread
  is mostly delivered at its next fault, before the core's handling starts. A signal that
  arrives while blocked is delivered when the mask is restored, after the locks are
  released.
- The holder cannot need the same lock again: the emulation's own accesses are aligned
  atomics, which cannot take an alignment fault, and the patch path touches only code.
- No fault is taken while a lock is held, by construction. Before the core emulates a
  misaligned atomic, the library checks that the host allows the access to every byte
  it touches (the kernel's region query, `mach_vm_region`: user space has no
  non-faulting probe instruction). If not, it reports the guest's page fault from this,
  the outer, handler, with the guest state from the outer context (rip at the locked
  instruction, the fault address the first inaccessible byte) and nothing written; the
  embedder fixes the page and runs again, and the instruction is emulated then
  (`split-lock-faults`: `lock xadd`, `lock cmpxchg`, `xchg` with the second half on a
  PROT_NONE or read-only page, four threads, totals exact, a further round afterwards).
  The check stays true until the emulation ends because every protection reduction the
  library makes (write-protecting translated code, `fexmac_memory_map/protect/unmap`)
  first raises a counter of pending changes and waits until no emulation is in
  progress, and an emulation that starts while a change is pending backs off and lets
  the instruction fault again (`GuestMemory::ReduceProtection`; lock-free, sequentially
  consistent counters in both directions). An emulation on a page whose write
  protection is being taken away by another thread is the self-modifying-code path
  instead (`split-lock-on-code-page`: four threads, `lock xadd` across 16 bytes on their
  own code page, about 4,000 write-protection round trips, total exact; it lost a fifth
  of the increments before the library gave write access back before clearing its mark).
- Asynchronous signals are blocked by the library itself (`pthread_sigmask`) while the
  core emulates, also when the host owns the signals and installed its handler with a
  narrower mask, so an asynchronous handler cannot interrupt the lock holder (and leave
  with `siglongjmp` or suspend the thread). The library adds no thread-stop signal: stop
  requests use the interrupt fault page.
- Left uncovered: the embedder changing the page with a raw `mprotect` or `munmap` at
  that moment (outside the contract, which asks it to use `fexmac_memory_*`). The nested
  fault is then claimed by `fexmac_handle_fault` in both arrangements (it never reaches
  a host handler, which might leave it with `siglongjmp` and the lock held), the library
  writes `fexmac: ... fault while emulating a misaligned atomic ... ending the process`
  and the process dies by the signal (`fexmac_hook_test split-emulation-fault-dies`,
  `split-emulation-fault-hosted-dies`, which remove access between the check and the
  emulation with a test hook). A fault-recoverable emulation (access primitives with a
  fixup address in the core's `Arm64.cpp`) would turn that into a guest fault too; it is
  a change to every access of the core's emulation and was not needed for anything the
  contract allows.

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

### Leaving translated code from the fault handler

The check sequence is in `Faults.cpp`. Items 3 to 6 above leave the translated code they
interrupted. At a fault in translated code the guest registers the translator keeps in
host registers (its static register allocation: the 16 GPRs, the low halves of the 16
XMM registers, NZCV, the PF and AF registers, the call/return stack pointer `x25`) are in
the signal context. `SpillGuestState` writes them to the thread's frame as the core's own
spill would (the mapping comes from `SignalDelegatorConfig.SRAGPRMapping`), takes `rip`
from the block's table of guest instruction starts (`RestoreRIPFromHostPC`) and the flags
from PSTATE and the PF/AF registers (`ReconstructCompactedEFLAGS`), as FEX's Linux
`SignalDelegator::SpillSRA` does. Then the handler edits the context so that returning
from the signal goes to one of:

- the dispatcher's stop handler (`ThreadStopHandlerAddress`), with the stack pointer at
  `ReturningStackLocation`: `fexmac_thread_run` returns with the stop record the handler
  filled (guest faults, stop requests);
- a trampoline in the library that runs in ordinary context on the dispatcher's stack and
  then re-enters the dispatcher at its fill-state entry (`AbsoluteLoopTopAddressFillSRA`),
  optionally with the single-instruction flag (self-modifying code);
- the fill-state entry directly (a thread whose code was discarded by another thread).

In every case the translator's FPCR modes (NEP, AH, FIZ) are cleared from the context
when host code runs next. Nothing in the handler allocates, takes a lock or logs except
with `write(2)`; the work that needs locks runs in the trampoline, after the signal has
returned.

## Guest memory

The embedder maps guest memory itself (guest addresses are host addresses) and tells the
library the guest's view with `fexmac_memory_map` (a new mapping, or any range with a new
protection), `fexmac_memory_protect` (a guest `mprotect` of a registered range),
`fexmac_memory_unmap`. Protections are `FEXMAC_PROT_READ`, `_WRITE`, `_EXEC`; addresses
and sizes are multiples of 4K and below 2^47. The library keeps one byte per 4K guest
page (two-level table, 1GB chunks created on first use, freed with the context) and one
per 16K host page (whether it write-protected the page for code), read lock-free by the
fault handler.

- **Before the first `fexmac_memory_map`** nothing changes: every address is executable
  code to the translator, nothing is write-protected, guest page faults are described
  from the host fault alone, and an embedder that changes code calls `fexmac_invalidate`.
- **Afterwards** the library sets each host page's protection itself, to the union of
  its four guest pages' protections (minus write while it holds translated code); x86
  has no write-only pages, so write or execute implies read, and guest memory is never
  executed by the host. The embedder must not `mprotect` registered memory itself (that
  would undo write protection without discarding code) and must report every guest
  `mmap`, `mprotect` and `munmap`.

  **The rule.** A guest page needs from its host page: no access (unmapped or
  PROT_NONE), read (R, X or R+X), or read and write (any W). EXEC is checked by the
  library per 4K page from its table. A change is enforced exactly when, afterwards, the
  four guest pages of every host page it touches need the same; then it is accepted.
  Accepted and enforced: R next to R+X (.text next to .rodata), W next to RW, PROT_NONE
  next to unmapped. Refused (`FEXMAC_ERROR_UNSUPPORTED`, nothing changed) unless the
  protection carries `FEXMAC_PROT_ALLOW_UNENFORCED`: R next to RW, PROT_NONE next to
  anything accessible, and a 4K mapping into an otherwise unmapped host page (its
  unmapped neighbours would become accessible), so a guest `mmap` whose size or place is
  not a multiple of 16K needs the flag or a 16K-rounded layout. With the flag the host
  page gets the union and `fexmac_memory_unenforced_count` counts it. A
  `fexmac_memory_unmap` of part of a host page whose other guest pages stay accessible
  is done (the pages are unmapped for the library and reported so) and returns
  `FEXMAC_DONE_UNENFORCED`, also counted, so an embedder cannot miss that the guest can
  still reach them. Tests: `memory-host-page-rules`, `memory-arguments`.

  **This is the 1.0 behaviour and a real limit.** On a host page whose guest pages need
  different host protections the guest gets the most permissive for all four. Not
  enforced there: PROT_NONE guard pages and unmapped 4K pages next to accessible memory
  (a stack guard, a red zone, an end-of-buffer guard) are readable, and writable next to
  writable memory, without a fault; read-only pages next to writable ones are writable.
  Self-modifying-code write protection (`mtrack`) does not cover a guest page that is not
  guest-writable but shares a host page with a writable one: a write to it goes through
  the union unseen, so the embedder uses `SMCCHECKS full` or calls `fexmac_invalidate`.
  Still enforced everywhere: mapped/unmapped and protection reporting, and instruction
  fetch only from guest-executable 4K pages. An embedder that keeps each host page's
  guest pages to one need never meets the limit; `fexmac_memory_unenforced_count` says
  whether it did.

  Future work, the sound scheme: give the host page the *most restrictive* protection of
  its guest pages and have the library complete, in the fault handler, each access the
  guest's own 4K protection allows: decode the faulting instruction, check the access
  against the guest page, perform it (through a second, fully accessible mapping of the
  memory) and step over it, or retranslate that instruction to go through the alias.
  Cost: every allowed access to the more permissive pages of a mixed host page becomes a
  signal round trip (several microseconds on the M4, against about a nanosecond), so a
  hot data page next to a guard page runs thousands of times slower; and the handler
  needs an emulator for every memory-accessing x86 form (vector, x87, string, atomic) or
  a retranslation path per faulting instruction. Weeks of work and a test matrix of its
  own; not done for 1.0.
- **`fexmac_memory_unmap`** makes a host page with no mapped guest page left inaccessible
  itself, waiting for misaligned-atomic emulations in progress, so the embedder's `munmap`
  after it cannot land inside one (`split-lock-unmap-race`).
- **Instruction fetch.** The core asks the library which ranges are executable
  (`QueryGuestExecutableRange`); with registered memory only pages with
  `FEXMAC_PROT_EXEC` are, so a jump to an unmapped or non-executable address stops with
  a page fault at that address (access `EXECUTE`) instead of the translator reading it.
  Without registered memory such a jump makes the translator read the address in host
  code: a host fault, passed on.
- **Host writes to guest memory.** A write by host code (the embedder, or a kernel call
  it makes for the guest, such as `read()` into a guest buffer) to a page that holds
  translated code faults (or the call fails with `EFAULT`). Bracket it with
  `fexmac_memory_write_begin` and `_end` (any host thread, the same range; they nest).
  **The contract:** `begin` covers memory the guest may write. If a 4K page of the range
  is not registered mapped and writable for the guest, it changes nothing and returns
  `FEXMAC_ERROR_INVALID_ARGUMENT` (`fexmac_memory_write_begin_ex` also gives the first
  such address), and a kernel-emulating embedder fails the guest's call with `EFAULT`, as
  Linux would. Otherwise `begin` gives the host pages of the range write access and keeps
  them from being write-protected again until `end`; `end` discards whatever was
  translated from them meanwhile and makes other threads look their code up again. For
  the embedder's own writes to memory the guest may not write (loading an image into
  read-only or executable pages), `fexmac_memory_write_begin_ex(..., FEXMAC_WRITE_FORCE,
  ...)` makes the host pages writable whatever their guest protection until the matching
  `end`, which puts the guest's protection back (the guest can write them meanwhile, so
  no guest code should run that could). All return at once when no memory is registered.
  Test: `memory-write-begin-contract` (a `read()` into a read-only page fails with
  `EFAULT`; with the force flag `fread()` works; afterwards the page is read-only again).
  The instruction-test runner uses it around the one system call it answers with a write
  (`time`), returning `EFAULT` when it is refused.

## Guest memory faults

A memory access fault in translated code (item 6) ends the run with
`FEXMAC_STOP_GUEST_FAULT`:

| Field | Value |
|---|---|
| `instruction_address`, state `rip` | the faulting guest instruction |
| `trap_number` | 14 (#PF) |
| `fault_address` | the address the host reported (the first byte of the access, or of its inaccessible part) |
| `access` | `FEXMAC_ACCESS_READ` or `_WRITE` (an atomic read-modify-write counts as a write: the instruction is decoded, because the syndrome's WnR bit reports the read half of an atomic on a PROT_NONE page); `_EXECUTE` for a fetch |
| `error_code` | x86 bits: `FEXMAC_PF_PRESENT` (1) if the page is mapped with some access, `_WRITE` (2), `_USER` (4, always), `_INSTRUCTION` (16) for a fetch |
| `signal`, `si_code` | what a Linux kernel would deliver: SIGSEGV with `SEGV_MAPERR` for an unmapped page, `SEGV_ACCERR` for a page whose protection forbids the access (a PROT_NONE page has no PRESENT bit, as Linux reports it); SIGBUS with `BUS_ADRERR` if the registered protection allows the access but the host refused it (a file mapping past its end, an embedder's own `mprotect`). Host signal numbers (SIGBUS is 10 on macOS, 7 on Linux) |

Without registered memory the library goes by the host fault: macOS raises SIGSEGV for an
unmapped address (reported as `SEGV_MAPERR`, not present) and SIGBUS for a protected page
(`SEGV_ACCERR`, present for a permission fault, not present for PROT_NONE).

### Guest memory faults: what is exact

Checked by `Scripts/macos_fault_diff.py` (the package D review's differential harness,
kept as a permanent test; tool `fexmac_fault_diff`): 368 programs, each also run with
three flag preludes inserted right before the marked instruction (CF PF AF SF OF set; ZF
PF set; all clear), so 1,472 in all, each with one marked
instruction that faults on a test page, in all four multiblock/TSO combinations, each
translated with `SMCCHECKS` full and none, comparing the complete guest state (all GPRs,
rip, rflags, MXCSR, x87 control/status/tags and registers, MMX, XMM, YMM upper halves,
FS/GS bases, a hash of the data pages) at the fault against a reference run (the marked
instruction replaced by `hlt`; for `rep` forms, the same program with rcx set to the
elements x86 completes before the fault), the result after fixing the page and resuming
against a clean run, the result of the embedder's guest-signal round trip (the state
read at the stop written back unchanged with `fexmac_thread_set_state`, then the page
fixed and the program resumed) against a clean run, and, for writes, the result through the write-protection path
(`mtrack`) against `SMCCHECKS` full. The data area is mapped at a fixed address so that
stored addresses compare alike. Every difference fails the test unless
`Source/Embedding/tests/fault_diff_known.txt` lists it; a listed difference that
disappears fails it too. The programs cover every ALU, shift, rotate and bit-test form on
memory at every size with CF live afterwards (`rmw-*`), `xadd`, `cmpxchg`, `shld`/`shrd`,
`pop [m]`, `leave`, `enter`, `push`/`call` across a page, 16- and 32-byte stores and
`fxsave` across a page, x87 store-and-pop, and 121 `rep movs`/`rep stos` cases (every
element size, both directions, faults at offsets 1 to 65 into a 32-byte chunk, overlapping
copies ahead and behind by 1, 3, 8 and 31 bytes, a faulting source).

**Exact (the state before the instruction, and running again gives x86's result):**

- Loads, plain stores, register `push`/`pop`, `call`/`ret`, vector and x87 accesses,
  `lock` operations. On macOS the core flushes its whole register cache (DF, MMX, the AVX
  upper halves, cached context values) and puts the flags in their canonical form before
  every instruction with a memory operand, and writes a virtual x87 stack back to the
  frame first (`Core.cpp`, `__APPLE__`). Before this, a fault after `std` reported DF
  clear, after `movq mm0, rax` mm0 was 0, after `vpcmpeqb ymm0` the upper half was stale,
  and after `fld1; fldpi` the x87 stack was empty.
- **`pop [m]` and `leave`** changed rsp before their faulting access. Pure reorder
  (`OpcodeDispatcher.cpp`, `__APPLE__`): load, store, then move rsp; `leave` loads from
  rbp before setting rsp and rbp. `pop` to an address that uses rsp (which x86 computes
  from the incremented rsp) keeps the core's form and saves rsp in the undo record below.
  The reorder is also faster: `push; pop [m]` loops as before, a `leave` loop 25% faster
  than `macos/v1` (no read-modify-write handle).
- **Read-modify-write instructions on memory** change the flags before their store in
  the core's handlers. Two designs, chosen per handler:
  - *Store first, flags after* (`OpcodeDispatcher.cpp`, `__APPLE__`): `add`, `or`, `and`,
    `sub`, `xor`, `inc`, `dec`, `neg`, `adc`, `sbb` (the result with the incoming carry
    by a non-flag-setting add), and `shl`/`shr` by an immediate or 1 on memory compute
    the result, store it, then compute the flags from the operands still in registers
    (the logical and shift forms recompute the result for the flags, since the register
    allocator would otherwise compute the stored value straight into the PF register).
  - *Already in order:* `lock` forms, `xadd` and `cmpxchg` on memory (always atomic) and
    `bts`/`btr`/`btc` on memory change state only after their single faulting access
    (checked, also misaligned through the split-lock emulation path).
  - *Undo record* for the rest: rotates, `sar`, shifts by `cl` (a zero count keeps every
    flag), `shld`/`shrd`. Before such an instruction (chosen in `Core.cpp` by opcode name,
    a memory destination and, for shifts, the opcode byte) the translated code stores
    NZCV, the raw PF and AF (not for rotates, which keep them: saving them makes them
    live, which was most of the cost), and for an rsp-based `pop` rsp, into
    `CPUState::UndoNZCV/UndoPF/UndoAF/UndoRSP`, sets `CPUState::UndoMask`, and clears it
    after the instruction. It is saved after the per-instruction self-modifying-code
    check, which can leave the block for retranslation (a record left set there would
    be applied to the next faulting instruction). The fault path (`Faults.cpp`,
    SpillGuestState) uses the saved values when the mask is set. The mask lives in the
    thread frame rather than in the RIP-entry metadata, so the fault path needs no
    lookup.
  - Cost against `macos/v1`: loops of `add/sub/inc/or`, `adc/sbb/add`, `lock add; lock
    xadd` and `shl/ror/bts` on memory run at +0 to 4%; a loop of only record-carrying
    forms (`rol [m],cl; rcl [m],1; sar [m],3`) +15%, of rotates only +13%. With a record on every
    read-modify-write (the first version of this rework) such loops took +65 to 130%.
  - **Flags at the fault are the flags before the instruction**, also the ones it
    overwrites and ones the rest of the block overwrites unread: before every instruction
    that can fault the translator emits `KeepFlagsLive`, an IR op with no code that the
    flag passes treat as reading every flag, so the incoming flags are materialised (host
    NZCV, PF/AF registers) where the fault can happen; every such instruction changes the
    flags only after its last faulting access or saves them in the undo record. Measured
    free (the loops in `docs/macOS-port-status.md`). For the store-first forms the
    operands the flag calculation takes as they are go through a copy made after the
    store: otherwise the register allocator put the loaded value straight into the AF
    (or PF) register before the store (AF was lost at faults with AF set). Implicit stack
    accesses (`call`, `ret`, `pushf`, `popf`, `iret`) count as instructions that can fault
    too (CF was reported inverted at a fault in `call` after `add r, 0`). x87
    store-and-pop pops after the store (checked; no record needed). Before this, `stc; sbb qword [m], 1` on 0x100 resumed to 0xff (x86 0xfe).
- **`rep movs` / `rep stos`** (JIT `MemCpy`/`MemSet`/`MemCpySrcBase`,
  `EmitRepStringInPlace` in `MemoryOps.cpp`): the loop uses the guest's own rcx/rsi/rdi
  static registers as its induction variables (with fs:/gs: on the source, the segment
  base is added to every source access and rsi keeps counting x86's way). One element is
  loaded and stored before the registers move. 32-byte chunks (`ldp`/`stp` of two q
  registers) run in batches during which neither the source nor the destination chunk
  crosses a 4K page, so a chunk faults whole or not at all, where x86 stops. When the
  destination is 1 to 32 bytes ahead of the source in the direction of the copy (the copy
  reads what it wrote), elements go one at a time in x86's order, except a forward byte
  copy with rdi = rsi + 1, which is a fill with the first byte; a source ahead of the
  destination is never read after being written, so whole chunks (loads before stores)
  are used however close. Plain and TSO (`ldapr`/`stlr`, per element) forms; both
  directions. The interrupt page is written after every batch and every 64 elements, so
  a stop request lands inside a long `rep` with exact registers (`stop-rep-movs`). A
  fault reports the first inaccessible byte of the element at rdi (store) or rsi plus
  base (load), not the host's chunk address (the harness checks it on every rep case).
  `rep lods`/`scas`/`cmps` were already exact.
- **Multi-part stores:** a 32-byte store is emitted as two 16-byte stores, the upper half
  first, so a store running into an inaccessible page above writes nothing; `push`,
  `call`, `enter`, 16-byte stores and `cmpxchg16b` across a page write nothing before the
  fault (checked).

**Not exact, at the fault only (listed in `fault_diff_known.txt`; running again gives
x86's result in every case):**

- A 32-byte store whose *lower* page is the inaccessible one has already written its
  upper 16 bytes.
- `fxsave` across a page may be partly written (x86 may leave it partly written too).

The embedder may then fix the cause (map or unprotect the page) and run again, which runs
the same instruction (`fault-mid-block`, `fault-store-loop`, the `guest-*` subtests), or
set a new state, for example a guest signal frame it built, and run (`fault-fetch`).
Faults that are not the guest's are still passed on (`unclaimed-fault-dies`,
`callback-callret-guard-passed-on`, `guest-fault-hosted` for a host fault).

## Stopping a thread from another thread

`fexmac_thread_request_stop(thread)` may be called from any host thread and from a
signal handler: it sets the thread's request flag and makes its interrupt fault page
read-only (an atomic store and an `mprotect`; no lock, no allocation). On macOS the core
always emits a store to that page at every block entry and every loop back-edge
(`NeedsPendingInterruptFaultCheck`, set in `Core.cpp`; a macOS-only addition also puts
the check on conditional back-edges, `DEF_OP(CondJump)`, where upstream had only
unconditional ones, so a loop closed by `jnz` inside one translation unit could not be
stopped). The store faults; the handler (item 3) writes the guest state back, at that
point an instruction boundary (rip the block's or loop target's first instruction), and
leaves the dispatcher; `fexmac_thread_run` returns `FEXMAC_STOP_REQUESTED`.

- **Where else the page is written.** The dispatcher writes it after its calls into the
  core and the core's C++ code at the end of every signal-deferring section (a lock
  guard). There the handler steps over the store (nothing reads the page) and leaves the
  page read-only, so the next block entry stops the thread; a system call callback that
  calls `fexmac_invalidate` passes through this.
- **Bounds.** A thread in translated code reaches a check within one block (at most
  `MAXINST` instructions without a loop) or one loop iteration. Linked blocks enter their
  target through its entry, so a chain of linked blocks is checked at every link. A
  `rep lods`/`scas`/`cmps` is checked on every iteration and stops part-way, with rcx,
  rsi and rdi showing its progress, as x86 interrupts one (`stop-rep`: a 256MB
  `repe cmpsb` stopped about 20 times). A `rep movs` or `rep stos` is one host loop and
  is not interrupted: the wait is up to its whole length. A system call callback in
  progress finishes first. Measured on the M4 with eight busy host threads on the ten
  cores (`stop-latency`, 2,000 stops per case: a block linked to itself, a loop closed
  by `jmp`, by `jnz`, multiblock on and off): median 4 to 6 us; 99th percentile 30 to
  130 us on an otherwise quiet machine and 0.4 to 1.2 ms with other builds running (load
  average 12 to 17); maximum 1 to 10 ms (the scheduler).
- **Defined behaviour in every state.** Each request is answered by exactly one
  `STOP_REQUESTED`; requests before it are merged. A thread not running answers it at
  the start of its next run, before any guest instruction (`stop-not-running`). A run
  that ends otherwise first (a system call stop, a fault) leaves it pending for the next
  run (`stop-during-callback`). A request that arrives while a stop is being delivered
  may be answered by that stop or by the next run. Races with entering and leaving runs
  (`stop-many-threads`: 20,000 runs of a thread that stops at a system call every
  iteration, against a thread requesting in a loop), with callbacks and with
  invalidation are covered; destroying the thread during the call is not allowed (the
  function cannot check the handle without a lock).
- **With the host owning the signals**, these faults arrive as SIGBUS on the stopped
  thread and the host passes them to `fexmac_handle_fault` like any other; the host must
  not block SIGBUS/SIGSEGV on guest threads.

## Self-modifying code

**Defaults (no `SMCCHECKS` option):** the core's default, `mtrack`.

- **No guest memory registered:** nothing is detected, exactly as on `macos/v1` before
  this package: `mtrack` without memory tracking has nothing to protect, and the embedder
  calls `fexmac_invalidate` after changing code.
- **Memory registered (`fexmac_memory_map`):** write protection of translated pages
  (below). A code write is re-run through the guest-fault path, which is exact for every
  form the differential harness covers on the write-protection path
  (`fault_diff_known.txt` has no write-protection entries left); before items 2 and 3 of
  the package D review were fixed the default for registered memory was `full`, as an
  interim. Host writes to such pages must be bracketed with
  `fexmac_memory_write_begin`/`_end`. Not seen: writes through another mapping of the
  memory (aliases, below) and writes to host pages registered with
  `FEXMAC_PROT_ALLOW_UNENFORCED` mixed protections ("Granularity").
- `SMCCHECKS full` (an option): the translator checks the guest bytes of each block
  before every instruction, everywhere, aliases included; on loops of 3 to 8 instructions
  with memory accesses it costs 2 to 6 times the time of `none` (the review's
  microbenchmarks, M4). `none` detects nothing.

With registered memory under `mtrack`:

1. When the core translates code from a 4K guest page (`MarkGuestExecutableRange`, called
   with the core's invalidation lock held shared), the library removes write access from
   its 16K host page if the guest page is writable.
2. A guest store to that page faults (item 5). The handler writes the guest state back
   (rip at the store) and returns from the signal into a trampoline.
3. The trampoline runs in ordinary context. It takes the context's thread lock, the
   core's invalidation lock exclusively and the memory lock, gives the host page its
   write access back, marks it as being written (so no other thread write-protects it
   again until the store is done) and discards the translations of the whole host page
   for every thread. No lock is ever taken in the signal handler: the interrupted code was
   translated guest code, which holds none, and the trampoline is ordinary code on that
   thread.
4. It re-enters the dispatcher with the single-instruction flag, so the store is
   translated and run on its own. A block that writes into its own later instructions
   therefore never runs their stale translation, and since single-instruction
   translations do not report code pages, the page is not protected again before the
   store has run (otherwise it would fault for ever). This is how FEX's Linux layer
   handles the same case (`SyscallsSMCTracking.cpp`).
5. At the next block entry after the store, the thread finishes the write: it discards
   what other threads translated from the page meanwhile (they may have read the old
   bytes) and asks every other thread to look its current code up again at its next
   block entry or back-edge (the interrupt fault page with a "resync" flag), so a thread
   spinning in a loop of the old code leaves it. The next translation from the page
   protects it again.

**Aliases.** Write protection covers the host page the code was translated from. A guest
that runs code through one mapping and writes it through another of the same memory (a
JIT's dual mapping, shared memory) is not detected: the embedder, which created the alias,
must call `fexmac_invalidate` for the executable alias's range after such writes (or keep
`SMCCHECKS full`). `smc-alias-needs-invalidate` shows the stale result without the call
and the new code with it.

Host pages are 16K and guest pages 4K, so data that shares a host page with code costs a
fault per write (counted by `fexmac_memory_code_write_count`). Measured on the M4
(`smc-data-on-code-page`, 20,000 `add qword [rdi], 1`, with other builds running on the
machine): data on another host page, no faults, 0.1 ms in all; on another 4K page of the
code's host page, or on the same 4K page, 20,000 faults, about 33 us per write (two
faults, two trampolines, two invalidations of the page and a retranslation each). A patch
of a function between calls (`smc-patch-loop`, 1,000 patch-and-call rounds) costs about
28 to 30 us. Before step 5 existed a write cost about 11 us, but threads could keep
running code translated from the old bytes (`smc-cross-thread` hung in most runs).
FEX's Linux layer does the same work per write on a 4K page without step 5; macOS
multiplies how much data lands on code pages by four. An adaptive fallback (a page that faults often stops being
write-protected and its blocks get the core's per-instruction check instead,
`Block.ForceFullSMCDetection`) would bound it; it needs a small core hook (the frontend
asking per range) and is not done. `SMCCHECKS full` remains available.

Tests: `smc-patch-loop` (execute, patch, execute, 1,000 times, multiblock on and off),
`smc-self-patch` (a block patching its own later instruction, `SameBlock.asm`'s pattern),
`smc-data-on-code-page`, `smc-cross-thread` (40 rounds of four spinning threads and a
fifth guest thread patching the immediate they test, with four busy host threads; every
spinner sees the patch, within a millisecond), `smc-misaligned-atomic`,
`split-lock-on-code-page`, `smc-embedder-write` (the callback writes code between
`write_begin` and `_end`; the guest runs the new code) and
`smc-embedder-write-unbracketed-dies` (without them: the host write is not the guest's,
the library says what to call, the process dies by SIGBUS). The instruction tests'
self-modifying-code tests run in this mode with
`Scripts/macos_asm_tests.py --smc mtrack`, which makes the runner register its memory
(`FEXMAC_RUNNER_REGISTER_MEMORY=1`); without registration all three fail, with it all
pass in all four modes.

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
- **Which memory is code** comes from the guest memory map (`fexmac_memory_map` with
  `FEXMAC_PROT_EXEC`; see "Guest memory"): the code cache hashes and patches guest bytes
  of executable registered pages only, and executing anything else stops with a fetch
  fault. Register the guest's memory when the cache is on. (`fexmac_add_code_range` /
  `fexmac_remove_code_range`, which package F added as a second registry, are replaced by
  it; there is one registry and "nothing registered" has one meaning: all memory is
  executable code.)
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
| `guest-load-prot-none`, `guest-lock-xadd-prot-none`, `guest-lock-cmpxchg-prot-none`, `guest-store-read-only` | a guest access to a protected page (nothing registered) stops the run as a page fault with the right rip, address, access, signal, si_code and error code; after `mprotect` the same instruction runs to the `hlt` |
| `guest-misaligned-atomic-prot-none` | a misaligned atomic on a PROT_NONE page: the same, reported before the core emulates anything |
| `guest-fault-hosted` | with the host owning the signals, the guest's fault is claimed (the run stops, the host handler never sees it) and a host fault still reaches the host handler |
| `fault-mid-block`, `fault-flags` | a load fault after several instructions of the block: rip, registers written before and not after, a vector register, flags read later; multiblock, TSO and registered memory in all eight combinations; fixed and run again |
| `fault-store-loop` | a store loop running into a read-only page stops with rcx = 2048 stores done; completes after the fix |
| `fault-kinds` | registered unmapped (`SEGV_MAPERR`), registered PROT_NONE (`SEGV_ACCERR`, no PRESENT), registered readable but host-inaccessible (`SIGBUS`, `BUS_ADRERR`), unregistered unmapped |
| `fault-rep` | `rep lodsb` into a PROT_NONE page shows its progress; `rep stosb` reports the state before it; both complete when run again; TSO on and off |
| `fault-vector`, `fault-stack`, `fault-fetch` | vector load fault; `push` to an inaccessible stack (rsp unchanged); a fault two calls deep, then both returns land right, three times (call/return stack consistent); fetch from unmapped and non-executable pages, delivered by setting a handler as the new state, and run again after the page is made executable |
| `memory-arguments` | argument checks of the memory functions |
| `stop-latency`, `stop-rep`, `stop-not-running`, `stop-during-callback`, `stop-many-threads` | see "Stopping a thread from another thread"; `stop-not-running` also requests from a signal handler |
| `smc-*`, `split-lock-faults`, `split-lock-on-code-page` | see "Self-modifying code" and "Fault handler locks" |
| `stop-resync-storm` | 3,000 stop requests (two at a time) to a spinning thread while four guest threads write code (each write resyncs it): none lost |
| `split-lock-unmap-race` | four threads run misaligned `lock xadd` on a page the main thread unmaps (`fexmac_memory_unmap`, `munmap`) and maps again for 3 seconds: page faults, no death (died within seconds before) |
| `stop-after-destroy` | a stop request for a destroyed thread returns `INVALID_HANDLE`; 2,000 destroys racing with requests from another thread |
| `smc-alias-needs-invalidate` | see "Self-modifying code", aliases |
| `code-ranges` | the memory map as the code registry: unregistered memory is a fetch fault with `SEGV_MAPERR`, a registered non-executable page `SEGV_ACCERR` with the present bit |
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
| `invalidate-concurrent` | 8 guest threads invalidating in every one of 20,000 system calls each while the main thread invalidates in a loop, at least 12 runs; a progress watchdog fails it within 5 seconds of a hang. It hung in each of 3 runs before package E's reader-writer lock fix and passed 6 of 6 after it |
| `split-locks-concurrent` | 8 threads at once run 30,000 `lock xadd` each on one qword crossing 16 bytes, a 128-byte line and the 16K page, a `lock cmpxchg` increment loop across 16 bytes (each adding 0x100000001, which changes both halves), and a spin lock taken with `xchg` across 16 bytes and released by a plain store; every total must be exact. With the split-lock lock switched off in a scratch build, the three `lock xadd` totals came out short in 3 of 3 runs |
| `split-locks-every-form` | every form the core emulates across 16 bytes, from 8 threads at once, 10,000 operations each: `lock xadd` of 16, 32 and 64 bits, `lock add`, `sub`, `xor`, `inc`, `dec`, `not`, `neg`, `or` then `and`, a `cmpxchg8b` increment loop, a `cmpxchg8b` spin lock with a plain release and an `xchg` spin lock whose value spans both halves; operands change both halves where the operation allows it; a host thread meanwhile writes the neighbouring bytes that share the operand's compare-and-swap units with plain stores. Every result exact. Before the split-lock fixes of package E's third round, 7 of the 13 forms came out wrong in each of 2 runs |
| `split-locks-signal-storm` | the host owns the fault signals with an empty mask; 8 threads run split `lock xadd` until 3,000 signals sent to one of them have landed inside its fault handling, while a host thread writes the neighbouring bytes; at each landing the signal handler waits for another thread's progress, which cannot come if the interrupted thread holds the split-lock lock. No stall, exact total. A stress test: it rarely catches missing signal blocking (see `misaligned-blocks-async-signals`) |
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
and limit, overrun a code buffer on purpose, read and set how much of the current code
buffer is used, and make the next code buffer allocations fail):

| Subtest | Checks |
|---|---|
| `guest-load-callret-low-guard`, `guest-store-callret-high-guard` | a guest load or store on a call/return guard page is the guest's page fault at that address; no reset of `x25`, no loop (`guard2.c`) |
| `scratch-guard-disarmed-after-translation` | the scratch-buffer guard is disarmed once a block is emitted, so no later access to that page can be taken for the translator's overflow |
| `scratch-overflow-many` | with the default first size, 96 `rep movsq` fit and 416 overflow (the premise of `scratch-overflow-sizes`); starting at 1, 6 and 12 bytes per IR node, translations overflow 0, 1, 2 and up to 5 times and every block runs correctly with the guard disarmed afterwards |
| `scratch-limit-stops` | with a 64K limit a block that does not fit stops with `UNSUPPORTED_INSTRUCTION` at its start after several overflows, with the core's message; the guard is disarmed; with the limit lifted the same thread translates and runs it |
| `code-buffer-guard-pages` | three code buffers in turn (filled by one large block translated again and again) each end in an inaccessible page with executable code before it |
| `code-buffer-overrun-dies`, `code-buffer-overrun-chained` | a write running past the end of the code buffer with the thread switched to writable (from the system call callback) faults on the guard page; the process dies with the library's message, or, with a host handler installed before the library, the message is written and the host receives the fault at the guard address |
| `guest-store-code-buffer-guard` | a guest store from translated code into a code buffer's guard page is the guest's page fault stop at that address, not reported as an overrun of the code buffer |
| `code-buffer-switch`, `code-buffer-switch-cached` | one large block translated again and again (invalidated in between) until the thread has written to three code buffers (checked with the buffer-guard hook), every copy correct; the second with the code cache on and the same block options, every copy after the first loaded from the cache by copy |
| `code-buffer-fill-exact` | with the buffer's used size set by a hook, a block that fits exactly in what is left ends on the buffer's last usable byte, in the same buffer, and runs; with 16 bytes fewer left it moves to a new buffer and runs |
| `code-buffer-fill-random` | the reviewer's fill test: blocks of random sizes (5,014 of them) until six code buffers have been used, every result exact; at most 20,000 blocks |
| `code-buffer-first-fails` | the first code buffer forced to fail as if `mmap` had (ENOMEM): `fexmac_thread_create` returns `OUT_OF_MEMORY` and logs `couldn't allocate the executable (MAP_JIT) memory (errno 12)`, not a guard-page failure; the next create works and the thread runs |
| `code-buffer-later-fails-dies` | a later code buffer forced to fail while translating: the process ends (SIGILL) with the same accurate message for the 32 MiB buffer |
| `callback-callret-guard-passed-on` | the system call callback reading a call/return guard page: the host's handler receives the fault at that address |
| `log-callback-reentry` | fexmac calls from inside the log callback return `BUSY`; afterwards they work |
| `fault-inside-handler-dies` | the hook fired exactly once, the process died with SIGBUS, the message was written |
| `no-log-callback-in-handler`, `log-restored-after-failed-create`, `invalidation-mask` | as named |
| `split-emulation-fault-dies`, `split-emulation-fault-hosted-dies` | a page made inaccessible between the library's check and the core's emulation of a misaligned atomic: the nested fault ends the process with the library's message, in both arrangements, and never reaches the host's handler |
| `misaligned-blocks-async-signals` | a hook sends the thread a signal where the core is about to handle a misaligned access (and may take its locks), in the host-owns-signals arrangement with an empty mask, 1000 times: the signal must be handled only after the handling (0 of 1000 inside; without `fexmac_handle_fault`'s blocking, 1000 of 1000, in 10 of 10 runs) |

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

- Exact memory at the fault (running again is right) for the two entries left in
  `fault_diff_known.txt` (see "Guest memory faults: what is exact"): a 32-byte store whose
  lower page faults, `fxsave` across a page.
- A cheaper stop check (future option, not done): today every block entry, back-edge and
  `rep` loop writes the interrupt fault page; on a loop of two linked blocks without
  multiblock that costs +50% (+18% with multiblock, where the loop's own back-edge check
  is what makes it stoppable). Checking only on direct exits to a guest address at or
  below the block start (every cycle of direct jumps has one) and on indirect exits,
  with the target's rip recovered at the stop, measured at the base time for that loop
  in a prototype. It would be an embedder setting, default the current check.
- An adaptive fallback for host pages where data and code share a page and every write
  faults (see "Self-modifying code").
- Enforcing 4K guest protections whose host needs differ within a 16K host page (refused
  unless `FEXMAC_PROT_ALLOW_UNENFORCED`; a partial unmap returns `FEXMAC_DONE_UNENFORCED`).
- A misaligned atomic whose page the embedder changes with a raw `mprotect` during the
  emulation ends the process instead of being a guest fault (see "Fault handler locks").
- The fault-handler contract does not say what happens when the library chains to a
  previously installed handler and that handler returns without resolving the fault: the
  library's handler returns too and the instruction faults again, forever. It should end
  in death by the original signal. The test runner avoids it by owning the signals (way 2).
- Several contexts in one process (the core's configuration is process-wide).
