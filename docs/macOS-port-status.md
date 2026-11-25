# macOS port status

This file tracks what the macOS (Apple silicon) port does and does not do yet. Update it
in the same change that closes or opens a gap.

## Where it stands

- **Builds and links on Apple silicon.** `libFEXCore.a`, `libFEXCore.dylib`, `libCommon.a`
  and `libCommonTools.a` build with no errors or warnings on macOS 26 with Apple clang.
  The dylib is arm64 and depends only on `libSystem` and `libc++`.
- **Has never been run.** No guest code has been translated or executed on macOS by this
  port. Building is the only thing verified so far.
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

## Known gaps before it can run

These are expected to fail at run time. They are listed in the order they will be hit.

1. **Writing translated code.** Mappings are made with `MAP_JIT`, but nothing yet calls
   `pthread_jit_write_protect_np` around code emission, block linking or self-modifying
   code invalidation. The first write to code memory will fault.
2. **Reserved register.** macOS reserves `x18`. The register allocation has not been
   checked or changed to avoid it.
3. **Page size.** Host pages are 16K on Apple silicon. Call sites that assume the host
   page size equals the 4K guest page size have not been audited.
4. **Fault context.** macOS lays out `ucontext_t` and `mcontext_t` differently from Linux.
   The signal-handling paths compile but have not been reviewed.
5. **Instruction cache flush.** Not checked against `sys_icache_invalidate`.
6. **`VirtualDontNeed`.** `MADV_DONTNEED` does not guarantee zeroed pages on macOS as it
   does on Linux.
7. **Region reservation in `Allocator.cpp`.** Uses `MAP_FIXED_NOREPLACE`, which macOS does
   not have; the fallback value defined for it is a Linux number.

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

None run on macOS yet. The two test programs that proved the earlier port live in the
Bachata-S4 project and have not been brought over.
