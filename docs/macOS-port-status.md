# macOS port status

This file tracks what the macOS (Apple silicon) port does and does not do yet. Update it
in the same change that closes or opens a gap.

## Where it stands

- **Builds and links on Apple silicon.** `libFEXCore.a`, `libFEXCore.dylib`, `libCommon.a`
  and `libCommonTools.a` build with no errors or warnings on macOS 26 with Apple clang.
  The dylib is arm64 and depends only on `libSystem` and `libc++`.
- **Runs a first x86-64 program.** A smoke test translates and runs a short loop on the
  hosted Apple silicon runner (macOS 26, virtual M1) and gets the right answer. Nothing
  larger has run, and nothing has run on a physical Mac.
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
4. **Code-buffer guard page.** Skipped on macOS, because `mprotect` cannot change a page
   inside a `MAP_JIT` mapping. Without this the core logs
   `Failed to mprotect last page of code buffer`.
5. **Host feature detection.** The Linux kernel version check is kept out of the Mac build.

Checked and found not to need a change:

- **Instruction cache flush.** Already uses `__builtin___clear_cache`, which calls
  `sys_icache_invalidate` on macOS.
- **Fault context.** The core does not read `ucontext_t` itself. Whatever embeds the core
  supplies that, so the macOS layout is the embedder's job.

Still unproven:

1. **The fixes above on real hardware.** The smoke test passes on the hosted Apple silicon
   runner with and without fixes 1 to 3, in both an ordinary and a hardened-runtime build.
   So that runner does not show that they are needed. `JITWriteProbe` confirms why: the
   runner is a virtual machine, reports `pthread_jit_write_protect_supported_np` as 0, and
   lets a thread write and run `MAP_JIT` memory in any state. Both branches and the probe
   need running on a physical Mac.
2. **`x18` under load.** Corruption would only show after a context switch, which the
   short smoke test does not provoke.
3. **Faulting paths.** Nothing that takes a fault has run: memory-access patching,
   self-modifying code, the interrupt fault page. These need a signal handler in the
   embedder.
4. **`dc cvau` from user space.** Allowed on the hosted runner; not yet confirmed on a
   physical Mac.
5. **`VirtualDontNeed`.** `MADV_DONTNEED` does not guarantee zeroed pages on macOS as it
   does on Linux.
6. **Region reservation in `Allocator.cpp`.** Uses `MAP_FIXED_NOREPLACE`, which macOS does
   not have; the fallback value defined for it is a Linux number. Not reached by the
   smoke test.
7. **More than one thread.** Not tested.

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

`FEXCoreSmokeTest` (`Source/Tools/CoreSmokeTest`) translates and runs a 23-byte x86-64
program: a loop that sums 1 to 100, then a system call carrying the sum. It passes when the
handler sees system call 60, argument 5050, and the address after `syscall` in `rcx`.
Multiblock is off so the loop branch goes through the block linker.

The Mac workflow builds it and runs it twice: once as built, and once signed for the
hardened runtime with only `com.apple.security.cs.allow-jit`
(`Data/macOS/jit.entitlements`).

`macos/core-port-smoke` is `macos/core-port` plus the test, kept only so the two can be
compared on a physical Mac.

The two test programs that proved the earlier port live in the Bachata-S4 project and have
not been brought over.
