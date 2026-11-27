#!/bin/bash
# SPDX-License-Identifier: MIT
# Syntax-checks the non-Apple side of the core files the macOS port changes, on a Mac.
# Each source and the port's headers it includes are copied with their __APPLE__ blocks
# removed (Scripts/macos_strip_apple.py), the headers into an overlay include directory
# that comes first, and the copies are compiled with -fsyntax-only using the build's own
# compile commands. It catches code that uses something declared only for macOS (the
# Linux build broke that way once: MemCpySrcBase's handler). It is not a Linux build:
# system headers are still macOS's; the fork's Linux workflow is the real check.
#
#   Scripts/macos_check_nonapple.sh [build-dir]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "${1:-$ROOT/build}" && pwd)"
strip_apple() { python3 "$ROOT/Scripts/macos_strip_apple.py" "$@"; }
OV="$(mktemp -d)"
TMPS=()
cleanup() { rm -rf "$OV"; rm -f "${TMPS[@]}"; }
trap cleanup EXIT INT TERM
mkdir -p "$OV/FEXCore/Core" "$OV/Interface/Core/JIT" "$OV/Interface/Core/X86Tables"
strip_apple "$ROOT/FEXCore/include/FEXCore/Core/CoreState.h" "$OV/FEXCore/Core/CoreState.h"
strip_apple "$ROOT/FEXCore/Source/Interface/Core/JIT/JITClass.h" "$OV/Interface/Core/JIT/JITClass.h"
strip_apple "$ROOT/FEXCore/Source/Interface/Core/OpcodeDispatcher.h" "$OV/Interface/Core/OpcodeDispatcher.h"
strip_apple "$ROOT/FEXCore/Source/Interface/Core/X86Tables/X86Tables.h" "$OV/Interface/Core/X86Tables/X86Tables.h"
R="$ROOT/FEXCore/Source/Interface/Core"
bad=0
cd "$BUILD"
for f in JIT/MemoryOps.cpp JIT/BranchOps.cpp JIT/MiscOps.cpp JIT/JIT.cpp OpcodeDispatcher.cpp OpcodeDispatcher/AVX_128.cpp Core.cpp; do
  src="$R/$f"; tmp="${src%.cpp}.nonapple_check.cpp"; TMPS+=("$tmp")
  strip_apple "$src" "$tmp"
  # Quoted includes resolve next to the source first: make the overlaid ones angle includes.
  sed -i '' -e 's|#include "Interface/Core/JIT/JITClass.h"|#include <Interface/Core/JIT/JITClass.h>|' \
    -e 's|#include "Interface/Core/OpcodeDispatcher.h"|#include <Interface/Core/OpcodeDispatcher.h>|' \
    -e 's|#include "Interface/Core/X86Tables/X86Tables.h"|#include <Interface/Core/X86Tables/X86Tables.h>|' "$tmp"
  C=$(ninja -t commands "FEXCore/Source/CMakeFiles/FEXCore_object.dir/Interface/Core/$f.o" | tail -1)
  C=$(echo "$C" | sed 's/ -MD -MT [^ ]* -MF [^ ]*//; s/ -o [^ ]*\.o / -o \/dev\/null /')
  C="${C//$src/$tmp}"
  C="${C/ -I/ -I$OV -I}"
  if out=$(eval "$C -fsyntax-only" 2>&1); then
    echo "ok   $f"
  else
    echo "FAIL $f"; echo "$out" | grep -E "error:" | head -10; bad=1
  fi
  rm -f "$tmp"
done
exit $bad
