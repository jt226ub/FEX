#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turns upstream's instruction-test results on Linux into a table and compares them with the Mac's.

    # In the Linux workflow: ctest's JUnit file to a table, with counts per mode.
    python3 Scripts/macos_linux_asm_compare.py --junit linux-asm.xml --out linux-asm-results.tsv
    # Afterwards, against a Mac run of Scripts/macos_asm_tests.py:
    python3 Scripts/macos_linux_asm_compare.py --linux linux-asm-results.tsv --mac asm-out/results.tsv
    # The port's Linux results against the upstream commit it is based on, same machine kind:
    python3 Scripts/macos_linux_asm_compare.py --linux port.tsv --base upstream-base.tsv

On Linux the tests run through upstream's machinery (TestHarnessRunner, CTest, the ASM
`Known_Failures` lists), where a known failure that fails counts as a CTest pass. This
script undoes that inversion, so each row gives the raw outcome: pass, fail, or skip
(disabled by upstream's lists). The Mac's results.tsv already gives raw outcomes
(pass, expected-<kind>, unexpected-<kind>, skip...). Tests are matched by name
(`Test_<path>.asm`) and mode (jit_1, jit_500, jit_500_m: the modes both platforms run;
the Mac's jit_500_tso has no Linux counterpart in upstream's CTest list).

The comparison prints every test that passes on one platform and not on the other,
except those the Mac skips or lists in Known_Failures_macOS (which the Mac's own run
already accounts for). With --base, every test whose raw outcome differs between the
port and its upstream base on Linux is printed: a test that passes upstream and fails on
the port is a Linux regression caused by the port. Exit status 1 if any difference remains.
"""

import argparse
import collections
import os
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMMON_MODES = ("jit_1", "jit_500", "jit_500_m")


def read_list(path):
    if not os.path.exists(path):
        return set()
    with open(path) as handle:
        return {line.strip() for line in handle if line.strip() and not line.startswith("#")}


def from_junit(path, root):
    known = read_list(os.path.join(root, "unittests/ASM/Known_Failures")) | read_list(
        os.path.join(root, "unittests/ASM/Known_Failures_jit"))
    rows = []
    for case in ET.parse(path).getroot().iter("testcase"):
        full = case.get("name")
        if "/" not in full:
            continue
        mode, rest = full.split("/", 1)
        if not rest.startswith("Test_64Bit_"):
            continue
        name = "Test_" + rest[len("Test_64Bit_"):]
        status = case.get("status", "")
        if case.find("skipped") is not None or status in ("notrun", "disabled"):
            raw, ctest = "skip", "skipped"
        elif case.find("failure") is not None or status == "fail":
            ctest = "failed"
            raw = "pass" if (full in known or name in known) else "fail"
        else:
            ctest = "passed"
            raw = "fail" if (full in known or name in known) else "pass"
        rows.append((mode, name, raw, ctest))
    return sorted(rows)


def counts(rows):
    by_mode = collections.defaultdict(collections.Counter)
    for mode, _, raw, _ in rows:
        by_mode[mode][raw] += 1
    return by_mode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--junit")
    ap.add_argument("--out")
    ap.add_argument("--linux")
    ap.add_argument("--mac")
    ap.add_argument("--base", help="the same table from the upstream base commit")
    ap.add_argument("--root", default=ROOT, help="source tree whose Known_Failures lists apply (default: this one)")
    args = ap.parse_args()

    if args.junit:
        rows = from_junit(args.junit, args.root)
        if args.out:
            with open(args.out, "w") as handle:
                handle.write("".join("%s\t%s\t%s\t%s\n" % row for row in rows))
    elif args.linux:
        with open(args.linux) as handle:
            rows = [tuple(line.rstrip("\n").split("\t")) for line in handle if line.strip()]
    else:
        ap.error("give --junit or --linux")

    for mode, c in sorted(counts(rows).items()):
        ctest_failed = sum(1 for r in rows if r[0] == mode and r[3] == "failed")
        print("linux %s: raw pass %d, fail %d, skip %d (CTest failures, i.e. not as upstream expects: %d)" %
              (mode, c["pass"], c["fail"], c["skip"], ctest_failed))
    if args.base:
        with open(args.base) as handle:
            base = {tuple(parts[:2]): parts[2] for parts in (line.rstrip("\n").split("\t") for line in handle) if len(parts) >= 3}
        port = {(r[0], r[1]): r[2] for r in rows}
        diffs = 0
        for key in sorted(set(base) | set(port)):
            b, p = base.get(key, "absent"), port.get(key, "absent")
            if b != p:
                diffs += 1
                print("   %s %s: upstream base %s, port %s%s" % (key[0], key[1], b, p,
                      "  <-- REGRESSION" if b == "pass" and p == "fail" else ""))
        print("\nport against upstream base: %d tests differ" % diffs)
        return 1 if diffs else 0
    if not args.mac:
        return 0

    mac = {}
    with open(args.mac) as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                mac[(parts[0], parts[1])] = parts[2]
    mac_known = read_list(os.path.join(ROOT, "unittests/ASM/Known_Failures_macOS"))
    linux = {(r[0], r[1]): r[2] for r in rows}
    differences = 0
    for mode in COMMON_MODES:
        only_linux, only_mac, mac_listed, missing = [], [], [], []
        for (m, name), raw in sorted(linux.items()):
            if m != mode or raw == "skip":
                continue
            status = mac.get((mode, name))
            if status is None:
                missing.append(name)
                continue
            if status.startswith("skip"):
                continue
            mac_pass = status == "pass"
            if raw == "pass" and not mac_pass:
                (mac_listed if name in mac_known else only_linux).append((name, status))
            elif raw == "fail" and mac_pass:
                only_mac.append((name, status))
        print("\n== %s: pass on Linux only %d (plus %d in Known_Failures_macOS), pass on the Mac only %d, "
              "not run on the Mac %d" % (mode, len(only_linux), len(mac_listed), len(only_mac), len(missing)))
        for name, status in only_linux:
            print("   LINUX ONLY %s (Mac: %s)" % (name, status))
        for name, status in mac_listed:
            print("   MAC LISTED %s (Mac: %s)" % (name, status))
        for name, status in only_mac:
            print("   MAC ONLY %s" % name)
        for name in missing[:20]:
            print("   NOT ON MAC %s" % name)
        differences += len(only_linux) + len(only_mac)
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
