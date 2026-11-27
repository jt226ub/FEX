#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the benchmark in unittests/macOS/Bench three ways, runs it and prints a table.

    python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-out
    python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-out --quick [--hardened]

The three builds come from the same C source with the same compiler flags, apart from the
target and the platform file (see unittests/macOS/Bench/bench.h):

  guest    a freestanding x86-64 image (static Mach-O at a fixed address, no libc), run
           by FEXCoreBench through the fexmac interface in each translator configuration;
  rosetta  an x86-64 macOS program, run under Rosetta 2 (skipped if Rosetta is missing);
  native   an arm64 macOS program: the ceiling.

Every run checks its own checksums against the values recorded in kernels.c, and this
script also compares the checksums between builds. Checksums are bit-exact across all
three builds except the x87 kernel, which is compared between the two x86-64 builds only
(long double is 64 bits on arm64). Known, explained differences are allowed per
configuration: with x86 memory-ordering emulation off, the message-passing kernel may see
data out of order (that is what it tests); with reduced x87 precision, the x87 kernel
differs. Exit status: 0 if every checksum is right.

--quick runs small sizes once (the correctness check the Mac workflow runs; no timing is
asserted). Without it: one warm-up and --reps timed repetitions per kernel; the table
shows the median (and minimum) per kernel, the warm-up time, and the time from process
start to the first result line.
"""

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "unittests", "macOS", "Bench")

# Identical for every build. -ffreestanding (which implies -fno-builtin) is used for the
# kernels in all three builds so the compiler makes the same choices in each.
CFLAGS = ["-O2", "-std=c11", "-ffreestanding", "-ffp-contract=off", "-fno-stack-protector", "-fno-stack-check", "-Wall",
          "-Wextra", "-Werror"]
X86_TARGET = ["-target", "x86_64-apple-macos11"]
ARM_TARGET = ["-target", "arm64-apple-macos11"]
# Above 4GB (macOS allows nothing below) and clear of the ranges macOS reserves.
GUEST_BASE = "0x20000000000"

TSO_ON = {"TSOENABLED": "1", "VECTORTSOENABLED": "1", "MEMCPYSETTSOENABLED": "1"}
TSO_OFF = {"TSOENABLED": "0", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "0"}
BASE = {"MAXINST": "500", "MULTIBLOCK": "1"}

# name: (options, kernels whose checksum may differ, why)
CONFIGS = {
    "tso_off": ({**BASE, **TSO_OFF}, {"threads_mp"}, "no ordering emulation: message passing may see stale data"),
    "tso_on": ({**BASE, **TSO_ON}, set(), ""),
    "tso_scalar": ({**BASE, **TSO_ON, "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "0"}, set(),
                   "TSO for scalar accesses only (FEX's defaults for the two other switches)"),
    "tso_on_nomb": ({**BASE, **TSO_ON, "MULTIBLOCK": "0"}, set(), ""),
    "tso_on_x87rp": ({**BASE, **TSO_ON, "X87REDUCEDPRECISION": "1"}, {"x87"}, "x87 computed in 64-bit precision"),
}
DEFAULT_CONFIGS = ["tso_off", "tso_on", "tso_scalar", "tso_on_nomb", "tso_on_x87rp"]
QUICK_CONFIGS = ["tso_off", "tso_on", "tso_on_nomb"]
X86_ONLY = {"x87"}


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def build(out):
    os.makedirs(out, exist_ok=True)
    k = os.path.join(SRC, "kernels.c")
    obj_k = os.path.join(out, "kernels-x86_64.o")
    obj_g = os.path.join(out, "platform_guest.o")
    guest = os.path.join(out, "bench-guest.macho")
    run(["clang", *X86_TARGET, *CFLAGS, "-c", k, "-o", obj_k])
    run(["clang", *X86_TARGET, *CFLAGS, "-c", os.path.join(SRC, "platform_guest.c"), "-o", obj_g])
    run(["clang", *X86_TARGET, "-nostdlib", "-static", "-e", "_bench_entry", "-Wl,-image_base," + GUEST_BASE, "-Wl,-no_pie",
         obj_k, obj_g, "-o", guest])
    natives = {}
    for arch, target in (("x86_64", X86_TARGET), ("arm64", ARM_TARGET)):
        obj = os.path.join(out, f"kernels-{arch}-native.o")
        exe = os.path.join(out, f"bench-{arch}")
        run(["clang", *target, *CFLAGS, "-c", k, "-o", obj])
        # The platform file is ordinary hosted C (libc, pthreads).
        run(["clang", *target, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", obj, os.path.join(SRC, "platform_native.c"),
             "-o", exe])
        # Rosetta 2 on macOS 27 hangs, unkillably, at the launch of an unsigned x86-64
        # program (seen on the M4); an ad hoc signature avoids it. arm64 programs are
        # already signed by the linker.
        run(["codesign", "--force", "--sign", "-", exe])
        natives[arch] = exe
    return guest, natives


def rosetta_available():
    try:
        return subprocess.run(["arch", "-x86_64", "/usr/bin/true"], timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def execute(cmd, timeout):
    """Runs one benchmark process; returns (exit code, lines, {"start": s, "first": s}): the
    seconds from launch to the program's first line (START, written before any kernel
    runs) and to its first result line (the end of the first kernel's warm-up)."""
    start = time.monotonic()
    first = None
    started = None
    lines = []
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
    try:
        for line in proc.stdout:
            if started is None and line.startswith("START"):
                started = time.monotonic() - start
            if first is None and line.startswith("K "):
                first = time.monotonic() - start
            lines.append(line.rstrip("\n"))
            if time.monotonic() - start > timeout:
                proc.kill()
                lines.append("TIMEOUT")
                break
        proc.wait(timeout=max(1, timeout - (time.monotonic() - start)))
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        lines.append("TIMEOUT")
    return proc.returncode, lines, {"start": started, "first": first}


def parse(lines):
    times, sums, verdicts = {}, {}, {}
    for line in lines:
        f = line.split()
        if f and f[0] == "K":
            times.setdefault(f[1], []).append(int(f[3]))
            sums.setdefault(f[1], f[4])
        elif f and f[0] == "C":
            verdicts[f[1]] = f[2]
            sums[f[1]] = f[3]
    return times, sums, verdicts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--loader", required=True, help="FEXCoreBench")
    ap.add_argument("--out", default="bench-out")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--only", default="", help="comma-separated kernel names")
    ap.add_argument("--configs", default="", help="comma-separated: " + ",".join(CONFIGS))
    ap.add_argument("--hardened", action="store_true", help="run a copy of the loader signed for the hardened runtime")
    ap.add_argument("--no-rosetta", action="store_true")
    ap.add_argument("--timeout", type=int, default=0, help="seconds per run (default 300, quick 120)")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    guest, natives = build(out)
    loader = os.path.abspath(args.loader)
    if args.hardened:
        hardened = os.path.join(out, "FEXCoreBench-hardened")
        shutil.copy2(loader, hardened)
        run(["codesign", "--force", "--sign", "-", "--options", "runtime", "--entitlements",
             os.path.join(ROOT, "Data", "macOS", "jit.entitlements"), hardened])
        loader = hardened

    reps = 1 if args.quick else args.reps
    common = (["--quick"] if args.quick else []) + ["--reps", str(reps)] + (["--only", args.only] if args.only else [])
    timeout = args.timeout or (120 if args.quick else 300)
    configs = [c for c in (args.configs.split(",") if args.configs else (QUICK_CONFIGS if args.quick else DEFAULT_CONFIGS)) if c]
    for c in configs:
        if c not in CONFIGS:
            sys.exit(f"unknown configuration {c}")

    runs = []  # (label, kind, allowed differences, why)
    runs.append(("native-arm64", [natives["arm64"], *common], "native", set(), ""))
    if not args.no_rosetta and rosetta_available():
        runs.append(("rosetta", ["arch", "-x86_64", natives["x86_64"], *common], "rosetta", set(), ""))
    else:
        print("Rosetta 2 is not available: skipping the x86-64 native build", flush=True)
    for c in configs:
        opts, allowed, why = CONFIGS[c]
        cmd = [loader]
        for name, value in opts.items():
            cmd += ["--option", f"{name}={value}"]
        runs.append((f"fex:{c}", cmd + common + [guest], "fex", allowed, why))

    results = {}
    problems = []
    prime_args = ["--quick", "--reps", "0", "--only", "calls"]
    for label, cmd, kind, allowed, why in runs:
        # A first launch of a freshly built program pays for the system's first look at
        # it (signature checks; Rosetta's ahead-of-time translation). Time one short
        # launch for that, then measure.
        i = next(i for i in range(len(cmd)) if cmd[i:i + len(common)] == common)
        _, _, prime = execute(cmd[:i] + prime_args + cmd[i + len(common):], timeout)
        code, lines, marks = execute(cmd, timeout)
        times, sums, verdicts = parse(lines)
        results[label] = {"exit": code, "times": times, "sums": sums, "verdicts": verdicts, "first_launch_start_s": prime["start"], "start_s": marks["start"],
                          "first_result_s": marks["first"], "cmd": cmd}
        bad = {k for k, v in verdicts.items() if v != "ok"}
        unexplained = bad - allowed
        if "TIMEOUT" in lines:
            problems.append(f"{label}: timed out after {timeout} s")
        if not lines or not lines[-1].startswith("END") and "TIMEOUT" not in lines:
            problems.append(f"{label}: did not finish (exit {code}); last line: {lines[-1] if lines else '(none)'}")
        if unexplained:
            problems.append(f"{label}: wrong checksum in {', '.join(sorted(unexplained))}")
        expected_exit = sum(1 for v in verdicts.values() if v in ("MISMATCH", "VARIES"))
        if code != expected_exit and "TIMEOUT" not in lines:
            problems.append(f"{label}: exit status {code}, expected {expected_exit}")
        explained = bad & allowed
        note = f" (allowed: {', '.join(sorted(explained))}: {why})" if explained else ""
        print(f"{label}: exit {code}, {len(verdicts)} kernels, {len(bad)} not ok{note}", flush=True)

    # Cross-build comparison of the checksums themselves.
    ref_x86 = results.get("rosetta")
    ref_arm = results["native-arm64"]
    for label, r in results.items():
        allowed = next((a for l, _, _, a, _ in runs if l == label), set())
        for k, s in r["sums"].items():
            if k in allowed:
                continue
            if k in X86_ONLY:
                if ref_x86 and label != "native-arm64" and ref_x86["sums"].get(k) not in (None, s):
                    problems.append(f"{label}: {k} checksum {s} differs from Rosetta's {ref_x86['sums'][k]}")
            elif ref_arm["sums"].get(k) not in (None, s):
                problems.append(f"{label}: {k} checksum {s} differs from native arm64's {ref_arm['sums'][k]}")

    if not args.quick:
        print_table(results, [r[0] for r in runs])
    meta = {
        "date": time.strftime("%Y-%m-%d %H:%M:%S"),
        "machine": platform.machine(),
        "macos": platform.mac_ver()[0],
        "load_before": os.getloadavg(),
        "quick": args.quick,
        "reps": reps,
        "hardened": args.hardened,
        "cflags": CFLAGS,
        "configs": {c: CONFIGS[c][0] for c in configs},
    }
    with open(os.path.join(out, "results-quick.json" if args.quick else "results.json"), "w") as f:
        json.dump({"meta": meta, "results": results}, f, indent=1)
    print(f"load average now: {' '.join(f'{x:.2f}' for x in os.getloadavg())}")
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print("  " + p)
        return 1
    print("PASS: every checksum as expected")
    return 0


def print_table(results, labels):
    kernels = list(results["native-arm64"]["times"].keys())

    def med(label, k):
        t = results.get(label, {}).get("times", {}).get(k, [])
        return statistics.median(t[1:]) / 1e6 if len(t) > 1 else None

    def mn(label, k):
        t = results.get(label, {}).get("times", {}).get(k, [])
        return min(t[1:]) / 1e6 if len(t) > 1 else None

    short = {l: l.replace("native-arm64", "arm64").replace("fex:", "") for l in labels}
    print("\nMedian ms per kernel (min in brackets). x/R = this column / Rosetta.")
    head = f"{'kernel':16}" + "".join(f"{short[l]:>24}" for l in labels)
    print(head)
    print("-" * len(head))
    for k in kernels:
        row = f"{k:16}"
        ros = med("rosetta", k)
        for l in labels:
            m, n = med(l, k), mn(l, k)
            if m is None:
                row += f"{'-':>24}"
                continue
            cell = f"{m:.1f} ({n:.1f})"
            if ros and l.startswith("fex:"):
                cell += f" {m / ros:.2f}x"
            row += f"{cell:>24}"
        print(row)
    print("\nWarm-up (first run, includes translation) ms; seconds from launch to the program's first output\n"
          "(START, before any kernel) at the first launch of the fresh build and at the measured launch, and\n"
          "to its first result (end of crc32's warm-up)")
    for k in kernels:
        row = f"{k:16}"
        for l in labels:
            t = results.get(l, {}).get("times", {}).get(k, [])
            row += f"{(t[0] / 1e6 if t else 0):>24.1f}"
        print(row)
    for key, title in (("first_launch_start_s", "1st launch out s"), ("start_s", "first output s"),
                       ("first_result_s", "first result s")):
        row = f"{title:16}"
        for l in labels:
            fr = results.get(l, {}).get(key)
            row += f"{(fr if fr is not None else 0):>24.3f}"
        print(row)
    print("\nGeometric mean of median / Rosetta median over the kernels:")
    for l in labels:
        if not l.startswith("fex:") and l != "native-arm64":
            continue
        ratios = [med(l, k) / med("rosetta", k) for k in kernels if med(l, k) and med("rosetta", k)]
        if ratios:
            print(f"  {short[l]:16} {statistics.geometric_mean(ratios):.2f}")


if __name__ == "__main__":
    sys.exit(main())
