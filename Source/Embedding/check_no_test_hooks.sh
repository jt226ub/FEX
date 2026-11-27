#!/bin/sh
# SPDX-License-Identifier: MIT
# Fails if a production form of the fexmac library exports a test-only hook.
if nm -gU "$1" 2>/dev/null | grep -q '_fexmac_testing_'; then
  echo "error: $1 contains fexmac test hooks" >&2
  exit 1
fi
exit 0
