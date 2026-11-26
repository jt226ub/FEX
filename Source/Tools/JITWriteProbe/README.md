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
