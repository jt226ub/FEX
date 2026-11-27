#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Checks a macOS build of the core for dependencies the hardened runtime would refuse.
#
#     Scripts/macos_check_linkage.sh build
#
# The hardened runtime refuses libraries signed by anyone else, so the core and the test
# tools may depend on libSystem and libc++ only. This also checks that the bundled fmt
# was compiled into the build. Exits non-zero on any problem, including a missing file.
set -u

build="${1:?usage: $0 <build directory>}"
bad=0

# The files to check. Every one must exist.
dylibs=()
while IFS= read -r f; do
  dylibs+=("$f")
done < <(find "$build" \( -name 'libFEXCore*.dylib' -o -name 'libfexmac.dylib' \) -type f)
if [ "${#dylibs[@]}" -eq 0 ]; then
  echo "no libFEXCore dylib found under $build"
  bad=1
fi
files=(${dylibs[@]+"${dylibs[@]}"} "$build/Bin/FEXCoreSmokeTest" "$build/Bin/FEXCoreThreadTest" "$build/Bin/FEXCoreASMTestRunner" "$build/Bin/FEXCoreOrderingTest" "$build/Bin/FEXCoreMutexWakeTest" "$build/Bin/FEXCoreBench" "$build/Bin/fexmac_c_test" "$build/Bin/fexmac_hook_test")

allowed='^[[:space:]]*(/usr/lib/libSystem\.B\.dylib|/usr/lib/libc\+\+\.1\.dylib|@rpath/libFEXCore\.dylib|@rpath/libfexmac\.dylib) '
for f in "${files[@]}"; do
  [ -n "$f" ] || continue
  echo "--- $f"
  if [ ! -f "$f" ]; then
    echo "missing: $f"
    bad=1
    continue
  fi
  if ! deps="$(otool -L "$f")"; then
    echo "otool failed on $f"
    bad=1
    continue
  fi
  deps="$(printf '%s\n' "$deps" | tail -n +2)"
  printf '%s\n' "$deps"
  if [ -z "$deps" ]; then
    echo "otool listed no dependencies for $f"
    bad=1
  elif printf '%s\n' "$deps" | grep -v -E "$allowed"; then
    echo "unexpected dependency in $f (above)"
    bad=1
  fi
done

# otool only shows that no other dylib is linked. A system fmt could still have been used
# as a static library, so also check that the bundled one was compiled.
if grep -q 'External/fmt/src/format.cc' "$build/build.ninja" 2>/dev/null; then
  echo "--- bundled fmt (External/fmt) is compiled in this build"
else
  echo "bundled fmt (External/fmt) is not part of this build"
  bad=1
fi

exit "$bad"
