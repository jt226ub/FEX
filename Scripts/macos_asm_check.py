#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks the summary that Scripts/macos_asm_tests.py printed, so a run cannot pass with tests missing.

    python3 Scripts/macos_asm_check.py <log> --modes N [--full | --full-virtual] [--floor FILE]

Fails (exit 1) when:
  - "Tests found" is below tests_found in the floor file (Scripts/macos_asm_floor.txt),
    or any test "could not be prepared";
  - the number of per-mode summaries is not N;
  - a mode ran no test at all (a --filter that matches nothing);
  - a mode has an unexpected failure or an expected failure that now passes (the script
    itself exits 0 for the second);
  - with --full (physical Mac) or --full-virtual (hosted Mac), a mode's pass count is
    below the floor's pass or pass_virtual.
Prints one line per check.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--modes", type=int, required=True)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--full", action="store_true")
    g.add_argument("--full-virtual", action="store_true")
    ap.add_argument("--floor", default=os.path.join(ROOT, "Scripts", "macos_asm_floor.txt"))
    args = ap.parse_args()

    floor = {}
    with open(args.floor) as handle:
        for line in handle:
            parts = line.split()
            if len(parts) == 2 and not line.startswith("#"):
                floor[parts[0]] = int(parts[1])
    with open(args.log, errors="replace") as handle:
        text = handle.read()

    problems = []
    found = re.search(r"^Tests found: (\d+)\s+disabled by upstream lists: (\d+)\s+could not be prepared: (\d+)", text, re.M)
    if not found:
        problems.append("no 'Tests found' line: the script did not get to its summary")
    else:
        if int(found.group(1)) < floor["tests_found"]:
            problems.append("tests found %s, floor %d" % (found.group(1), floor["tests_found"]))
        if int(found.group(3)) != 0:
            problems.append("%s tests could not be prepared" % found.group(3))
    modes = re.findall(r"^== (jit_\w+): (.*)$", text, re.M)
    if len(modes) != args.modes:
        problems.append("%d mode summaries, %d expected" % (len(modes), args.modes))
    want = floor["pass"] if args.full else floor["pass_virtual"] if args.full_virtual else None
    for mode, counts in modes:
        c = {k: int(v) for k, v in re.findall(r"([a-z\-]+) (\d+)", counts)}
        if sum(c.values()) == 0:
            problems.append("%s: no test ran" % mode)
        if want is not None and c.get("pass", 0) < want:
            problems.append("%s: pass %d, floor %d" % (mode, c.get("pass", 0), want))
        print("%s: %s" % (mode, counts.strip()))
    clean = re.findall(r"^   unexpected failures: (\d+)   expected failures that now pass: (\d+)$", text, re.M)
    if len(clean) != args.modes:
        problems.append("%d 'unexpected failures' lines, %d expected" % (len(clean), args.modes))
    for unexpected, fixed in clean:
        if unexpected != "0" or fixed != "0":
            problems.append("unexpected failures %s, expected failures that now pass %s" % (unexpected, fixed))
    for p in problems:
        print("CHECK FAILED: " + p)
    if not problems:
        print("check passed: %d modes%s" % (args.modes, ", pass floor %d" % want if want is not None else ""))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
