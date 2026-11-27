# macOS port status

This file tracks what the macOS (Apple silicon) port does and does not do yet. Update it
in the same change that closes or opens a gap.

Related documents:

- `docs/macOS-v1.0-plan.md`: what "1.0" means for this port and the work left to get there.
- `docs/macOS-embedding-interface.md`: what a program hosting the core must provide.
- `docs/macOS-code-cache-design.md`: how cached translations are loaded on macOS, keyed and
  rejected (implemented).
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
- **Fixed: a lost wake-up in the core's reader-writer lock.** On macOS a writer hand-off
  woke one arbitrary waiter on the shared futex word; if that was a reader, the writer
  slept forever. Eight threads linking many blocks hung about one run in 30 in
  `ExitFunctionLink`. Writer hand-offs now wake all waiters
  (`WritePriorityMutex.h`). Seen with `FEXCoreCacheTest`, cache on or off.
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

Physical Mac (M4), all four modes, 2,292 tests, cold pass then warm pass in one
directory with six processes at a time: the warm pass compiled **no block** in any test
that ran to completion (the 13 crashing or timing-out expected failures never save),
loaded 181,032 / 4,722 / 2,615 / 2,615 blocks (jit_1, jit_500, jit_500_m, jit_500_tso),
and every result was identical to the cold pass; no entry was rejected. 59 MB on disk.
Also a jit_1 two-pass over `3DNow`, `TwoByte`, `SelfModifyingCode` and `FEX_bugs`
(regression run for the 3DNow! fix).

Speed: little gained on these tiny tests. Cold pass 110 s, warm pass 80 s (as built,
other work on the machine); the whole script with the cache off about 125 s including
assembling. Per process, start-up dominates. In jit_1, whose database has 85,000
entries (10.7MB index), a warm run cost 8.5 ms against 3.4 ms uncached before the
index was made lazy (reading it twice and parsing every entry took about 9 ms), and
5.8 ms against 2.9 ms after. The largest test (`FEX_bugs/XeSS_quadratic`, 65,538
blocks in jit_1) takes 72 ms compiling and 79 ms loading. A benefit has to be shown with
real programs (package G).

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

Still unproven or open:

1. **Memory-ordering emulation between threads.** The instruction tests pass with it
   switched on, but they are single-threaded, so they show the code paths work, not that
   ordering is preserved between threads.
2. **The interrupt fault page and guest signals.** Not exercised.
3. **Self-modifying code detected by write-protecting pages.** Only the
   check-on-every-block kind is exercised.
4. **Speed.** Nothing has been measured.

## Not implemented, by decision or for lack of a macOS method

- **Upstream's whole-section code cache (`FXCC`, `EnableCodeCachingWIP`).** It publishes
  code with `mremap`, which macOS lacks, so its macOS path still stops with an error. It
  is not needed: the per-block disk cache below does the job by copying.
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
| `Scripts/macos_asm_tests.py --cache-dir DIR --cache-passes 2` | The instruction tests cold then warm; fails if a warm result differs, a finished warm run compiled anything, or an entry was rejected | added in package F, not yet run there (two modes in the job) | pass, all four modes, both |
| `FEXCoreASMTestRunner` with `Scripts/macos_asm_tests.py` | Upstream's 2,292 assembly instruction tests, in four modes | 2,253 pass in each mode (before the upstream sync added one test) | 2,255 pass in each mode, both |
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
