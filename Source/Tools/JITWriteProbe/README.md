# JITWriteProbe

Prints how the machine it runs on treats executable memory. Standalone, macOS only, not
linked against FEX.

```
clang -O1 -o JITWriteProbe Source/Tools/JITWriteProbe/Main.c
./JITWriteProbe
codesign --force --sign - --options runtime --entitlements Data/macOS/jit.entitlements JITWriteProbe
./JITWriteProbe
```

Run it both ways: as built, and signed for the hardened runtime.

With `--expect-enforced` it also exits 1 unless every asserted case gives the result the
port relies on when write protection is enforced, as on a physical Apple silicon Mac, or
if a fault-handler case's handler never ran. GitHub's hosted Mac does not enforce write
protection, so run that mode only on real hardware:

```
./JITWriteProbe --expect-enforced
```

The "Guard:" cases show how a guard page can be placed after `MAP_JIT` memory. With
`--expect-enforced` they assert what FEXCore's code buffer relies on: `MAP_JIT` with
`MAP_FIXED` is refused, `mprotect` and `mach_vm_protect` inside a `MAP_JIT` mapping are
refused, and a `MAP_FIXED` `PROT_NONE` mapping over the tail of the program's own
`MAP_JIT` mapping is allowed, leaves the front runnable and write-protected, faults a
write past it even while the thread is switched to writable, and is freed with the rest
by one `munmap`. Whether a merely hinted mapping lands right after `MAP_JIT` memory is
reported only.
