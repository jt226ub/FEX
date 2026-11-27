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
  with it off (physical Mac).
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
  inside the handler crashes the process instead of hanging it (a reproducer hangs
  without it on the M4).
- Invalidation discards stale translations when guest code changes, holding the core's
  invalidation lock, for every thread of the context. The self-modifying-code tests
  looped forever until the runner did this. With many threads invalidating at once it
  hung until package E's reader-writer lock fix; `fexmac_c_test invalidate-concurrent`
  now passes.
- `Source/Tools/CommonTools/MacOSHostFaults.h` and `CoreThreadSetup.h`, the previous
  per-tool versions, are used only by `FEXCoreOrderingTest` (package E) and kept until it
  is ported; `MacOSHostFaults.h` got the same fault-status check.

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

Results on the Mac mini (M4), hardened runtime, `--soak` (100 million tries per test):

| Test | What it checks | Forbidden by x86, emulation off | Emulation on |
|---|---|---|---|
| SB | store buffering; allowed by x86, shows the threads race | relaxed outcome 27.8M | relaxed outcome 0.58M (allowed) |
| MP, MP+spin | message passing, plain 64-bit moves | 14.3M, 0.52M | 0 |
| S | store then store against load then store | 185,129 | 0 |
| 2+2W | two threads each storing twice | 271,950 | 0 |
| LB | load buffering | 0 | 0 |
| MP-sse, MP-sse2 | 16-byte `movdqu` data, scalar or `movdqu` flag | 14.0M, 14.3M | 0 |
| MP-movsb, MP-stosq, MP-movsb-read | data written or read by `rep movsb` / `rep stosq` | 18.6M, 15.2M, 8.6M | 0 |
| MP-flag-stosq, MP-flag-movsb | flag written or read by a string instruction | 13.2M, 8.0M | 0 |
| MP-split16, MP-splitline | misaligned data and flag across 16 bytes, across a 128-byte line | 13.4M, 21.2M | 0 |
| MP-split4K | data across a 4K guest page, flag across a 16K host page | 21.2M | 0 |
| MP-split16K | data across a 16K host page | 24 | 0 |

So x86 ordering holds between threads with emulation on, and the same harness sees it
broken with emulation off: Apple M-series cores reorder loads with loads and stores with
stores. LB is never seen (the cores do not let a store pass an older load), and a load that
crosses a 16K page almost never completes out of order, so `--expect weak` does not
require those two. With emulation on, the misaligned tests take 6 to 9 alignment faults
per run where 4 instructions are patched, so several threads fault on the same
instruction at once and the back-patching under the block's spin lock is exercised
concurrently.

`--tso scalar` switches on only the general setting, as upstream's defaults do (vector
and string-instruction ordering off). Then MP-sse2 (vector flag) and MP-flag-movsb (flag
read by `rep movsb`) still break; the other tests hold because the release store or
acquire load of the scalar access next to them orders them.

The default run (`FEXCoreOrderingTest`, emulation on, 3.2 million tries per test) takes
about 4 seconds; `--tso off` checks the weak side the same way. Both run in the Mac job.
The hosted Mac is a virtual machine on Apple silicon with three processors: the ordering
check with emulation on is meaningful there (one pair at a time), but whether the weak
outcomes appear with emulation off depends on the host, so that run only warns. Only a
physical Mac has shown the table above.

Found by it and fixed:

- **Lost wake-up in the core's reader-writer lock** (`WritePriorityMutex`, used for the
  code invalidation lock). On macOS readers and writers wait on the same word with
  `os_sync_wait_on_address`, which, unlike Linux futex bitsets, cannot wake only writers.
  A writer's hand-off woke one waiter, which could be a reader; the reader went back to
  sleep and the writer never woke. Threads that started translating together hung about
  once in 300 to 1,500 runs of eight threads. A writer hand-off now wakes all waiters.
  `FEXCoreMutexWakeTest` reproduces the hang deterministically (a reader and then a
  writer queue behind a writer); 20,000 eight-thread starts ran clean after the fix.

Not found, but noted: when the core back-patches a misaligned release store, it writes the
barrier into the instruction slot before it and then the plain store. A thread that ran
the old slot just before the patch and fetches the new store just after would make one
store without its barrier, once. The window is a few instructions wide, once per patched
instruction, and was not observed; it is upstream's design.

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
| `FEXCoreOrderingTest` | Multi-thread x86 memory-ordering litmus tests, emulation on (must hold) and off (must break); see above | not yet run | pass, both; off: all required weak outcomes seen |
| `FEXCoreMutexWakeTest` | The core's reader-writer lock never loses a wake-up | not yet run | pass |
| `fexmac_c_test` | A C program using only `fexmac.h`, linked as an outside project would be: 30 subtests covering system calls (prefixed, restarted), stops, state, guest faults, invalidation (also from 8 threads at once), misaligned atomics with each signal-handler arrangement, call/return stack overflow and underflow, guest accesses to protected pages, chained handlers (including one that leaves with siglongjmp) and sent signals, garbage struct sizes, lifetime races and error paths | not yet run | pass, both |
| `fexmac_hook_test` | The same library built with test hooks, 11 subtests: guest and callback accesses to guard pages are not claimed, a log callback cannot re-enter the library, a fault inside the handler crashes with a message instead of hanging, the log callback is never called from a signal handler, a failed create leaves no log hook, invalidation leaves fault signals unblocked | not yet run | pass, both |
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
| Expected failures | 18 (12 crash, 6 fail) |

Since the runner uses `fexmac`, `TwoByte/0F_02` (a test that makes a Linux system call)
fails at once instead of timing out: the old runner's handler left `rip` on the
`syscall`, so the call repeated forever. The totals are otherwise unchanged.

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
