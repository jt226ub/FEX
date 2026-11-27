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
