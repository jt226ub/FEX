# macOS port status

This file tracks what the macOS (Apple silicon) port does and does not do yet. Update it
in the same change that closes or opens a gap.

## Where it stands

- **Builds and links on Apple silicon.** The core libraries and the test tools build with
  no errors on macOS 26 with Apple clang. The dylib depends only on `libSystem` and `libc++`.
- **Passes 2,253 of upstream's 2,291 instruction tests** on GitHub's hosted Apple silicon
  Mac, in each of upstream's three translator modes and in a fourth with x86
  memory-ordering emulation switched on. None of the rest is an unexplained failure;
  they are listed under "Tests".
- **Runs eight guest threads at once** with correct, separate results.
- **Nothing has run on a physical Mac.** The hosted Mac is a virtual machine and does not
  enforce the write protection on translated code that real hardware does.
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
   fault-time patching of memory accesses.
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
  `sys_icache_invalidate` on macOS.
- **Fault context.** The core does not read `ucontext_t` itself; the embedder does.

Found out about macOS on Apple silicon:

- **The first 4GB cannot be used.** A 64-bit program that reserves less than 4GB at
  address zero is killed at launch (`lowmem-pagezero` in the Mac job exits with 137).
  Guest memory therefore cannot be placed below 4GB.
- **64GB to 448GB is reserved**, observed as one region on the hosted Mac and believed to
  be the GPU's.
- A further region of about 12GB appears at a different address near 32GB in each process.
  What it is has not been identified.

Still unproven or open:

1. **Write protection on real hardware.** `JITWriteProbe` shows the hosted Mac reports
   `pthread_jit_write_protect_supported_np` as 0 and lets a thread write and run `MAP_JIT`
   memory in any state. The smoke test passes there with and without fixes 1 to 3.
2. **Memory-ordering emulation between threads.** The instruction tests pass with it
   switched on, but they are single-threaded, so they show the code paths work, not that
   ordering is preserved between threads.
3. **The interrupt fault page and guest signals.** Not exercised.
4. **Self-modifying code detected by write-protecting pages.** Only the
   check-on-every-block kind is exercised.
5. **`dc cvau` from user space.** Allowed on the hosted Mac; not confirmed on hardware.
6. **Speed.** Nothing has been measured.

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
at upstream and are being ported by hand, macOS only.

## Tests

All of these run in the Mac job on every push to a `macos/**` branch.

| Test | What it does | Hosted Mac result |
|---|---|---|
| `FEXCoreSmokeTest` | Translates and runs a 23-byte loop, as built and under the hardened runtime | pass, both |
| `FEXCoreThreadTest` | Eight host threads share a context, each summing a different count in a long guest loop | pass |
| `FEXCoreASMTestRunner` with `Scripts/macos_asm_tests.py` | Upstream's 2,291 assembly instruction tests, in four modes | 2,253 pass in each mode |
| `JITWriteProbe` | Reports how the machine treats executable memory | report only |
| Low-memory probe | Shows that a program with a small page-zero reservation is killed | report only |

The instruction tests, per mode, on the most recent run:

| Outcome | Count |
|---|---|
| Pass | 2,253 |
| Skipped by the runner (host lacks a CPU feature the test needs, or it needs Linux system calls) | 9 |
| Skipped by upstream's own lists | 6 |
| Skipped because the test reaches low memory through a 32-bit operand | 5 |
| Expected failures | 18 |

The skip count depends on which machine GitHub assigns: an earlier run on a different
virtual CPU skipped 16 and passed 2,244 with the same code paths.

The fourth mode, `jit_500_tso`, is this port's addition. Upstream runs these tests with
x86 memory-ordering emulation off; this mode turns it on.

`unittests/ASM/Known_Failures_macOS` lists 17 of the expected failures: 16 believed to be
side effects of moving the tests' memory above 4GB (one confirmed in a debugger, the rest
not checked one by one) and one that needs a Linux system call. The remaining one comes
from upstream's own list.

32-bit guest tests (`unittests/32Bit_ASM`) are not run: they need memory below 4GB.

`macos/core-port-smoke` is `macos/core-port` plus the smoke test, kept only so the two can
be compared on a physical Mac.
