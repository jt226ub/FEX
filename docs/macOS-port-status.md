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

## Where it stands

- **Builds and links on Apple silicon.** The core libraries and the test tools build with
  no errors with the macOS 27 SDK (command line tools, Apple clang 2100.4.1.1) and with
  Xcode 26.5 (macOS 26.5 SDK). The dylib and the test tools depend only on `libSystem`
  and `libc++`; the bundled `fmt` and `range-v3` are always used on macOS, even when
  Homebrew has its own.
- **Passes on a physical Mac.** On a Mac mini (Apple M4, macOS 27.0.1, 16K pages), as
  built and signed for the hardened runtime with only the JIT entitlement: the smoke and
  multi-thread tests pass, and 2,255 of upstream's 2,292 instruction tests pass in each
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

- `Source/Tools/CommonTools/MacOSHostFaults.h`: catches the alignment fault ARM64 raises
  for a misaligned atomic operation and hands it to the core (53 instruction tests depend
  on it), and restarts translation when the translator's scratch buffer fills.
- The test runner implements the callback that discards stale translations when guest
  code changes, holding the core's invalidation lock. The self-modifying-code tests
  looped forever until it did both.
- `Source/Tools/CommonTools/CoreThreadSetup.h`: allocates the per-thread call and return
  stack and a full descriptor table. Every guest `call` and `ret` depends on the first.
- `docs/macOS-embedding-interface.md` lists every such duty found so far.

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
  handler that switches leaves the setting it last chose. `MacOSHostFaults.h` handles two
  kinds of fault. Misaligned accesses in translated code are patched with the setting
  switched, and translated code only runs executable, so the handler ends in the state
  it found. Scratch-buffer guard faults in the translator's own emitter code are
  resumed without touching the setting at all. Neither path leaves a wrong setting.
- **Flushing the instruction cache while still writable is enough.** Rewriting the same
  code 50,000 times with a different result each time, flushing before switching back,
  always ran the new code; flushing after switching back did too; never flushing ran
  stale code, which shows the test can detect it. Single thread, one address.
- **Reading `ctr_el0`, `midr_el1` or `id_aa64isar0_el1` from user space faults**
  (SIGILL); the port uses `sysctl` for all of them. **`dc cvau` is allowed.**
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

With emulation on, the misaligned tests take 9 to 10 alignment faults per run where 4
instructions are patched, so several threads fault on the same instruction at once and
the back-patching under the block's spin lock runs concurrently.

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
  fault there and the core emulates them with two compare-and-swaps. By default
  (`StrictInProcessSplitLocks` off) concurrent emulations interleave and tear, and the
  increment is dropped: LOCK-xadd-split* lost between 1 in 4,000 and 1 in 900 increments in every run.
  On macOS that setting now defaults to on (`FEXCore/Source/CMakeLists.txt`), which
  serialises the emulated operations within the process; they already cost a fault each
  and the counting tests show no measurable slowdown from the lock.
- **A locked operation could report an old value it never wrote.** Even serialised, the
  emulation wrote the upper half first; if a plain store (such as a spin lock's release)
  changed the lower half before the second compare-and-swap, the operation gave up as
  "torn" and returned a mix of old values. For an `xchg` lock that meant two threads held
  the lock: LOCK-xchg-split16 lost an increment in 3 of 10 long runs (and in 1 to 3 of 100
  soak runs). On macOS the half that the operation does not change is now compared first,
  so a failure of the second is retried instead (`SplitCAS` in
  `FEXCore/Source/Utils/ArchHelpers/Arm64.cpp`); 10 long runs and the soaks are clean.
  When both halves change, a tear is still possible; x86 promises a locked operation is
  atomic even against plain stores, which two compare-and-swaps cannot give.

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

## Not implemented, by decision or for lack of a macOS method

- **On-disk code cache loading.** Linux publishes cached code with `mremap`. macOS has no
  `mremap` and will not remap `MAP_JIT` memory. The macOS path stops with an error until it
  has its own design.
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
| `FEXCoreASMTestRunner` with `Scripts/macos_asm_tests.py` | Upstream's 2,292 assembly instruction tests, in four modes | 2,253 pass in each mode (before the upstream sync added one test) | 2,255 pass in each mode, both |
| `FEXCoreOrderingTest` | Multi-thread x86 memory-ordering litmus tests and misaligned locked-operation counts, in three emulation modes; see above | not yet run | pass, both, in each mode |
| `FEXCoreMutexWakeTest` | The core's reader-writer lock never loses a wake-up, in six queueing orders | not yet run | pass, both; with the bug restored, two orders hang |
| `JITWriteProbe` | Reports how the machine treats executable memory, including writes and switches from a fault handler and instruction-cache flushing | report only | see the facts above |
| `JITWriteProbe --expect-enforced` | The same, but exits 1 unless every asserted case gives the result the port relies on with write protection enforced, or if a fault-handler case's handler never ran | not run: the hosted Mac does not enforce, so it would fail | pass, as built and hardened |
| `Scripts/macos_check_linkage.sh` | Fails if the core or a test tool links anything but `libSystem` and `libc++`, if one of them is missing, or if the bundled `fmt` was not compiled. Homebrew's `fmt` is installed first. `otool` shows only that no fmt dylib is linked; the `build.ninja` check covers a static one | not yet run (added after the last hosted run) | pass, both toolchains |
| Low-memory probe | Shows that a program with a small page-zero reservation is killed | report only | same as hosted |

The instruction tests, per mode, on the most recent physical-Mac run (as built and
hardened, with either toolchain, the same):

| Outcome | Count |
|---|---|
| Pass (2,254 before the upstream sync at `a92baf073` added `FEX_bugs/shufps_single_source`) | 2,255 |
| Skipped by the runner (host lacks a CPU feature the test needs, or it needs Linux system calls) | 8 |
| Skipped by upstream's own lists | 6 |
| Skipped because the test reaches low memory through a 32-bit operand | 5 |
| Expected failures | 18 |

The skip count depends on the CPU: the hosted Mac skips one more test than the M4, and an
earlier hosted run on a different virtual CPU skipped 16 and passed 2,244 with the same
code paths.

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

`unittests/ASM/Known_Failures_macOS` lists 17 of the expected failures: 16 believed to be
side effects of moving the tests' memory above 4GB (one confirmed in a debugger, the rest
not checked one by one) and one that needs a Linux system call. The eighteenth has not
been identified; it is presumably covered by one of upstream's lists, and
`asm-out/results.tsv` from any run will name it.

32-bit guest tests (`unittests/32Bit_ASM`) are not run: they need memory below 4GB.

`macos/core-port-smoke` is `macos/core-port` plus the smoke test, kept only so the two can
be compared on a physical Mac.
