# macOS port status

This file tracks what the macOS (Apple silicon) port does and does not do yet. Update it
in the same change that closes or opens a gap.

Related documents:

- `docs/macOS-v1.0-plan.md`: what "1.0" means for this port and the work left to get there.
- `docs/macOS-embedding-interface.md`: what a program hosting the core must provide.
- `docs/macOS-code-cache-design.md`: design for loading cached translations on macOS.
- `Scripts/macos_sync_upstream.sh` and `docs/macOS-upstream-sync.md`: bring new upstream FEX
  commits into the port. Upstream merged so far: FEX-Emu/FEX `main` at `a92baf073`
  (27 November 2026).
- `Scripts/macos_asm_tests.py`: runs the instruction tests.
- `docs/macOS-benchmarks.md` and `Scripts/macos_bench.py`: the guest benchmark, its method,
  results and analysis.

## Where it stands

- **Builds and links on Apple silicon.** The core libraries and the test tools build with
  no errors with the macOS 27 SDK (command line tools, Apple clang 2100.4.1.1) and with
  Xcode 26.5 (macOS 26.5 SDK). The dylib and the test tools depend only on `libSystem`
  and `libc++`; the bundled `fmt` and `range-v3` are always used on macOS, even when
  Homebrew has its own.
- **Passes on a physical Mac.** On a Mac mini (Apple M4, macOS 27.0.1, 16K pages), as
  built and signed for the hardened runtime with only the JIT entitlement: the smoke and
  multi-thread tests pass, and 2,265 of 2,296 instruction tests (upstream's 2,292 and four of the port's) pass in each
  of upstream's three translator modes and in a fourth with x86 memory-ordering emulation
  switched on, with no unexpected failures. The rest are listed under "Tests".
- **Write protection is enforced on real hardware and respected.** `JITWriteProbe` shows
  it; the branch without the runtime fixes dies at its first write to code memory.
  GitHub's hosted Mac is a virtual machine that does not enforce it.
- **Runs eight guest threads at once** with correct, separate results.
- **Keeps x86 memory ordering between threads** with the emulation on: multi-thread litmus
  tests see no forbidden outcome in 100 million tries each, and see them by the million
  with it off; vector and string-instruction ordering are each shown to depend on their
  own setting (physical Mac).
- **Runs a real guest program to the right answer** (criterion 7): a freestanding x86-64
  benchmark run through `fexmac` by `FEXCoreBench`. Its literal mode (16 kernels, no
  system call but exit) reports every checksum right; its timed mode (18 kernels, with a
  private write/clock/thread ABI) gives bit-identical checksums to the same C source
  under Rosetta 2 and natively on arm64. Speed is measured (criterion 8): the generated
  summary in "Guest program and benchmark" below gives the figures and says whether they
  are provisional or final.
- **Fault handling for an embedder** (criterion 4, met apart from at-fault-only limits: see
  "Fault handling for an embedder" below): misaligned atomics,
  scratch-buffer restart, self-modifying code detected by write-protecting translated
  pages, stopping a thread from another thread at a safe point, and guest page faults
  delivered as stops with the guest state reconstructed, each tested through the C
  interface as built and hardened. See "Fault handling for an embedder" below.
- **Linux is unaffected, and this is now checked on every push.** A hosted arm64 Ubuntu
  job builds the core-only configuration and the whole ordinary Linux build, and runs
  upstream's 64-bit instruction tests and unit tests there, on the port and on the
  upstream commit it is based on: identical results (see "Test jobs" below). It found
  one Linux regression, now fixed (the scratch-buffer guard page).

Only the translator core is built on macOS. FEX's Linux process loader, syscall emulation,
FEXServer and thunks are not part of this port.

## Done, using the method macOS provides

| Area | macOS method |
| --- | --- |
| CPU feature detection | `sysctlbyname("hw.optional.arm.FEAT_*")` |
| Thread waiting | `os_sync_wait_on_address` |
| Thread ids and signal masks | `pthread_threadid_np`, `pthread_sigmask` |
| Random numbers | `arc4random_buf` |
| Directory listing | `fdopendir`, `readdir` |
| Executable mappings | `MAP_JIT` flag on allocation |
| Linker options | `-dead_strip`, `-x`, `-dead_strip_dylibs` |
| CPU tuning | `-mcpu=apple-m1` baseline |

## Runtime fixes and what is still unproven

Fixed in code on `macos/runtime-fixes`:

1. **Writing translated code.** `FEXCore::Allocator::ScopedJITWrite` wraps
   `pthread_jit_write_protect_np`. It surrounds the dispatcher emit, the copy of compiled
   blocks into the code buffer, block linking and unlinking, cached-code loading and the
   fault-time patching of memory accesses. For the patching it covers the whole locked
   region, because the lock word itself sits in the block's tail inside the code buffer:
   with only the patch covered, the lock's first write faulted inside the fault handler
   and the thread hung (24 instruction tests, memory-ordering mode only, physical Mac
   only).
2. **Reserved register.** `x18` is removed from the register allocation and from the
   dynamic save lists on macOS.
3. **Fault page.** The per-thread interrupt fault page and the thread state are aligned to
   the 16K host page.
4. **Code-buffer guard page.** Linux protects the last page of each code buffer with
   `mprotect`; macOS refuses that inside a `MAP_JIT` mapping (and `mach_vm_protect`
   too), and refuses `MAP_JIT` with `MAP_FIXED`, so the guard was first skipped and a
   write past a buffer's end landed in whatever was mapped next. It now replaces the
   buffer's last 16K host page with a `MAP_FIXED` mapping of ordinary `PROT_NONE`
   memory: the rest stays `MAP_JIT`, and a write past the end faults whatever the
   thread's write setting (`JITWriteProbe`'s "Guard:" cases, as built and hardened on
   the M4: the replacement is allowed, the front still runs code and is still
   write-protected, a store past it faults with DFSC 0x07 while the thread is
   writable, one `munmap` frees both). Every write into a code buffer (compiled blocks,
   blocks copied from the code cache, block tails) is bounds-checked before it is made,
   so in normal operation the guard never fires; it is a safety net. If the core's own
   code hits it, the library's handler (when installed; a host that owns the signals
   and calls `fexmac_handle_fault` gets no such line) writes `fexmac: SIGBUS: write
   past the end of a code buffer, at address ... (its guard page) from host pc ...`
   and passes the fault on, and the process dies (`fexmac_hook_test
   code-buffer-overrun-dies`, `-chained`). A guest access from translated code or the
   dispatcher that happens to hit the page is the guest's fault, a page-fault stop at that address
   (`guest-store-code-buffer-guard`). A buffer that cannot be made (its `MAP_JIT`
   `mmap` fails, now checked against `MAP_FAILED`, or the guard cannot be placed) is
   reported with what failed and its errno: for the context's first buffer, made with
   its first thread, `fexmac_thread_create` returns `FEXMAC_ERROR_OUT_OF_MEMORY`
   (`code-buffer-first-fails`); for a later one, made while translating, the process
   ends with that message (`code-buffer-later-fails-dies`). One block may have at most
   64MB of host code (half the largest, 128MB, buffer): the translator gives up beyond
   that, the code cache rejects an entry claiming more (before, an entry above
   128MB-16K made the loader replace code buffers for ever while holding the
   invalidation lock; `macos_code_cache_tests.py`'s 64MB+16 and 128MB crafted
   entries), and the core's buffer allocation refuses such a size instead of looping.
   The debug-only cache validation (`CodeCache::Validate`) skips a section larger than
   the largest buffer instead of looping. Tested filling exactly to a buffer's last
   byte and with 5,014 random-size blocks across six buffers (`code-buffer-fill-exact`,
   `code-buffer-fill-random`). Cost: one
   extra `mmap` per code buffer (0.4 us; buffers are made a handful of times per
   process), no extra address space, 12K less usable space per buffer than the 4K
   page Linux reserves.
5. **Host feature detection.** The Linux kernel version check is kept out of the Mac build.
6. **Clearing memory.** `MADV_DONTNEED` does not zero memory on macOS, but the block
   lookup cache is cleared by relying on that. `VirtualDontNeed` now maps fresh pages
   over the range on macOS.
7. **Reserving addresses.** The Linux-only "do not replace" mapping flag is replaced by a
   hint-and-check, and the address-space size comes from `sysctl`.

Added for embedders (the core expects these from whatever hosts it):

- **`fexmac`, a C embedding interface** (`Source/Embedding/`, header `fexmac.h`), built
  as a static library, as a standalone prelinked archive that needs only `libSystem` and
  `libc++` (`build/Lib/libfexmac_bundle.a`) and as a dylib. It owns context creation and
  option checking, per-thread setup (call and return stack, descriptor tables, allocator
  per-thread initialisation), guest state, running guest code, system calls, the host
  fault handlers and stale-translation invalidation. All three test tools use it; the
  smoke and thread tests use nothing else. `docs/macOS-embedding-interface.md` describes
  it, the signal-handler contract and what the instruction-test runner still takes from
  the core for harness work.
- The fault handlers tell faults apart by the data abort's fault status code
  (ESR_EL1.DFSC, which macOS passes in the signal context), because macOS reports a
  protection fault and an alignment fault alike as SIGBUS with si_code 1. They hand
  alignment faults in translated code to the core (53 instruction tests depend on it),
  restart translation when the translator's scratch buffer fills, and reset the
  call/return stack pointer when deep recursion or unmatched calls and returns run into
  its guard pages. A guest access to a protected page is passed on, never claimed (it
  used to be sent to the misaligned-access rewrite and loop). They can be installed by
  the library (chaining to handlers installed before, honouring their mask and flags) or
  called from the host's own handlers. They are installed with `SA_NODEFER`, so a fault
  inside the library's handler crashes the process instead of hanging it (a reproducer
  hangs without it on the M4); handlers the library chains to are not policed, and a
  fault inside one ends in a stack overflow rather than a hang. The core disarms the
  translator's scratch-buffer guard when a block is emitted (macOS only), so only a
  translation in progress can be restarted.
- **The translator's scratch buffer** (package J). Each block is emitted into a
  per-thread scratch buffer followed by one inaccessible 16K host page; a block whose
  code does not fit runs into it, the handler resumes the thread at the restart point
  set at the start of `Arm64JITCore::EmitJITCode` (per thread, valid exactly while the
  guard is armed), and the translation starts again with twice the bytes per IR node.
  On macOS: the guard address the core records is exactly the first byte of that page
  and the handler claims exactly that page (it used to be inferred from a 4K-based
  address that could sit up to 16K below the real page, and the emitter stopped 4K
  short of it); the guard is disarmed as soon as the emitter stops writing, before the
  copy into the code buffer, so a restart can never skip that copy's write-protect
  switch; a restarted attempt drops the abandoned attempt's guest-instruction records,
  which were written into the block's RIP table before; and growth is bounded: a
  translation that would need more than 64MB (half the largest code buffer, so
  whatever fits can be stored) gives up with `Block at ... still overflows its
  translation buffer` and the run stops as `FEXMAC_STOP_UNSUPPORTED_INSTRUCTION` at
  the block, instead of doubling until the size wraps. The emitter's writes are
  sequential; the largest step it takes without writing is the block tail (under 64
  bytes), so no emit can step over a 16K guard. The extra host page after the scratch
  buffer and the exact guard address are macOS-only (`PooledAllocatorVirtualWithGuard`
  in `ThreadPoolAllocator.h`, `EmitJITCode`): an earlier port commit had given the
  allocator its extra page on every system, so on Linux the guard the core recorded
  (4K short of the buffer's end, as the Linux and Windows fault handlers expect) was
  writable memory and an overflow would have faulted a page later, unclaimed; Linux and
  Windows are now exactly upstream's there. The scratch buffer is ordinary memory,
  written with the thread executable, so the restart leaves the thread in the state it
  had. Translation never nests on a thread (one restart point per thread is enough),
  and nothing else runs on the thread while the guard is armed: guest code and callbacks
  run only between translations (package D's stop requests and guest-fault delivery act
  on translated code and must keep out of the translator). A thread keeps its buffer and only
  claims a larger one when a translation needs it. Measured with `rep movsq` blocks:
  with the default first size, long runs overflow once; starting from 1 byte per IR
  node (a test hook) translations overflow up to 5 times, and every one runs correctly.
- Invalidation discards stale translations when guest code changes, holding the core's
  invalidation lock, for every thread of the context. The self-modifying-code tests
  looped forever until the runner did this. With many threads invalidating at once it
  hung until package E's reader-writer lock fix; `fexmac_c_test invalidate-concurrent`
  now passes.
- `Source/Tools/CommonTools/MacOSHostFaults.h` and `CoreThreadSetup.h`, the previous
  per-tool versions, are gone: `FEXCoreOrderingTest`, the last tool using them, now uses
  `fexmac.h` only.

Checked and found not to need a change:

- **Instruction cache flush.** Already uses `__builtin___clear_cache`, which calls
  `sys_icache_invalidate` on macOS. The dispatcher flushes a second time after switching
  back to executable; the probe shows that is not needed on an M4, but it is kept, as
  not proven unneeded elsewhere.
- **Fault context.** The core does not read `ucontext_t` itself; the embedder does.

Found out about macOS on Apple silicon:

- **The first 4GB cannot be used.** A 64-bit program that reserves less than 4GB at
  address zero is killed at launch (`lowmem-pagezero` in the Mac job exits with 137).
  Guest memory therefore cannot be placed below 4GB.
- **64GB to 448GB is reserved**, observed as one region on the hosted Mac and believed to
  be the GPU's.
- A further region of about 12GB appears at a different address near 32GB in each process.
  What it is has not been identified.

Found out on a physical Mac (Apple M4, macOS 27.0.1), identical as built and hardened,
with `JITWriteProbe`:

- `pthread_jit_write_protect_supported_np` is 1, and write protection is enforced. A
  thread must switch to writable before writing `MAP_JIT` memory (in its default state or
  switched to executable, the write faults with SIGBUS) and back before running it
  (running while writable faults). Atomic operations count as writes.
- **`mprotect` of a page inside a `MAP_JIT` mapping is refused**, and so is
  `mach_vm_protect`; `MAP_JIT` together with `MAP_FIXED` is refused (EINVAL), so a
  `MAP_JIT` mapping cannot be placed over a reserved range. A `MAP_FIXED` `PROT_NONE`
  mapping may replace the tail of the program's own `MAP_JIT` mapping, which is how the
  code buffer gets its guard page (runtime fix 4). A `PROT_NONE` mapping merely hinted
  at the end of a `MAP_JIT` mapping happened to land there in one run and not in
  another, so hints are not relied on. Page-protection self-modifying-code detection
  still cannot be built inside the code buffer.
- **A fault inside a SIGBUS handler hangs the thread** instead of killing the process,
  because the signal is blocked while its handler runs. A hang in a test can be a crash
  in a fault handler.
- **Returning from a signal handler does not restore the thread's write setting.** A
  handler that switches leaves the setting it last chose. The library's fault handler (and
  before it `MacOSHostFaults.h`) handles these kinds of fault. Misaligned accesses in translated code are patched with the setting
  switched, and translated code only runs executable, so the handler ends in the state
  it found. Scratch-buffer guard faults in the translator's own emitter code are
  resumed without touching the setting at all (the emitter writes ordinary memory with
  the thread executable, and the guard is disarmed before the translator switches to
  writable to copy the block into the code buffer). Neither path leaves a wrong setting.
- **Flushing the instruction cache while still writable is enough.** Rewriting the same
  code 50,000 times with a different result each time, flushing before switching back,
  always ran the new code; flushing after switching back did too; never flushing ran
  stale code, which shows the test can detect it. Single thread, one address.
- **Reading `ctr_el0`, `midr_el1` or `id_aa64isar0_el1` from user space faults**
  (SIGILL); the port uses `sysctl` for all of them. **`dc cvau` is allowed.**
- **A protection fault and an alignment fault both arrive as SIGBUS with si_code 1**
  (`BUS_ADRALN`); a sent SIGSEGV arrives with si_code 2 and a sent SIGBUS with 1. The
  syndrome in the signal context tells them apart: DFSC 0x21 is an alignment fault,
  0x04 to 0x0F a translation or permission fault, and a signal the thread sent itself
  carries the SVC exception class. `JITWriteProbe --expect-enforced` asserts the cases.
- Misaligned atomic loads fault only when they cross a 16-byte boundary: an 8-byte
  `ldar` at offset 4 of a 16-byte-aligned buffer runs, at offset 12 it raises SIGBUS.
- GitHub's hosted Mac reports `pthread_jit_write_protect_supported_np` as 0 and lets a
  thread write and run `MAP_JIT` memory in any state, so it cannot test any of this.

Still unproven or open:

1. **Speed.** Nothing has been measured.

(The interrupt fault page and self-modifying code by write protection, open here before,
are exercised now: see "Fault handling for an embedder".)

## Fault handling for an embedder (criterion 4)

`docs/macOS-embedding-interface.md` has the contracts; this is the state after the package
D review and its second rework, on the M4. Criterion 4 is met except for the at-fault
limits listed in the last row, where running again still gives x86's result.

| Fault path | Tests | State |
|---|---|---|
| Misaligned atomics | `faults-*`, `split-locks-concurrent`, `split-lock-faults`, `split-lock-unmap-race`, `guest-misaligned-atomic-prot-none`, `split-emulation-fault-*-dies` | pass; with the second half on an inaccessible page each thread stops with a page fault before anything is written, totals exact after the fix |
| Scratch-buffer restart | `scratch-guard-disarmed-after-translation`, `scratch-overflow-*`; `CodeBufferOverflow` | pass |
| Self-modifying code by page protection | `smc-*`, `split-lock-on-code-page`, `SelfModifyingCode/*` with `--smc mtrack`, the harness's write-protection path | pass; **the default once memory is registered** (no memory registered: nothing detected, as on `macos/v1`); writes through aliases and to non-writable guest pages sharing a host page with writable ones are not seen |
| Stopping a thread from another thread | `stop-*`, `stop-rep-movs` | pass; median 4 to 6 us request to return; also inside a long `rep movs`/`stos` (32 MB, thousands of stops, registers exact at each, final memory x86's) |
| Guest fault with correct guest state | `fault-*`, `guest-*`, `Scripts/macos_fault_diff.py` (368 programs, each also with three flag preludes, x 4 modes x 4 ways including the get/set-state round trip) | **exact, and running again gives x86's result, for loads, stores, push/pop/call/ret, `pop [m]`, `leave`, `enter`, every read-modify-write form on memory (also locked and misaligned), `xadd`/`cmpxchg`, vector and x87 accesses, 16- and 32-byte stores across a page, and every `rep` string form part-way (also with fs:/gs:), with x86's fault address**; RFLAGS at the fault is the pre-instruction value, every flag; 2 memory-at-the-fault differences remain in `fault_diff_known.txt`: a 32-byte store whose lower page faults, `fxsave` across a page |
| 4K protections within a 16K host page | `memory-arguments` | refused unless `FEXMAC_PROT_ALLOW_UNENFORCED`; accepted ones are counted and NOT enforced (guard pages, read-only next to writable, SMC protection of the non-writable page): the 1.0 limit, stated in `fexmac.h` and the embedding document with the sound scheme as future work |

How (all macOS-only, `__APPLE__`): a full register-cache flush and x87 stack sync before
every instruction with a memory operand; an undo record (flags) for read-modify-write
instructions on memory and for `pop` to an rsp-based address; `pop [m]` and `leave`
reordered; 32-byte stores upper half first; `rep movs`/`stos` loops that use the guest's
rcx/rsi/rdi as their induction variables, in page-bounded 32-byte chunk runs; interrupt
checks on block entries, back-edges, conditional back-edges and inside `rep` loops; no
code check for undecodable instructions. The code cache's port revision is 3.

Performance, the review's microbenchmarks (best of 5, `SMCCHECKS none`, M4, machine
shared with other builds, so differences under about 5% are noise). Base: `macos/v1`;
"before": this package before the second rework (no undo record, the core's rep loops);
after: the tip.

| Loop | Multiblock | Base (`macos/v1`) | Previous tip `995572564` | Tip |
|---|---|---|---|---|
| counted `jnz` | off / on | 0.153 / 0.150 s | +3% / -1% | +3% / -1% |
| memory mixed (ld/st/adc) | off / on | 0.099 / 0.099 s | +2% / +2% | +2% / +2% |
| memory with carry | off / on | 0.127 / 0.127 s | +0% / -0% | -0% / -0% |
| `memjmp` (two linked blocks) | off / on | 0.051 / 0.036 s | +50% / +21% | +50% / +20% |
| `add [m]; adc [m]; inc [m]` | off / on | 0.185 / 0.174 s | +118% / +123% | -5% / +0% |
| `add/sub/inc/or [m]` | off / on | 0.178 / 0.177 s | +131% / +132% | -0% / +1% |
| `lock add [m]; lock xadd [m]` | off / on | 0.182 / 0.183 s | +70% / +60% | +0% / -2% |
| `adc/sbb/add [m]` | off / on | 0.178 / 0.178 s | +122% / +132% | +2% / +1% |
| `shl [m],1; ror [m],3; bts [m],5` | off / on | 0.176 / 0.183 s | +130% / +130% | +4% / -0% |
| `rol [m],cl; rcl [m],1; sar [m],3` (undo record) | off / on | 0.205 / 0.204 s | +78% / +78% | +16% / +15% |
| `push; pop [m]` | off / on | 0.055 / 0.051 s | +19% / +11% | +16% / +11% |
| `rep movsb` / `movsq` 4K | off / on | 0.034 / 0.035 s | +12 to 16% | +12 to 19% |
| `rep stosq` 4K | off / on | 0.033 / 0.033 s | +23% / +18% | +25% / +32% |
| `rep movsb` 64 bytes | off / on | 0.025 / 0.025 s | +27% / +27% | +27% / +27% |
| `rep movsb` 4K, rdi = rsi + 1 | off / on | 0.21 / 0.24 s | +159% / +131% | -94% / -95% |
| `rep movsb` 4K, rdi = rsi + 8 | off / on | 0.21 / 0.22 s | +128% / +135% | +129% / +131% |
| `rep movsb` 4K, rsi = rdi + 8 | off / on | 0.21 / 0.21 s | +127% / +129% | -95% / -95% |

`Scripts/macos_bench.py` `rep_movs` kernel (median of 3 launches x 5 runs, ms): `tso_on`
331 before, 381 after (+15%); `tso_off` 17.8 before, 21.4 after (+20%); native 16.3.
`copy_loop` unchanged (655 / 651 `tso_on`). The rep cost is the exactness: registers move
after every 32-byte chunk and every element, the page-bounded run setup, and an element
loop that counts elements for the interrupt check. A forward byte copy overlapping by
one (`rdi == rsi + 1`, run-length fills in LZ decoders) is a fill with the first byte,
and an overlap with the source ahead in the direction of the copy uses whole chunks:
both about 16 times faster than `macos/v1`. A destination 2 to 32 bytes ahead of the
source still goes element by element, bound by store-to-load forwarding (about 4 cycles
a byte); chunks narrower than the distance would keep x86's result and progress but are
not done. A
cheaper setup for short counts (one page test for the whole copy instead of the
per-pointer page room) was measured and gave nothing, so it is not in.

Read-modify-writes: `add`/`or`/`and`/`sub`/`xor`/`inc`/`dec`/`neg`/`adc`/`sbb` and
`shl`/`shr` by a constant store first and compute the flags after; lock forms, `xadd`,
`cmpxchg` and bit tests need nothing; only rotates, `sar`, shifts by `cl` and
`shld`/`shrd` keep the undo record (NZCV only for rotates). The record's cost was mostly
in reading PF and AF, which makes them live. The `memjmp` cost is the interrupt check at each
block entry: the check is one store (`str xzr, [x28, #off]`), and a load or even a `nop`
in its place costs the same, so there is no cheaper form; it is the extra instruction in a
two-block loop. A prototype that checks only on exits to a lower or equal guest address
(every cycle of direct jumps has one) brought `memjmp` without multiblock to the base time
(0.049 s against 0.074); it needs the same check on indirect exits with the target's rip
recovered at the stop. That is a documented future option (an embedder setting, default
the current always-on check), not done. With multiblock the
loop's own back-edge check is the cost (+18 to 31%) and is what makes the loop stoppable.
`SMCCHECKS full` costs 2 to 6 times `none` on the same loops (as on `macos/v1`).

## x86 memory ordering between threads

`FEXCoreOrderingTest` runs litmus tests: two guest threads (hand-assembled x86-64) make a
few accesses to shared memory, many times over, meeting at a barrier in guest code before
each try and waiting a small random time after it. Four such pairs run at once on the
same translated code; each run copies the code to a fresh address so it is translated
again, and all eight threads wait at a host-side start barrier before they run. Outcomes
are counted. Pinning threads to cores is not possible on macOS; the threads ask for the
user-interactive quality of service and the tool relies on numbers. The tool uses only
`fexmac.h` (it owns SIGBUS and SIGSEGV and hands each fault to `fexmac_handle_fault`,
counting the alignment faults the library claims).

Every test must also show an outcome that is only possible when its two threads overlap
(for message passing: no flag yet but some data, or part of the flag with all of the
data; for SB: both loads seeing both stores), or it fails as NOT RACING in every mode, so
no test can pass because its threads stopped overlapping. A test that shows none gets up
to two more sets of runs first (reported; their outcomes count too), and with fewer than
four groups every run has proportionally more iterations, because on a machine with few
processors (the hosted virtual Mac) two threads sometimes share a core for a whole run. With
one group and emulation on (or scalar), SB's relaxed outcome is reported but not
required: two threads alone sometimes show none over 24 runs of 400,000 tries on a loaded
M4, since it needs a load-acquire to pass a store-release; they must still overlap.
Counting tests instead run one of the locked operations the core emulates from every
thread of every group on one misaligned operand and check the exact result; each thread
takes a native ticket before and after its operations, and a gap between the two is the
evidence that threads ran in between.

In message passing, x86 forbids seeing any part of the flag without all of the data: the
flag is stored after the data, and a store becomes visible only after every earlier one.
(The first version of the tool counted only a whole flag; a partly visible flag with
missing data was counted as overlap. With emulation off that hid most of MP-splitline's
and nearly all of MP-split16K's forbidden outcomes.)

Results on the Mac mini (M4), hardened runtime, `--soak`: 100 million tries per litmus
test, 2 million locked operations per counting test, in each of the three modes. "On" is
all three settings (`FEX_TSOENABLED`, `FEX_VECTORTSOENABLED`, `FEX_MEMCPYSETTSOENABLED`);
"scalar" is only the first, as upstream's defaults; "off" is none.

| Test | What it checks | Forbidden outcome, off | scalar | on |
|---|---|---|---|---|
| SB | store buffering; allowed by x86 (relaxed outcome counted) | 27.4M relaxed | 0.44M relaxed | 0.60M relaxed |
| SB+mfence, SB+lock | Dekker: `mfence` or a locked instruction between store and load | 0, 0 | 0, 0 | 0, 0 |
| MP, MP+spin | message passing, plain 64-bit moves | 19.2M, 0.64M | 0, 0 | 0, 0 |
| S | store then store against load then store | 182,194 | 0 | 0 |
| 2+2W | two threads each storing twice | 201,416 | 0 | 0 |
| LB | load buffering | 0 | 0 | 0 |
| MP-sse | `movdqu` data, scalar flag | 19.9M | 0 | 0 |
| MP-sse2 | `movdqu` data and flag, both sides | 21.3M | 3.52M | 0 |
| MP-sse-st | `movdqu` stores, scalar loads | 22.0M | 38,514 | 0 |
| MP-sse-ld | scalar stores, `movdqu` loads | 24.8M | 81,651 | 0 |
| MP-movsb, MP-stosq | data by `rep movsb` / `rep stosq`, scalar flag | 24.1M, 21.9M | 0, 0 | 0, 0 |
| MP-stos-stos, MP-movs-movs | data and flag both by `rep stosq` / `rep movsb` | 20.7M, 13.4M | 2,204, 10,129 | 0, 0 |
| MP-movsb-read | scalar flag load, data copied by `rep movsb` | 7.1M | 0 | 0 |
| MP-movsb-read2 | flag and data both copied by `rep movsb` | 7.8M | 5,322 | 0 |
| MP-flag-stosq | scalar data, flag by `rep stosq` | 17.8M | 0 | 0 |
| MP-flag-movsb | scalar data, flag copied by `rep movsb` | 11.5M | 80,798 | 0 |
| MP-split16, MP-splitline | misaligned data and flag across 16 bytes, across a 128-byte line | 21.8M, 56.6M | 0, 0 | 0, 0 |
| MP-split4K | data across a 4K guest page, flag across a 16K host page | 23.3M | 0 | 0 |
| MP-split16K | data across a 16K host page | 3,867 (9,336 in a repeat) | 0 | 0 |
| LOCK-* (17 tests) | `lock xadd` of 16, 32 and 64 bits (64 also across a 128-byte line and the 16K page), `lock add`, `sub`, `xor`, `inc`, `dec`, `not`, `neg`, `or`/`and`, `cmpxchg` and `cmpxchg8b` loops, spin locks by `xchg` (plain release; and a value in both halves, `xchg` release) and by `cmpxchg8b` (plain release), with plain stores to the neighbouring bytes | exact | exact | exact |

So x86 ordering holds between threads with all three settings on, and the same harness
sees it broken with them off: Apple M-series cores reorder loads with loads and stores
with stores. `--expect weak` (the default with `--tso off`) requires each forbidden
outcome above that the M4 showed, except LB, and requires the fence, locked-instruction
and counting tests to hold, except the two spin locks with a plain release
(LOCK-xchg-split16, LOCK-cmpxchg8b-lock), whose release needs TSO and so may lose counts
with emulation off (none did here). Why LB never appears, and why a load that crosses a
16K page reorders far less often than one that crosses a 4K page within it, has not been
established; a plausible but unverified explanation is that the cores do not let a store
become visible before an older load has its value, and perform a page-crossing load
late.

With emulation on and at least two groups, a misaligned test must see some instruction
(host address in translated code) fault on two or more distinct threads over the run, so
that the core's back-patching under the block's spin lock runs concurrently; otherwise it
fails as NOT CONCURRENT. The tool's fault handler records which threads faulted at each
address, so a thread faulting again on its own copy does not count. With the start
barrier up to 3 or 4 distinct threads fault on one instruction; with the barrier removed
every default run failed NOT CONCURRENT (in 5 of 5 runs, three or four of the four
misaligned tests each time). (Translations are not all shared: the misaligned tests fault
at about 9 instruction addresses per run for 4 instructions, so some threads have their
own copy of a block.) The start barrier is what makes the concurrency happen: the guest barrier lines up only the two threads of one group,
which run different code, so the threads that run the same code (the same role in
different groups) used to reach its first, faulting execution at different times; after
the move to `fexmac` that skew left 4.0 to 4.4 faults per run, almost never more than one
thread per instruction (9 to 10 before the move, by the same skew being smaller).

**What each sub-setting protects.** With only the general setting on, scalar loads
become acquire loads (`ldapr`/`ldapur`) and scalar stores release stores
(`stlr`/`stlur`): `DEF_OP(LoadMemTSO)` and `DEF_OP(StoreMemTSO)` in
`FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp`. Vector loads and stores stay plain
`ldr`/`str` unless `FEX_VECTORTSOENABLED` adds a `dmb ishld` after the load or a `dmb ish`
before the store (same functions), and `rep movs`/`rep stos` stay plain copies unless
`FEX_MEMCPYSETTSOENABLED` makes each element an acquire load and release store
(`DEF_OP(MemCpy)`, `DEF_OP(MemSet)`, the `IsAtomic` branch). A release store orders every
earlier access before it and an acquire load orders every later access after it, so a
vector or string access followed by a scalar store, or preceded by a scalar load, is
ordered by the general setting alone: MP-sse, MP-movsb, MP-stosq and MP-movsb-read hold in
the scalar mode. What depends on the sub-settings is order between two vector accesses
(MP-sse-st: stores, MP-sse-ld: loads) and between two string accesses (MP-stos-stos,
MP-movs-movs: stores; MP-movsb-read2: loads), and a plain load before an acquire load
(MP-flag-movsb). A plain store after a release store (MP-flag-stosq) is also allowed to
pass it by the architecture but has not been seen to on the M4. `--expect scalar` (the
default with `--tso scalar`) requires each of those named tests to break and the rest to
hold, so a sub-setting that stopped ordering its accesses would be noticed.

The default run (`FEXCoreOrderingTest`, emulation on, 3.2 million tries per litmus test)
takes about 15 seconds, of which the counting tests are about 10: every locked operation
on a misaligned operand is a fault. `--tso off` and `--tso scalar` take the same. All three run
in the Mac job. The hosted Mac is a virtual machine on Apple silicon with three
processors: the checks that must hold are meaningful there (one pair at a time), but
whether the weak outcomes appear depends on the host, so there a test that should break
and does not only warns; a test that must hold and breaks, a stall, a crash, BROKEN or NOT
RACING fail the job. Only a physical Mac has shown the table above.

Found by it and fixed:

- **Lost wake-up in the core's reader-writer lock** (`WritePriorityMutex`, used for the
  code invalidation lock). On macOS readers and writers wait on the same word with
  `os_sync_wait_on_address`, which, unlike Linux futex bitsets, cannot wake only writers.
  A writer hand-off woke one waiter, which could be a reader; the reader went back to
  sleep and the writer never woke. Threads that started translating together hung about
  once in 300 to 1,500 runs of eight threads. A writer hand-off now wakes all waiters.
  `FEXCoreMutexWakeTest` checks six queueing orders, each step waiting until the thread
  is blocked in the kernel (its Mach run state), not for a fixed time. With the old
  header two of them hang every time: a writer owns while a reader then a writer queue
  (write-to-write hand-off), and a reader owns while a writer then a reader queue and a
  spurious wake moves the writer behind the reader (read-to-write hand-off); the other
  four pass with either header. 20,000 eight-thread starts ran clean after the fix.
- **Lost updates from locked operations on an operand that crosses 16 bytes.** ARM64
  atomics fault there and the core emulates them with two compare-and-swaps. With
  `StrictInProcessSplitLocks` off (upstream's default) concurrent emulations interleave
  and tear, and the update is lost: before the next fix, LOCK-xadd-split* lost between 1
  in 4,000 and 1 in 900 increments in every run. On macOS the setting now defaults to on
  (`FEXCore/Source/CMakeLists.txt`) and `fexmac` always sets it (it is not an option an
  embedder can change; see "Fault handler locks" in `docs/macOS-embedding-interface.md`
  for why taking that lock in the fault handler is safe). It serialises the emulated
  operations within the process; they already cost a fault each and the counting tests
  show no measurable slowdown. With the lock switched off in a scratch build,
  `fexmac_c_test split-locks-concurrent` (operands that change both halves) came out short
  in 3 of 3 runs; with it on it is exact.
- **A locked operation could report an old value it never wrote, or apply itself twice.**
  The emulation wrote the upper half first. If anything changed the lower half before the
  second compare-and-swap (another thread's plain store, such as a spin lock's release or
  a neighbouring field's update), the operation gave up as "torn"; for a read-modify-write
  it then either returned a stale result without completing or, if the bytes next to the
  operand had changed, started again on the half-written value and applied itself to the
  upper half twice; `cmpxchg8b` reported failure while memory held its new value. Fixed on
  macOS in `SplitCAS` (`FEXCore/Source/Utils/ArchHelpers/Arm64.cpp`), which every split
  path now uses (`DoCAS16`, `DoCAS32`, `DoCAS64` and `RunCASPAL`, the last for
  `cmpxchg8b`): the half the operation does not change is compared first, so that a
  failure of the second has written nothing and is retried; when both halves change and
  the second compare-and-swap fails only because the operand's neighbouring bytes
  changed, the lower half is written again with the new neighbouring bytes, completing the
  operation (at most 16 times); and after a real tear the operation is never started
  again on the torn value. The split-lock lock is released and taken again between
  attempts, so that plain stores that keep changing the word make the emulation retry,
  not hold the lock. Evidence: LOCK-xchg-split16 lost an increment in 3 of 10 long runs
  before the first part; `fexmac_c_test split-locks-every-form` (13 forms, with plain
  stores to the neighbouring bytes) gives a wrong result for 7 of the 13 forms in each of
  2 runs with the code before this round and none after; the ordering test's 17 counting
  tests (which add the same neighbour stores from guest code) are exact in every soak.
- **Asynchronous signals while the core held a lock.** A host that owns the fault
  signals runs `fexmac_handle_fault` with its own mask; an asynchronous handler could
  then interrupt the core while it held the split-lock lock or the patch lock (and the
  thread was switched to writing code), and a handler that left by a jump or stopped the
  thread would have left them so. `fexmac_handle_fault` now blocks asynchronous signals
  itself around the core's handling of misaligned accesses. `fexmac_hook_test
  misaligned-blocks-async-signals` checks it deterministically: a test hook sends the
  thread a signal where the core is about to take its locks, and the signal's handler must
  not run before the handling ends; without the blocking it runs inside in 1000 of 1000
  accesses, in 10 of 10 runs. `fexmac_c_test split-locks-signal-storm` is the stress
  version (a real signal storm until 3,000 signals have landed inside fault handling,
  exact totals, no stall), but it rarely catches the missing blocking (the reviewer saw 1
  failure in 7 runs; I saw none in 6): a signal sent to a thread that is running is
  delivered when it next enters the kernel, which for a thread taking faults is mostly at
  the fault, before the core's handling starts, and forcing interrupts (protection
  changes on a spare page, which make the kernel interrupt the process's other running
  threads) did not change that.

Remaining limits of split locked operations, none of them seen in the tests:

1. When an operation changes both halves, there is a moment between its two
   compare-and-swaps when the upper half is new and the lower half old. Any reader can
   see that value: a plain load, or a native atomic on part of the same bytes. x86 makes a
   locked operation atomic for every observer.
2. A natively executed aligned locked operation on part of the same bytes (an atomic
   that lies within one half, so it does not fault) bypasses the split-lock lock, so it
   is not ordered with the emulation: it can land between the two compare-and-swaps.
3. When both halves change and another thread changes the operand's own bytes in the
   lower half (not just neighbouring bytes) between the two compare-and-swaps, the
   operation is torn: the upper half has its new value, the lower half the other
   thread's, and the guest is told the operation failed (`cmpxchg`) or given the old value
   it read. It is not applied again.
4. The lock is per process (one context per process). Memory shared with another process
   is not covered.
5. When both halves change and neighbouring bytes of the lower word keep changing, the
   completion (rewriting the lower half with the new neighbouring bytes) gives up after
   16 attempts: the operation is then torn as in 3, upper half new, lower half old.

Inaccuracies in the previous write-up, corrected: the tests' spin-lock release is a plain
store of a whole qword, across both halves (it changes only the lower one, since the
upper is zero throughout); `--expect weak` does not require LOCK-xchg-split16 to hold (it
is report-only with emulation off, like LOCK-cmpxchg8b-lock); and the 16-bit
dual-compare-and-swap path is the one across a 16-byte boundary (a 16-bit operand that
crosses only an 8-byte boundary uses a single 128-bit compare-and-swap).

Known gap, not fixed: **the misaligned back-patch is cross-modifying code that the
architecture does not guarantee.** When a misaligned acquire load or release store
faults, the handler (`HandleUnalignedAccess` in
`FEXCore/Source/Utils/ArchHelpers/Arm64.cpp`, the LDAPR/LDAPUR and STLR/STLUR branches)
rewrites it while other threads may be running the same code:

- Store: the emitted pair is `nop; stlur`. The handler writes `dmb ish` over the `nop`,
  then `stur` over the `stlur`. A thread that has already executed the old `nop`, and
  then fetches the new `stur`, makes that store without the barrier: an earlier store
  may become visible after it.
- Load: the pair is `ldapur; nop`, rewritten to `ldur; dmb ishld` (barrier first). A
  thread that fetches the new `ldur` but still has the old `nop` (the two words can be in
  different instruction-cache lines, and no instruction synchronisation barrier is
  executed by the other thread) makes that load without the barrier.

Each case affects at most one access per thread per patched instruction, once, and has not
been observed. ARM guarantees concurrent modification only between B, BL, NOP, BRK, SVC,
HVC, SMC and ISB, so no order of writes fixes it. Two remedies were weighed. Patching a
single `nop` into a branch to an out-of-line stub would be safe but needs stub space for
every such access, a change to code generation. Leaving the instruction alone and
emulating it in the fault handler on every execution is safe and small, and was measured:
the four misaligned message-passing tests went from 0.3 seconds each to 55 to 63 seconds
(12.8 million faults per test, tens of microseconds per access under contention), which
would make every misaligned access that crosses 16 bytes hundreds of times slower in real
programs. Neither is done; the gap is upstream's design and is kept.

## Guest program and benchmark (criteria 7 and 8)

`unittests/macOS/Bench/` holds a benchmark written for the port: 18 kernels (CRC and
hashing, sorting, an interpreter with indirect jumps and calls, a state machine, the
string instructions including overlapping and direction-flag copies, a vector copy,
pointer chasing, scalar and vector matrix multiply, n-body, FFT, x87 `long double`,
64-bit multiply and divide, call/return and deep recursion, and two four-thread kernels
with a spin lock, atomics and message passing). The same C file is built as a
freestanding x86-64 image (static Mach-O at 2TB, no libc), as an x86-64 macOS program for
Rosetta 2 and as an arm64 program. `FEXCoreBench` (`Source/Tools/CoreBench/`,
`fexmac.h` only) loads and runs the image. Method, tables and analysis:
`docs/macOS-benchmarks.md`.

- **Criterion 7, literally: the literal mode.** The guest runs the 16 single-threaded
  kernels and makes exactly one system call, exit, carrying the number of wrong
  checksums and of kernels run; `FEXCoreBench --literal` refuses any other call and
  requires 0 wrong and all 16 run. It passes as built and hardened (quick size, in the
  Mac workflow) and in full size, with the ordering emulation on and off. The **timed
  mode** used for criterion 8 needs a wider private ABI (write, clock, start and join
  threads); it uses no Linux or macOS system call either.
- **Correct results.** Every checksum is bit-exact against Rosetta 2 and native arm64
  (x87 against Rosetta 2: `long double` is 64-bit on arm64), in every launch of every
  configuration measured, quick and full size. The checksums cover every byte the
  kernels write: 58 mutations of the kernels (shorter string instructions, skipped
  elements, lost updates, ...) each change their checksum
  (`Scripts/macos_bench_mutations.py`, in the Mac workflow). Stated exceptions: with the
  ordering emulation off the two thread kernels may be wrong (and the message-passing
  one is); with `X87REDUCEDPRECISION=1` the x87 checksum is a different recorded value.
  No translator bug was found.
- **Criterion 8.** The figures below are generated by `Scripts/macos_bench.py
  --write-docs` from `docs/macOS-benchmarks-results.json`, the same results as the
  tables in `docs/macOS-benchmarks.md`, and say whether they are provisional or final;
  `--check-docs` fails if they, or any figure outside them, do not match that file. The
  like-for-like comparison with Rosetta 2's hardware TSO is all ordering on (`tso_on`);
  ordering off is an upper bound only, not correct for multi-threaded guests. Nearly all
  of the gap to Rosetta 2 is the software emulation of x86 memory ordering, most of it
  the barriers on vector accesses (`docs/macOS-benchmarks.md` explains the mechanisms).
  Blocks compiled and loaded come from `fexmac`'s translation counters; two code-cache
  configurations (cold: a fresh private directory per launch; warm: a filled one) show
  the cache at work on this program (criterion 6).

<!-- bench-summary:begin -->
<!-- Generated by Scripts/macos_bench.py from docs/macOS-benchmarks-results.json; do not edit by hand. -->

**PROVISIONAL** measurement of `FEX-2611-286-g3b12faa03`, 2026-11-27T19:20:57-05:00: other processes kept 0.33 cores busy on average (at most 2.36) of the machine's 10; load average 1.9. Provisional numbers show the shape, not final values.

- **Headline** (15 single-threaded integer, memory and floating-point kernels), geometric mean of the time relative to Rosetta 2: all ordering on (`tso_on`), like for like with Rosetta 2's hardware TSO **2.07**; scalar ordering only (`tso_scalar`, FEX's defaults) **0.98**; scalar and vector ordering (`tso_vector`) **1.63**; scalar and string-instruction ordering (`tso_memcpy`) **1.21**; all on, multiblock off **2.30**; all on, reduced x87 precision **2.03**; all on, code cache cold **2.08**; all on, code cache warm **2.04**; native arm64 0.73; ordering off 0.91 (an upper bound only: not correct for multi-threaded guests).
- **Slowest against Rosetta 2 with all ordering on** (ratio; in brackets the same kernel with scalar ordering only and with ordering off): `copy_loop` 56.13 (tso_scalar 2.54, tso_off 2.48); `rep_movs` 4.87 (tso_scalar 0.19, tso_off 0.20); `matmul_f32x4` 4.82 (tso_scalar 1.05, tso_off 1.06); `fft` 4.22 (tso_scalar 1.45, tso_off 1.12); `nbody` 2.95 (tso_scalar 1.00, tso_off 1.00); `matmul_f64` 1.93 (tso_scalar 0.67, tso_off 0.68).
- **Vector ordering** (`tso_vector` against `tso_scalar`, ratios to Rosetta 2), kernels it slows by half or more: `copy_loop` 49.49 against 2.54; `matmul_f64` 1.79 against 0.67; `matmul_f32x4` 4.81 against 1.05; `nbody` 2.94 against 1.00; `fft` 4.23 against 1.45.
- **String-instruction ordering** (`tso_memcpy` against `tso_scalar`), kernels it slows by half or more: `rep_movs` 4.93 against 0.19.
- **Multiblock off** (`tso_on_nomb` against `tso_on`), kernels it slows by half or more: `calls` 5.19 against 1.85.
- **x87** (x86-64 builds only): Rosetta 2 557 ms; `tso_on` 0.72 of Rosetta 2's time; `tso_on_x87rp` 0.06 of Rosetta 2's time.
- **Thread kernels**, all ordering on against Rosetta 2 (in no mean): `threads_lock` 3.71 (launch medians 36.3 to 42.3 ms; Rosetta 2 9.2 to 12.8 ms); `threads_mp` 9.11 (launch medians 23.5 to 25.0 ms; Rosetta 2 2.6 to 2.7 ms).
- **Translation and the code cache**: `tso_on` compiles 86 blocks per launch; 587 with multiblock off; a cold-cache launch compiles 83 and loads 3; a warm-cache launch compiles 0 and loads 80. Launch to first kernel result: `tso_on` 0.064 s, `tso_on_cache_cold` 0.065 s, `tso_on_cache_warm` 0.064 s; whole launch: `tso_on` 8.64 s, `tso_on_cache_cold` 8.62 s, `tso_on_cache_warm` 8.36 s.
- **Clock read** (calibrated by the program, subtracted from the times): native-arm64 6 ns, rosetta 15 ns, tso_on 136 ns.
<!-- bench-summary:end -->

- **Rosetta 2 on this Mac** hangs, unkillably, at the launch of an unsigned x86-64
  program; the scripts sign the x86-64 builds, and every launch has a deadline.

The quick mode (checksums, the literal mode and the loader's contract cases), as built and
hardened, and the mutation test run in the Mac workflow; they assert no timing. The
hosted Mac may not have Rosetta 2, in which case the script compares with native arm64
only.

## On-disk code cache

Implemented by copying cached code into the live `MAP_JIT` buffer, using upstream's
per-block `DiskCache`; details in `docs/macOS-code-cache-design.md`. Switched on with
`FEX_DISKCACHE=1` and `FEX_DISKCACHEPATH=<dir>` (default `~/Library/Caches/fex-emu/DiskCache/`); an
embedder calls `Context::FlushDiskCache()` before exiting and can read
`Context::GetTranslationCounters()`.

Added by this port to upstream's cache: keying on the core's Mach-O UUID, CPU model,
enumerated options and the effective TSO emulation state; no caching of blocks with a
custom IR handler or forced-TSO instructions; a checked description per database; an
integrity hash (covering the index's extents too) and full bounds checks on every entry,
including entry points against the block's guest extents and code before the block tail,
before anything reaches executable memory; refusal (never narrowing) of directories and
files not private to the user, including ACL entries that grant write, opened without
following links; the size limit enforced; the index read once and parsed lazily; repair
of a partly written index record before every append; fork support; pruning only of
buckets unused for a week; a default thread implementation for the core's writer thread.
Fixed in the frontend: a RIP-relative displacement followed by another field (3DNow!) was
patched one byte off on load. Fixed in the store path: guest code that changed between
decoding and storing was filed under its new bytes. Details and the trust model:
`docs/macOS-code-cache-design.md`.

Physical Mac (M4), all four modes, 2,296 tests, through the fexmac-based runner, cold
pass then warm pass in one directory with six processes at a time, as built and
hardened: in the warm pass no test that passes compiled a block; the only blocks
compiled (6 per mode) belong to expected failures that stop at code the translator cannot
translate, which is never cached. Loaded 181,174 / 4,743 / 2,630 / 2,630 blocks (jit_1,
jit_500, jit_500_m, jit_500_tso); every result identical to the cold pass and to the run
without the cache; no entry rejected. 59 MB on disk. Also a jit_1 two-pass over `3DNow`,
`TwoByte`, `SelfModifyingCode` and `FEX_bugs` (regression run for the 3DNow! fix).

The runner registers the memory it maps as guest code (`fexmac_memory_map`, read/write/execute), with or
without the cache. Three expected failures (`PrimaryGroup/5_FF_05`, `_03_o64`,
`_03_o64_imm`, which jump to an address cut to 32 bits) therefore now stop with a guest
page fault at that address instead of dying of a host fault while the translator read
unmapped memory; `Known_Failures_macOS` says so.

Speed: little gained on these tiny tests. Pass times (as built): cold 59 s, warm 19 s;
hardened 53 s and 17 s. Per process, start-up dominates. In jit_1, whose database has
85,000 entries (10.7MB index): before the index was made lazy a warm run cost 8.5 ms
against 3.4 ms uncached (reading the index twice and parsing every entry took about
9 ms); after, 5.8 ms against 2.9 ms; with the fexmac runner on a quieter machine, 2.7 ms
warm and 2.7 ms uncached. The largest test (`FEX_bugs/XeSS_quadratic`, 65,538 blocks in
jit_1) took 72 ms compiling and 79 ms loading. A benefit has to be shown with real
programs (package G).

`Scripts/macos_code_cache_tests.py` (107 checks, as built and hardened): 37 kinds of
damage to the files, 300 randomly damaged caches, re-sealed entries with bad entry
points or tails, bad index records, a record torn by a dying process followed by
appends, seven configuration changes, a database under another configuration's name or
with a different format version or build, custom IR handlers, hardware and forced TSO,
guest code changed between decoding and storing, changed guest bytes at the same
address, the same code at three addresses, eight threads at once, fork with a cold and a
warm cache, the size limit, and the directory, file, ACL and link rules.

Found on the way: on APFS, processes creating the same new file at once can get ENOENT
from `openat(O_CREAT)`; the cache retries such a create briefly.

## Not implemented, by decision or for lack of a macOS method

- **Upstream's whole-section code cache (`FXCC`, `EnableCodeCachingWIP`).** It publishes
  code with `mremap`, which macOS lacks, so its macOS path still stops with an error. It
  is not needed: the per-block disk cache above does the job by copying.
- **Hardware memory ordering.** macOS gives ordinary processes no way to switch on the
  x86-style ordering mode, so FEX's software emulation is used. `prctl` is stubbed to
  report "not supported".
- **Huge-page and core-dump hints.** No macOS equivalent. Both are hints only.
- **CPU index (`getcpu`).** macOS does not expose it. It reports 0, as the Windows build
  does for the node.

## Not brought over from the earlier port

The earlier macOS port this work draws on (`alamshafil/fexcore-darwin`, itself built on
Bachata-S4 and AetherPS4) also carried iOS and Android handling: a double-mapped code
buffer, a buffer-reuse generation counter, and related diagnostics. Those files were left
at upstream and the macOS parts were written by hand.

## Test jobs (exit criterion 10)

Three jobs run the tests automatically. Each shows something the others cannot.

| Job | Where | Trigger | What it can show | What it cannot |
|---|---|---|---|---|
| `macos_core.yml` (this repository) | GitHub's hosted Mac (a virtual machine, Apple silicon) | push to `macos/**` | the build with the default toolchain, linkage, every test tool, the instruction tests in four modes, the code cache, the benchmark checksums, the sync script | write protection (the VM does not enforce it), the forbidden memory-ordering outcomes with emulation off (warn only), the Metal tiers; tests needing CPU features the VM lacks are skipped (14 instead of 6) |
| `physical_mac.yml` (the private umbrella repository, `jt226ub/Apple-Silicon-Translation-Runtime`) | the physical Mac mini (M4), self-hosted runner `m4-mini` registered to that private repository only | push to `physical-mac/**`, manual (FEX ref, quick/full/soak), nightly | everything above as built **and** hardened, with write protection enforced (`JITWriteProbe --expect-enforced`), the ordering test's off/scalar modes as hard requirements, the 1,000-start stress, all four modes of the two-pass cache run, the Xcode 26.5 toolchain, and the umbrella's MetalFX tests (all six, under Metal validation) | Linux; another Mac model |
| `linux_core_arm64.yml` (this repository) | GitHub's hosted `ubuntu-24.04-arm` (Azure Cobalt 100, 4K pages) | push to `macos/**` | that the port's changes do not break Linux: the `BUILD_FEXCORE_ONLY` build, the whole Linux build, upstream's 64-bit instruction tests through `TestHarnessRunner` and CTest (modes jit_1, jit_500, jit_500_m), upstream's FEXCore, API and emitter unit tests; all also on the upstream base commit, compared test by test | the port's own tools and `fexmac` (Apple-only by design), the TSO mode the Mac runs, anything Mac-specific |

No runner is registered on this public repository, and none may be: a self-hosted runner
on a public repository can be made to run a stranger's code. The physical-Mac workflow
lives in the private repository for that reason and checks this repository out read-only.

**Physical Mac, quick level, `macos/v1` at `e513ada49`**: every step PASS (machine
record, probe enforced, build, linkage, smoke, thread, `fexmac_c_test` 46 subtests,
`fexmac_hook_test` 9, mutex wake, ordering on/off/scalar, 1,000 starts, instruction tests
2,265 pass / 15 expected failures / 6 skips in all four modes both ways, code cache tests,
two-pass cache run, benchmark quick both ways, 58 benchmark mutations caught, sync script
tests, MetalFX 6 of 6 in all three runs); the write-protection SMC run is ABSENT on that
ref (package D adds it; the step runs it once `macos_asm_tests.py` has `--smc`). 13
minutes with the machine at load 20. Run URLs and the full level are in the umbrella's
`docs/testing/physical-mac-job-report.md`.

**Physical Mac, full level** (adds the Xcode 26.5 leg and the two-pass cache run in all
four modes): the Xcode leg passes every step with the same counts. The default leg found
two code cache faults, both reproduced by hand outside the job, fixed in package F:

1. `macos_code_cache_tests.py`, "six processes rotating one database, 72 runs": every
   answer right, but four generations left on disk where at most three may be (for
   example `-1`, `-2`, `-45`, `-46`), 2 of 8 runs by hand on `e513ada49`. Cause: the
   bucket was listed through a duplicate of its descriptor, which shares the read
   position, so every listing after a process's first saw nothing; a process retrying
   its open started again from generation 0, and rotations never saw what others had
   started. Fixed (each listing rewinds), and the cleanup now converges whatever the
   gaps: every open and rotation removes stale generations (older than N - 1), at most 16
   per call, each only under both of its locks; the test now checks that one more open
   after the storm leaves exactly N - 1 and N, and a new test plants 31 stale generations
   with gaps and one held locked. 16 of 16 script runs pass (8 as built, 8 hardened).
2. `--cache-passes 2` in all four modes: `FEX_bugs/XeSS_quadratic` in jit_1 (65,538
   one-instruction blocks) timed out (60 s) in the cold pass. Cause: the writer stored
   entry by entry (two `flock`s, five `fstat`s, seven `pwrite`s each, about 15 µs alone),
   so processes writing one database handed the lock to each other 65,538 times, the
   low-priority writer was starved on a busy machine, and the exit flush waited for all
   of it: six copies of the test into one database next to ten CPU-bound processes took
   139 s each (0.1 s without the cache). Now the writer appends in batches (one lock and
   one write per file per batch) and runs at the waiting thread's priority while a flush
   waits: the same six copies take 0.4 to 0.6 s (0.15 s without the cache), the test
   alone 0.33 s cold instead of 1.1 s. The four-mode two-pass run at the script's default
   parallelism passes cold and warm, 5 of 5 runs (cold pass 40 to 58 s). A new cache-script check runs six processes each
   cold-compiling 60,000 different blocks into one database on a busy machine and
   requires at most twice the time without the cache plus 2 s: 2.2 to 6.3 s against 1.6
   to 5.1 s (per-entry stores: 71 s against 2.4 s).

**Hosted Mac job changes (this package).** The instruction-test steps now also fail on
an expected failure that now passes (the script itself exits 0 for that); the old
diagnosis step, which ran `lldb` and `sample` on three tests that pass since the
scratch-buffer restart, is replaced by printing the unexpected rows of `results.tsv` when
a step failed. No step uses `continue-on-error`; the memory-ordering off/scalar runs still
only warn on the virtual machine when a forbidden outcome is not seen (documented in the
step), and fail on anything else. Run time about 12 minutes.

**Linux arm64.** On the port (with the fix below) and on its upstream base
(`a92baf073`): instruction tests 2,285 raw passes, 1 known failure
(`FEX_bugs/32bit_syscall`), 6 disabled by upstream's lists, in each of the three modes,
CTest 6,876 of 6,876 as upstream's lists expect; unit tests 355 of 355 (the four
`FutexSpin-Timed` tests, 8, 16, 32 and 64 bit, are left out: they time wall-clock waits
and failed at random on the upstream base too on these shared machines). An empty
comparison cannot pass: CTest runs with `--no-tests=error`, and the compare script fails
on an empty table, a missing mode or different row counts on the two sides. Port against base: 0 tests differ. The
core-only build and the full build have no warnings.

Linux against the Mac (`Scripts/macos_linux_asm_compare.py`, modes jit_1, jit_500,
jit_500_m): no test passes on one and fails on the other, apart from the 11 tests in
`Known_Failures_macOS` (fixed low addresses) and tests the Mac runner skips: 4 that need
memory below 4GB, `SecondaryModRM/Reg_7_1` (Linux system calls), and 5 for CPU features
the guest CPUID does not report (`VEX/sha512`, `avx_vnni_int8`, `avx_vnni_int16`,
`vpmadd52huq`, `vpmadd52luq`). Those 5 pass on Linux, where upstream's runner does not
check CPUID; the Mac runner could run them too (not changed here).

**Linux regression found and fixed.** `FEX_bugs/CodeBufferOverflow` failed in the
multiblock mode on Linux and passed on the upstream base. The macOS change to
`PooledAllocatorVirtualWithGuard` (`ThreadPoolAllocator.h`: a host-page guard placed
after the requested size, because 16K pages could swallow the whole buffer) was not
guarded, so on Linux the guard page moved one page past `JITGuardPage`, the address the
JIT and Linux's signal handler use to recognise a full scratch buffer; a full buffer then
crashed instead of restarting the translation. Upstream's layout is now kept on Linux,
macOS's unchanged.

**Second Linux break, found the same way.** After package D landed (`9c9ea7ecc`), FEXCore
no longer compiled on Linux: `DEF_OP(MemCpySrcBase)` in `JIT/MemoryOps.cpp` called
`EmitRepStringInPlace`, which is declared and defined for macOS only (the Linux job on
`macos/v1` went red, run 46076606725). The op is emitted on macOS only; on other hosts its
handler body is now a stop with a message. Linux green again with the fix.

Upstream's own workflows (`ccpp.yml`, `glibc_fault.yml`, `hostrunner.yml`,
`instcountci.yml`, `mingw_build.yml`, `steamrt4.yml`, `vixl_simulator.yml`,
`wine_dll_artifacts.yml`, `pr-code-format.yml`) trigger only on pushes to `main` or pull
requests to `main` and target upstream's self-hosted runners. They never run on `macos/**`
pushes, and none has run on this fork (only `macOS core build` is registered), so they are
left unchanged to keep upstream syncs clean.

**Instruction-test floor.** `Scripts/macos_asm_floor.txt` holds the minimum counts (2,296
tests found; 2,265 passes per mode on a physical Mac, 2,257 on the hosted VM) and
`Scripts/macos_asm_check.py` checks a `macos_asm_tests.py` log against it, also failing on
unprepared tests, a mode that ran nothing and expected failures that now pass. The hosted
Mac job and the physical-Mac job both use it; raise the floor when tests are added or
fixed. All actions in this repository's workflows are pinned to full commits.

**Repository settings the owner should apply** (not changed by any package):

- Settings, Actions, General: require approval for workflow runs from all outside
  contributors (not only first-time ones), so a fork's pull request cannot start a job
  unasked.
- Allow only actions created by GitHub (and require them pinned to a full commit), since
  the workflows use nothing else.
- Never register a self-hosted runner on this public repository; the physical-Mac job
  lives in the private umbrella repository and only reads this one.
- Consider disabling upstream's own workflows on this fork (Actions, select the workflow,
  Disable workflow): they cannot run usefully here, and disabling them in the settings
  keeps the files identical to upstream's.

## Tests

All of these run in the Mac job on every push to a `macos/**` branch.

| Test | What it does | Hosted Mac result | Physical Mac (M4) result |
|---|---|---|---|
| `FEXCoreSmokeTest` | Translates and runs a 23-byte loop, as built and under the hardened runtime | pass, both | pass, both |
| `FEXCoreThreadTest` | Eight host threads share a context, each summing a different count in a long guest loop | pass | pass, both |
| `FEXCoreCacheTest` | Eight threads run a chain of 200 blocks at once, with the code cache on; checks results that depend on every constant and on the code's address, and the cache counters | added in package F, not yet run there | pass, both; 150 of 150 runs after the mutex fix (hung about one run in 30 before it) |
| `Scripts/macos_code_cache_tests.py` | Damaged, mismatched and moved caches, fuzzing, threads, fork (with and without the fexmac calls), stalled lock holders, superseded entries, generations and size limits, six processes rotating at once (then exactly N - 1 and N after one open), stale generations with gaps and a locked one, six processes each storing 60,000 blocks into one database on a busy machine within twice the time without the cache, a planted generation name, cache states, directory rules (see above) | added in package F, not yet run there | 107 of 107, both, 8 runs each |
| `fexmac_c_test` cache subtests | `cache-off`, `cache-cold-warm`, `cache-cross-process`, `cache-untrusted`, `cache-untrusted-file`, `cache-unavailable`, `cache-options`, `cache-fork`, `code-ranges` | added in package F, not yet run there | pass, both |
| `Scripts/macos_asm_tests.py --cache-dir DIR --cache-passes 2` | The instruction tests cold then warm; fails if a warm result differs, a finished warm run compiled anything, or an entry was rejected | added in package F, not yet run there (two modes in the job) | pass, all four modes, both |
| `FEXCoreASMTestRunner` with `Scripts/macos_asm_tests.py` | Upstream's 2,292 assembly instruction tests plus the port's four in `unittests/macOS/ASM`, in four modes | 2,253 pass in each mode (before the upstream sync added one test) | 2,265 of 2,296 pass in each mode, both |
| `FEXCoreOrderingTest` | Multi-thread x86 memory-ordering litmus tests and misaligned locked-operation counts, in three emulation modes; see above | not yet run | pass, both, in each mode |
| `FEXCoreMutexWakeTest` | The core's reader-writer lock never loses a wake-up, in six queueing orders | not yet run | pass, both; with the bug restored, two orders hang |
| `fexmac_c_test` | A C program using only `fexmac.h`, linked as an outside project would be: 48 subtests covering large blocks (every size across the first scratch-buffer overflow, 8 threads translating large blocks at once), system calls (prefixed, restarted), stops, state, guest faults, invalidation (also from 8 threads at once), misaligned atomics with each signal-handler arrangement, call/return stack overflow and underflow, guest accesses to protected pages, chained handlers (one that leaves with siglongjmp, over 306 stack layouts; one that faults itself, which must end the process quickly; one that returns without fixing the fault) and sent signals, garbage struct sizes, lifetime races, cached translations and code ranges, split locked operations from 8 threads in every emulated form, also with plain stores to neighbouring bytes and under a signal storm (exact results), and error paths | not yet run | pass, both |
| `fexmac_hook_test` | The same library built with test hooks, 22 subtests: asynchronous signals held off while the core handles a misaligned access, translations overflowing the scratch buffer 0 to 5 times run correctly, translations moving through three code buffers (also from the code cache), a block ending exactly on a code buffer's last byte, 5,014 random-size blocks across six buffers, a first code buffer that cannot be made fails thread creation cleanly and a later one ends the process with an accurate message, a guest store into a code buffer's guard page is a guest fault, a block that never fits stops cleanly at its start and the thread stays usable, each code buffer ends in an inaccessible page, a write past the end of a code buffer dies with a message (also with a host handler chained), guest and callback accesses to guard pages are not claimed, the scratch-buffer guard is disarmed after each translation, a log callback cannot re-enter the library, a fault inside the handler crashes with a message instead of hanging, the log callback is never called from a signal handler, a failed create leaves no log hook, invalidation leaves fault signals unblocked | not yet run | pass, both |
| `FEXCoreBench` with `Scripts/macos_bench.py --quick` | The freestanding x86-64 benchmark, as built and hardened: the literal mode (no system call but exit) natively and under FEX; the loader's contract (9 cases of a guest breaking it); the timed mode (18 kernels, 4 guest threads) in four translator configurations, every checksum matching the recorded value and the native arm64 build (and Rosetta 2's where present), with the stated exceptions for ordering off and reduced x87 precision | not yet run (added after the last hosted run) | pass, both |
| `Scripts/macos_bench_mutations.py` | 58 mutations of the benchmark kernels, each of which must change its kernel's checksum (native arm64; x86-64-only ones under Rosetta 2) | not yet run | pass |
| `JITWriteProbe` | Reports how the machine treats executable memory, including writes and switches from a fault handler, instruction-cache flushing and the ways of placing a guard page after `MAP_JIT` memory | report only | see the facts above |
| `JITWriteProbe --expect-enforced` | The same, but exits 1 unless every asserted case gives the result the port relies on with write protection enforced, or if a fault-handler case's handler never ran | not run: the hosted Mac does not enforce, so it would fail | pass, as built and hardened |
| `Scripts/macos_check_linkage.sh` | Fails if the core or a test tool links anything but `libSystem` and `libc++`, if one of them is missing, or if the bundled `fmt` was not compiled. Homebrew's `fmt` is installed first. `otool` shows only that no fmt dylib is linked; the `build.ninja` check covers a static one | not yet run (added after the last hosted run) | pass, both toolchains |
| Low-memory probe | Shows that a program with a small page-zero reservation is killed | report only | same as hosted |

The instruction tests, per mode, on the most recent physical-Mac run (as built and
hardened, the same in all four modes). There are 2,296: upstream's 2,292 and the port's
four in `unittests/macOS/ASM` (named `Test_macOS/...`): `syscall_time`, `ud2`, `mov_fs`
and `tf_int80`. They live outside `unittests/ASM` because upstream's CMake collects
every `.asm` under that directory into its Linux tests; nothing of the port's runs there.
`asm-out/results.tsv` lists every test in every mode, including those not run, with the
reason; the script also prints each skip with its reason.

| Outcome | Count |
|---|---|
| Pass | 2,265 |
| Skipped by the runner | 6 |
| Skipped by upstream's own lists | 6 |
| Skipped because the test reaches low memory through a 32-bit operand | 4 |
| Expected failures: 3 crashes, 8 guest page-fault stops and 1 stop (`32bit_syscall`) | 12 |
| Port tests whose correct result is a stop (`ud2`, `mov_fs`, `tf_int80`) | 3 |

The script counts the last two rows together as 15 expected failures (3 crash, 12 fail).

Before package B, on the merged base (the fexmac runner), the counts were pass 2,255,
runner skips 8, low-memory skips 5 and 18 expected failures (12 crash, 6 fail). Nine
tests now pass: `FEX_bugs/cmpxchg8b`, `FEX_bugs/cmpxchg16b`, `TwoByte/0F_8B_16`,
`FEX_bugs/o16_leave`, `FEX_bugs/SIBScaleTranspose` (the move of the tests' memory had
broken their expected values, now moved too), `TwoByte/0F_02` (the runner now answers the
system call through the fexmac callback and resumes), `Secondary/09_XX_06` and `09_XX_07`
(RDRAND and RDSEED, through FEX's software generator) and `Primary/Primary_CF` (a
`mov esp, <address>` is now written as `mov rsp`). None of the remaining failures is a
translator or port fault.

**Expected failures are checked by outcome, not only by name.** Each entry in
`Known_Failures_macOS` states how the test must fail: `crash fault=<address>
signal=SIGSEGV|SIGBUS where=host|translated` (`fault=` is required), or a stop,
`guest-fault` or `unsupported-instruction`, with `signal=`, `trap=`, `si_code=` and
`at=<rip>`. A listed test that fails any other way (a wrong value, a timeout, a crash at
another address, in other code or with another signal, a different stop) is reported as
unexpected and fails the run; this was checked by changing one entry's address and
another's trap number, which both became unexpected failures. The evidence is in the
runner's output, kept in results.tsv. The runner owns SIGSEGV and SIGBUS (way 2 in
`fexmac.h`): each fault goes to `fexmac_handle_fault` first, and one the library does not
claim is printed as `runner: unclaimed SIGSEGV at address <a>, pc <p> in host|translated
code (ESR class <c>, fault status <s>)` before the process dies of the original signal.
"Host code" means the pc is in the `__TEXT` of an image loaded at startup (the runner,
with the core and the library linked in, or a system library); anything else is
translated code. A run that stops prints `Stopped: ... reason <n> signal <n> trap <n>
si_code <n> at <rip> ...`. (The runner used to chain its handler behind the library's;
that relied on changing the process-wide action from inside the library's chain, and the
library's contract does not say what happens when a previous handler returns without
resolving a fault: its handler returns too and the instruction faults again forever.)

**What `TwoByte/0F_02` and `macOS/syscall_time` prove.** The runner answers Linux `time`
(201) with the host's time (also stored through rdi when it is non-null and writable,
-EFAULT otherwise, also for a range that would wrap past the top of the address space) and every other system call with -ENOSYS. Upstream's `0F_02` does
`mov rax, 201; syscall` and passes if rax is then non-zero, which is also true if the call
never happens (rax stays 201) or returns an error, so its pass shows only that `syscall`
neither hangs nor faults and that execution continues after it. The port's
`macOS/syscall_time` checks the result: rax is a plausible current time (after November
2023, below 2^32), the value stored through rdi equals rax, rcx is the address of the
instruction after `syscall` and r11 holds the flags from before it. With `syscall` replaced
by two `nop`s it fails.

**Every test that does not pass, and why** (each run on the M4 in all four modes, with
the addresses from the runner's lines above):

- *Expected failures, 12, in every mode.* Eleven crash on an address cut to 32 bits,
  which lands below 4GB where macOS maps nothing:
  `FEX_bugs/a32_lods` and `a32_rep_lods` (fault at 0x80000), `Primary/Primary_A6_addrmod`,
  `Primary_A6_REP_addrmod` (0x10000200) and `Primary_AE_addrmod` (0x10000100) use the
  0x67 address-size prefix; `VEX/vmovq_vmovd_reg` reads `[edx]` (0xe0000000; the earlier
  description, a 32-bit vector move cutting the address, was wrong); all are data aborts
  in translated code (ESR class 0x24). `PrimaryGroup/5_FF_05` and the four `5_FF_05_03_*`
  far-call tests jump through an m16:32 pointer built with `lea edi, [rel .success]`: the
  jump and o64 forms fault at 0x10028, the code's original address, in host code (the
  translator reading the guest instructions there), the o32 forms first at 0xe0000ff0
  through `[esp]`. The twelfth is `FEX_bugs/32bit_syscall`, also on upstream's
  `Known_Failures_jit`: FEX refuses `int 0x80` in a 64-bit process, so no block can be
  compiled there. Upstream (and this port before) then jumped to host address 0; on macOS
  the run now stops with `FEXMAC_STOP_UNSUPPORTED_INSTRUCTION` at the instruction
  (rip = code + 0x1d), so the test fails cleanly.
- *Low memory, 4, not assembled.* The moved address does not fit the operand:
  `Displacement_Encoding` and `MemoryData` use a sign-extended 32-bit absolute
  displacement (reaches only the low 2GB), `Primary/Primary_A0` and `Primary_A2` use a
  32-bit `moffs` with the 0x67 prefix. No relocation can fix these: guest addresses are
  host addresses, and macOS maps nothing below 4GB.
- *Skipped by the runner, 6.* `SecondaryModRM/Reg_7_1` (RDTSCP) declares that it needs
  Linux system calls. `VEX/sha512`, `avx_vnni_int8`, `avx_vnni_int16`, `vpmadd52huq` and
  `vpmadd52luq` need SHA512, AVX-VNNI-INT8, AVX-VNNI-INT16 and AVX-IFMA, which FEX's
  CPUID never advertises (hard-coded 0 on every host) and its decoder does not implement:
  forced to run on the M4 they stop at "Invalid instruction". Still justified; the M4's
  own features are not the reason. The runner names the missing feature.
- *Upstream's lists, 6.* `Primary/Primary_E8`, `Primary_E9` (cannot be tested in a real
  environment), `X87/D9_F2`, `D9_F9` (rely on undefined behaviour), `X87/D9_F8` (rounding)
  and `Secondary/06_XX_00_2` (UMIP value FEX does not emulate). Upstream's entry
  `FEX_bugs/XeSS_quadratic.asm` lacks the `Test_` prefix, so it disables nothing, here or
  upstream; the test runs and passes.

**Untranslatable code (macOS core change).** When the frontend can translate no
instruction at an address (a valid x86 instruction FEX does not implement, about 15 cases:
`int 0x80` in 64-bit mode, `mov fs/gs, r` in 64-bit mode, `o16 iret`, some
address-size-override string forms, a missing LOCK handler; or bytes it cannot decode),
upstream's `CompileBlock` and `CompileSingleStep` return 0, and the three places that
branch to the result jump to host address 0: the dispatcher's compile path, its
single-step path (taken with the trap flag set, and also when `ExitFunctionLink` finds TF
set and returns to the loop top), and `ExitFunctionLink` when linking a block exit. No
fourth exists on macOS (the other dispatcher calls, to the system call, CPUID and
invalidation handlers, return into the loop). On macOS, `CompileBlockOrFault` and
`CompileSingleStepOrFault` instead enter the dispatcher's synchronous-fault entry with an
invalid-opcode fault at the instruction marked `Untranslatable` and with si_code 0, and
fexmac turns that into `FEXMAC_STOP_UNSUPPORTED_INSTRUCTION` (signal SIGILL, trap 6, so a
host that only knows guest faults can still deliver it as #UD). A real #UD such as `ud2`
stays `FEXMAC_STOP_GUEST_FAULT` with si_code 2 (ILL_ILLOPN). `ExitFunctionLink` does not
link a block to the fault entry, since the code there may become valid. Port tests:
`macOS/ud2` (guest fault), `macOS/mov_fs` (unsupported), `macOS/tf_int80` (unsupported,
through the single-step path; with that path's fallback removed, it crashes at pc 0 as the
reviewer's probe did), and upstream's `FEX_bugs/32bit_syscall` (the dispatcher path in
`jit_1`, the linking path in the others). The route from `ExitFunctionLink` does not reset
the host stack as `DEF_OP(Break)` does on its way to the same entry; harmless while the
handler never returns (see `docs/macOS-embedding-interface.md`, package D).
`ExitFunctionLink` also cleaned the data-cache line at the target block's address (`dc
cvau` given `Record->HostCode`, the value) where the location written is the record's
`HostCode` field; on macOS it now cleans the field's own line. Linux keeps upstream's code
for all of this.

**Known upstream behaviour, not macOS failures.** FEX's CPUID reports an Intel CPU
(`GenuineIntel`, `CPUID_AMD` is not defined), but far jumps and calls through memory
(`FF /5`, `FF /3`) always read a 32-bit offset, also with REX.W, which is AMD's behaviour
(Intel reads m16:64 with REX.W); a far call with REX.W still pushes 64-bit values. The
intended guest, a console with an AMD CPU, expects the AMD behaviour. And `iret` without
REX.W (`o32 iret`) pops 64-bit values in 64-bit mode, where x86 pops 32-bit ones:
`IRETOp` pops at the GPR size and refuses only the 16-bit form.

**Host features.** The runner's feature check reads the host's `hw.optional.arm.FEAT_*`
sysctls and the CPUID the translator gives the guest. On the M4 it is right: no SVE (so
AVX is translated as two 128-bit halves; the AVX tests run), FEAT_AFP, BF16, SHA3 and
SHA512 present, and no FEAT_RNG (the M4 has no such sysctl and `mrs RNDR` raises SIGILL),
so RDRAND and RDSEED are offered to the guest only with `FEX_SOFTWARERNG=1`, which the test
script sets on macOS (fexmac accepts `SOFTWARERNG` as a 0/1 option). x87 80-bit arithmetic is emulated in software and needs no host
feature; the x87 tests run and pass.

**How the script moves addresses.** Besides moving every constant that names a mapped
low region, it: writes `mov r32, <low address>` as the 64-bit move (identical for an
address below 4GB); moves an expected register value lying within 64KB above a region
(an address whose low 16 bits an instruction replaced); and, for a register the test
loads with a RIP-relative `lea`, moves an expected value that is an address in the test's
code, mapping a label's address through nasm's symbol map of the test before and after
the move, since moved immediates lengthen instructions. `EXTRA_LOW_ADDRESSES` in the
script names the one address that is built by arithmetic (the base register of
`SIBScaleTranspose`); with only the base moved, a transposed base and index lands on an
unmapped address, so the test still catches the bug it exists for (checked with the
operands swapped: it faults at 0x800a0000000). The base is now loaded with a 10-byte
`mov` of a 41-bit constant; that does not reduce what the test covers. The bug it was
written for (upstream `920fe6049`, "ConstProp: Fix bug with transposed elements from
AddShift op") swapped the two register sources of an `AddShift` when folding it into a
scaled-register load, a choice that does not look at the registers' values; the test
breaks the block (`jmp .test`) before each load, so base and index reach the address
computation as registers, not constants, whatever their size. That pass is gone
upstream; today the same folding is `SelectAddressMode`'s scaled-register case, which
also takes registers only, and the index register (`0x2000_0000`) is unchanged.

**Regression test for the fault-time patching fix.** The 24 instruction tests that hung
in `jit_500_tso` (listed in the round 1 report; for example `Test_TwoByte/0F_B0_3.asm`
and `Test_FEX_bugs/UnalignedLoadStoreSIGBUS.asm`), run on a physical Mac, together with
`JITWriteProbe --expect-enforced` there, which checks that an atomic operation on
protected code memory faults, that a fault inside a fault handler hangs, and that
switching inside a handler works. **GitHub's hosted Mac cannot detect a regression
here:** it does not enforce write protection, so those tests passed there even before
the fix. Only the physical-Mac run counts.

The fourth mode, `jit_500_tso`, is this port's addition. Upstream runs these tests with
x86 memory-ordering emulation off; this mode turns it on.

32-bit guest tests (`unittests/32Bit_ASM`) are not run: they need memory below 4GB.

`macos/core-port-smoke` is `macos/core-port` plus the smoke test, kept only so the two can
be compared on a physical Mac.
