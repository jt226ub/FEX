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
  multi-thread tests pass, and 2,264 of upstream's 2,292 instruction tests pass in each
  of upstream's three translator modes and in a fourth with x86 memory-ordering emulation
  switched on, with no unexpected failures. The rest are listed under "Tests".
- **Write protection is enforced on real hardware and respected.** `JITWriteProbe` shows
  it; the branch without the runtime fixes dies at its first write to code memory.
  GitHub's hosted Mac is a virtual machine that does not enforce it.
- **Runs eight guest threads at once** with correct, separate results.
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

1. **Memory-ordering emulation between threads.** The instruction tests pass with it
   switched on, but they are single-threaded, so they show the code paths work, not that
   ordering is preserved between threads.
2. **The interrupt fault page and guest signals.** Not exercised.
3. **Self-modifying code detected by write-protecting pages.** Only the
   check-on-every-block kind is exercised.
4. **Speed.** Nothing has been measured.

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
| `FEXCoreASMTestRunner` with `Scripts/macos_asm_tests.py` | Upstream's 2,292 assembly instruction tests, in four modes | 2,253 pass in each mode (before the upstream sync added one test) | 2,264 pass in each mode, both |
| `JITWriteProbe` | Reports how the machine treats executable memory, including writes and switches from a fault handler and instruction-cache flushing | report only | see the facts above |
| `JITWriteProbe --expect-enforced` | The same, but exits 1 unless every asserted case gives the result the port relies on with write protection enforced, or if a fault-handler case's handler never ran | not run: the hosted Mac does not enforce, so it would fail | pass, as built and hardened |
| `Scripts/macos_check_linkage.sh` | Fails if the core or a test tool links anything but `libSystem` and `libc++`, if one of them is missing, or if the bundled `fmt` was not compiled. Homebrew's `fmt` is installed first. `otool` shows only that no fmt dylib is linked; the `build.ninja` check covers a static one | not yet run (added after the last hosted run) | pass, both toolchains |
| Low-memory probe | Shows that a program with a small page-zero reservation is killed | report only | same as hosted |

The instruction tests, per mode, on the most recent physical-Mac run (as built and
hardened, the same in all four modes). `asm-out/results.tsv` lists every test in every
mode, including those not run, with the reason; the script also prints each skip with its
reason.

| Outcome | Count |
|---|---|
| Pass | 2,264 |
| Skipped by the runner | 6 |
| Skipped by upstream's own lists | 6 |
| Skipped because the test reaches low memory through a 32-bit operand | 4 |
| Expected failures (all crashes) | 12 |

Before package B the counts were pass 2,255, runner skips 8, low-memory skips 5 and
18 expected failures (crash 12, fail 5, timeout 1). Nine tests now pass:
`FEX_bugs/cmpxchg8b`, `FEX_bugs/cmpxchg16b`, `TwoByte/0F_8B_16`, `FEX_bugs/o16_leave`,
`FEX_bugs/SIBScaleTranspose` (the move of the tests' memory had broken their expected
values, now moved too), `TwoByte/0F_02` (the runner's system call handler did not step
past `syscall`), `Secondary/09_XX_06` and `09_XX_07` (RDRAND and RDSEED, through FEX's
software generator) and `Primary/Primary_CF` (a `mov esp, <address>` is now written as
`mov rsp`). None of the remaining failures is a translator or port fault.

**Every test that does not pass, and why** (checked by running each one on the M4; the
runner prints the faulting address of a crash, and results.tsv carries it):

- *Expected failures, 12, all crashes, in every mode.* Eleven take an address cut to
  32 bits, which lands below 4GB where macOS maps nothing:
  `FEX_bugs/a32_lods` and `a32_rep_lods` (fault at 0x80000), `Primary/Primary_A6_addrmod`,
  `Primary_A6_REP_addrmod` (0x10000200) and `Primary_AE_addrmod` (0x10000100) use the
  0x67 address-size prefix; `VEX/vmovq_vmovd_reg` reads `[edx]` (0xe0000000; the earlier
  description, a 32-bit vector move cutting the address, was wrong);
  `PrimaryGroup/5_FF_05` and the four `5_FF_05_03_*` far-call tests jump through an m16:32
  pointer built with `lea edi, [rel .success]` (0x10028, the code's original address;
  the o32 forms fault first at 0xe0000ff0 through `[esp]`). They are listed in
  `unittests/ASM/Known_Failures_macOS`. The twelfth, previously unidentified, is
  `FEX_bugs/32bit_syscall`, on upstream's `Known_Failures_jit` and failing the same way
  on Linux: FEX refuses `int 0x80` in a 64-bit process, the block cannot be compiled,
  `CompileBlock` returns 0 and `ExitFunctionLink` faults at address 0 trying to link it.
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
  own features are not the reason. The runner now names the missing feature.
- *Upstream's lists, 6.* `Primary/Primary_E8`, `Primary_E9` (cannot be tested in a real
  environment), `X87/D9_F2`, `D9_F9` (rely on undefined behaviour), `X87/D9_F8` (rounding)
  and `Secondary/06_XX_00_2` (UMIP value FEX does not emulate). Upstream's entry
  `FEX_bugs/XeSS_quadratic.asm` lacks the `Test_` prefix, so it disables nothing, here or
  upstream; the test runs and passes.

**Host features.** The runner's feature check reads the host's `hw.optional.arm.FEAT_*`
sysctls and the CPUID the translator gives the guest. On the M4 it is right: no SVE (so
AVX is translated as two 128-bit halves; the AVX tests run), FEAT_AFP, BF16, SHA3 and
SHA512 present, and no FEAT_RNG (the M4 has no such sysctl and `mrs RNDR` raises SIGILL),
so RDRAND and RDSEED are offered to the guest only with `FEX_SOFTWARERNG=1`, which the test
script sets on macOS. x87 80-bit arithmetic is emulated in software and needs no host
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
operands swapped: it faults at 0x800a0000000).

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
