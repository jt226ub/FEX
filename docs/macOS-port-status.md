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
  under Rosetta 2 and natively on arm64. Speed is recorded (criterion 8), provisionally
  (machine loaded): with all of x86 memory ordering emulated, which is what Rosetta 2
  gets from hardware, about 2.1 times Rosetta 2's time over the single-threaded kernels.
  See "Guest program and benchmark" below.
- **Linux is unaffected.** The core-only Linux build passes with the same changes.

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
4. **Code-buffer guard page.** Skipped on macOS. Without this the core logs
   `Failed to mprotect last page of code buffer`.
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
- **`mprotect` of a page inside a `MAP_JIT` mapping is refused.** Guard pages and
  page-protection self-modifying-code detection cannot be built that way inside the code
  buffer.
- **A fault inside a SIGBUS handler hangs the thread** instead of killing the process,
  because the signal is blocked while its handler runs. A hang in a test can be a crash
  in a fault handler.
- **Returning from a signal handler does not restore the thread's write setting.** A
  handler that switches leaves the setting it last chose. The library's fault handler (and
  before it `MacOSHostFaults.h`) handles these kinds of fault. Misaligned accesses in translated code are patched with the setting
  switched, and translated code only runs executable, so the handler ends in the state
  it found. Scratch-buffer guard faults in the translator's own emitter code are
  resumed without touching the setting at all. Neither path leaves a wrong setting.
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

1. **The interrupt fault page and guest signals.** Not exercised.
2. **Self-modifying code detected by write-protecting pages.** Only the
   check-on-every-block kind is exercised.
3. **Speed.** Nothing has been measured.

## x86 memory ordering between threads

`FEXCoreOrderingTest` runs litmus tests: two guest threads (hand-assembled x86-64) make a
few accesses to shared memory, many times over, meeting at a barrier in guest code before
each try and waiting a small random time after it. Four such pairs run at once on the
same translated code; each run copies the code to a fresh address so it is translated
again. Outcomes are counted. Pinning threads to cores is not possible on macOS; the
threads ask for the user-interactive quality of service and the tool relies on numbers.

Every test must also show an outcome that is only possible when its two threads overlap
(for message passing: the flag not yet written but some data already; for SB: both loads
seeing both stores), or it fails as NOT RACING in every mode, so no test can pass because
its threads stopped overlapping. Counting tests instead run locked operations from every
thread of every group on one misaligned word and check the exact total.

Results on the Mac mini (M4), hardened runtime, `--soak`: 100 million tries per litmus
test, 8 million locked operations per counting test, in each of the three modes. "On" is
all three settings (`FEX_TSOENABLED`, `FEX_VECTORTSOENABLED`, `FEX_MEMCPYSETTSOENABLED`);
"scalar" is only the first, as upstream's defaults; "off" is none.

| Test | What it checks | Forbidden outcome, off | scalar | on |
|---|---|---|---|---|
| SB | store buffering; allowed by x86 (relaxed outcome counted) | 27.2M relaxed | 0.46M relaxed | 0.45M relaxed |
| SB+mfence, SB+lock | Dekker: `mfence` or a locked instruction between store and load | 0, 0 | 0, 0 | 0, 0 |
| MP, MP+spin | message passing, plain 64-bit moves | 18.8M, 0.84M | 0, 0 | 0, 0 |
| S | store then store against load then store | 172,771 | 0 | 0 |
| 2+2W | two threads each storing twice | 187,567 | 0 | 0 |
| LB | load buffering | 0 | 0 | 0 |
| MP-sse | `movdqu` data, scalar flag | 18.9M | 0 | 0 |
| MP-sse2 | `movdqu` data and flag, both sides | 17.7M | 3.69M | 0 |
| MP-sse-st | `movdqu` stores, scalar loads | 16.6M | 45,941 | 0 |
| MP-sse-ld | scalar stores, `movdqu` loads | 20.8M | 108,934 | 0 |
| MP-movsb, MP-stosq | data by `rep movsb` / `rep stosq`, scalar flag | 21.3M, 18.9M | 0, 0 | 0, 0 |
| MP-stos-stos, MP-movs-movs | data and flag both by `rep stosq` / `rep movsb` | 18.6M, 15.8M | 993, 6,944 | 0, 0 |
| MP-movsb-read | scalar flag load, data copied by `rep movsb` | 11.1M | 0 | 0 |
| MP-movsb-read2 | flag and data both copied by `rep movsb` | 7.8M | 9,652 | 0 |
| MP-flag-stosq | scalar data, flag by `rep stosq` | 15.8M | 0 | 0 |
| MP-flag-movsb | scalar data, flag copied by `rep movsb` | 10.1M | 92,596 | 0 |
| MP-split16, MP-splitline | misaligned data and flag across 16 bytes, across a 128-byte line | 17.5M, 21.5M | 0, 0 | 0, 0 |
| MP-split4K | data across a 4K guest page, flag across a 16K host page | 20.8M | 0 | 0 |
| MP-split16K | data across a 16K host page | 31 | 0 | 0 |
| LOCK-xadd-split16/-splitline/-splitpage | `lock xadd` on a qword crossing 16 bytes, a line, a page | exact | exact | exact |
| LOCK-cmpxchg-split16 | `lock cmpxchg` increment loop across 16 bytes | exact | exact | exact |
| LOCK-xchg-split16 | spin lock by `xchg` across 16 bytes, released by a plain store | exact | exact | exact |

So x86 ordering holds between threads with all three settings on, and the same harness
sees it broken with them off: Apple M-series cores reorder loads with loads and stores
with stores. `--expect weak` (the default with `--tso off`) requires each forbidden
outcome above that the M4 showed, except LB and MP-split16K, and requires the fence,
locked-instruction and counting tests to hold. Why LB never appears, and why a load that
crosses a 16K page almost never completes out of order, has not been established; a
plausible but unverified explanation is that the cores do not let a store become visible
before an older load has its value, and perform a page-crossing load late, in order.

With emulation on, the misaligned tests take more alignment faults per run than the 4
instructions they patch, so in some runs several threads fault on the same instruction at
once and the back-patching under the block's spin lock runs concurrently: 9 to 10 per run
when the tool set up its threads itself, 4.0 to 4.4 per run (403 to 436 over 100) since
it runs on `fexmac`, so concurrent faults on one instruction are now the exception rather
than the rule.

The tool now uses only `fexmac.h` (it owns SIGBUS and SIGSEGV and hands each fault to
`fexmac_handle_fault`, counting the alignment faults the library claims). The three
soaks were repeated on it, hardened: all pass, with no forbidden outcome with emulation
on, every required one with it off and in the scalar mode, and exact counting totals
(MP-split16K showed 19 with emulation off, LB none).

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
takes about 11 seconds, of which the counting tests are about 6: every locked operation on
a misaligned word is a fault. `--tso off` and `--tso scalar` take the same. All three run
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
- **Lost updates from locked operations on a word that crosses 16 bytes.** ARM64 atomics
  fault there and the core emulates them with two compare-and-swaps. With
  `StrictInProcessSplitLocks` off (upstream's default) concurrent emulations interleave
  and tear, and the update is lost: before the next fix, LOCK-xadd-split* lost between 1
  in 4,000 and 1 in 900 increments in every run. On macOS the setting now defaults to on
  (`FEXCore/Source/CMakeLists.txt`) and `fexmac` always sets it (it is not an option an
  embedder can change; see "Fault handler locks" in `docs/macOS-embedding-interface.md`
  for why taking that lock in the fault handler is safe). It serialises the emulated
  operations within the process; they already cost a fault each and the counting tests
  show no measurable slowdown. The ordering test's counting tests and `fexmac_c_test
  split-locks-concurrent` add 0x100000001 per operation, which changes both halves of the
  word: with the lock switched off in a scratch build, the C subtest's three `lock xadd`
  totals came out short in 3 of 3 runs; with it on they are exact.
- **A locked operation could report an old value it never wrote.** The emulation wrote
  the upper half first; if anything (another emulation, or a plain store such as a spin
  lock's release) changed the lower half before the second compare-and-swap, the
  operation gave up as "torn" and returned a mix of old values without completing. For
  an `xchg` lock that meant two threads held the lock: LOCK-xchg-split16 lost an
  increment in 3 of 10 long runs (and in 1 to 3 of 100 soak runs). On macOS the half that
  the operation does not change is now compared first, so a failure of the second has
  written nothing and is retried (`SplitCAS` in
  `FEXCore/Source/Utils/ArchHelpers/Arm64.cpp`); 10 long runs and the soaks are clean.

Remaining limits of split locked operations: the split-lock lock makes emulations atomic
with each other, and `SplitCAS` keeps an operation that changes only one half from
completing with a stale result when a plain store intervenes (it retries instead). An operation that changes both halves can still tear if a plain store
from another thread (one that is not a locked operation, so not under the lock) lands
between its two compare-and-swaps: the upper half is then written and the lower is not,
and the guest gets a mixed result. x86 promises a locked operation is atomic even against
plain stores, which two compare-and-swaps cannot give. Not observed (the tests' plain
stores only touch the lower half of their lock word).

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
  kernels write: 53 mutations of the kernels (shorter string instructions, skipped
  elements, lost updates, ...) each change their checksum
  (`Scripts/macos_bench_mutations.py`, in the Mac workflow). Stated exceptions: with the
  ordering emulation off the two thread kernels may be wrong (and the message-passing
  one is); with `X87REDUCEDPRECISION=1` the x87 checksum is a different recorded value.
  No translator bug was found.
- **Criterion 8, provisional numbers** (Mac mini M4, macOS 27.0.1, 27 November 2026,
  other processes using about 2 to 3 of 10 cores on average and up to 9; the final
  measurement is to be made on an idle machine with the same script). Geometric mean of
  the time relative to Rosetta 2 over the 15 single-threaded non-x87 kernels, two runs:
  - **all ordering on (`tso_on`), the like-for-like comparison with Rosetta 2's
    hardware TSO: 2.10 and 2.23**;
  - scalar ordering only (FEX's defaults): 1.00 and 1.06; plus vector ordering: 1.80
    and 2.13; plus string-instruction ordering instead: 1.17 and 1.24;
  - native arm64: 0.77 and 0.89;
  - ordering off: 0.85 and 0.96, an upper bound only for a guest known not to need
    ordering (it is not correct for multi-threaded guests).
  Worst with ordering on: SSE kernels (a `dmb` after every vector load and before every
  vector store: copy loop 21 to 63 times Rosetta 2 and very load-dependent, matrix
  multiply 5x, FFT 4.3x), `rep movs` 4.2x (one ordered byte load and store per byte),
  the interpreter 1.9x (`ldapr`/`stlr` on every access). x87 (x86-64 builds only): FEX
  0.7x Rosetta 2's time. Thread kernels are reported with their spread and in no mean.
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

The runner registers the memory it maps as guest code (`fexmac_add_code_range`), with or
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

`Scripts/macos_code_cache_tests.py` (93 checks, as built and hardened): 37 kinds of
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

## Tests

All of these run in the Mac job on every push to a `macos/**` branch.

| Test | What it does | Hosted Mac result | Physical Mac (M4) result |
|---|---|---|---|
| `FEXCoreSmokeTest` | Translates and runs a 23-byte loop, as built and under the hardened runtime | pass, both | pass, both |
| `FEXCoreThreadTest` | Eight host threads share a context, each summing a different count in a long guest loop | pass | pass, both |
| `FEXCoreCacheTest` | Eight threads run a chain of 200 blocks at once, with the code cache on; checks results that depend on every constant and on the code's address, and the cache counters | added in package F, not yet run there | pass, both; 150 of 150 runs after the mutex fix (hung about one run in 30 before it) |
| `Scripts/macos_code_cache_tests.py` | Damaged, mismatched and moved caches, fuzzing, threads, fork, directory rules (see above) | added in package F, not yet run there | 93 of 93, both |
| `fexmac_c_test` cache subtests | `cache-off`, `cache-cold-warm`, `cache-cross-process`, `cache-untrusted`, `cache-options`, `cache-fork`, `code-ranges` (with the rest: 41 subtests) | added in package F, not yet run there | pass, both |
| `Scripts/macos_asm_tests.py --cache-dir DIR --cache-passes 2` | The instruction tests cold then warm; fails if a warm result differs, a finished warm run compiled anything, or an entry was rejected | added in package F, not yet run there (two modes in the job) | pass, all four modes, both |
| `FEXCoreASMTestRunner` with `Scripts/macos_asm_tests.py` | Upstream's 2,292 assembly instruction tests plus the port's four in `unittests/macOS/ASM`, in four modes | 2,253 pass in each mode (before the upstream sync added one test) | 2,265 of 2,296 pass in each mode, both |
| `FEXCoreOrderingTest` | Multi-thread x86 memory-ordering litmus tests and misaligned locked-operation counts, in three emulation modes; see above | not yet run | pass, both, in each mode |
| `FEXCoreMutexWakeTest` | The core's reader-writer lock never loses a wake-up, in six queueing orders | not yet run | pass, both; with the bug restored, two orders hang |
| `fexmac_c_test` | A C program using only `fexmac.h`, linked as an outside project would be: 35 subtests covering system calls (prefixed, restarted), stops, state, guest faults, invalidation (also from 8 threads at once), misaligned atomics with each signal-handler arrangement, call/return stack overflow and underflow, guest accesses to protected pages, chained handlers (one that leaves with siglongjmp, over 306 stack layouts; one that faults itself, which must end the process quickly; one that returns without fixing the fault) and sent signals, garbage struct sizes, lifetime races, split locked operations from 8 threads (exact totals) and error paths | not yet run | pass, both |
| `fexmac_hook_test` | The same library built with test hooks, 9 subtests: guest and callback accesses to guard pages are not claimed, the scratch-buffer guard is disarmed after each translation, a log callback cannot re-enter the library, a fault inside the handler crashes with a message instead of hanging, the log callback is never called from a signal handler, a failed create leaves no log hook, invalidation leaves fault signals unblocked | not yet run | pass, both |
| `FEXCoreBench` with `Scripts/macos_bench.py --quick` | The freestanding x86-64 benchmark, as built and hardened: the literal mode (no system call but exit) natively and under FEX; the loader's contract (9 cases of a guest breaking it); the timed mode (18 kernels, 4 guest threads) in four translator configurations, every checksum matching the recorded value and the native arm64 build (and Rosetta 2's where present), with the stated exceptions for ordering off and reduced x87 precision | not yet run (added after the last hosted run) | pass, both |
| `Scripts/macos_bench_mutations.py` | 53 mutations of the benchmark kernels, each of which must change its kernel's checksum (native arm64; x86-64-only ones under Rosetta 2) | not yet run | pass |
| `JITWriteProbe` | Reports how the machine treats executable memory, including writes and switches from a fault handler and instruction-cache flushing | report only | see the facts above |
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
| Expected failures: 11 crashes and 1 stop (`32bit_syscall`) | 12 |
| Port tests whose correct result is a stop (`ud2`, `mov_fs`, `tf_int80`) | 3 |

The script counts the last two rows together as 15 expected failures (11 crash, 4 fail).

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
