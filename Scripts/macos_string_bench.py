#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""String-instruction microbenchmark (round 2, task E2).

    python3 Scripts/macos_string_bench.py --loader build/Bin/FEXCoreBench --out string-bench [--launches 3] [--background]

Builds unittests/macOS/Bench/strings.c as a guest image (with platform_guest.c, as
Scripts/macos_bench.py builds the kernels) and as an x86-64 macOS program for Rosetta 2,
then runs: Rosetta 2, and FEXCoreBench with string ordering off (scalar ordering only),
on (one acquire load and release store per element), and on with MEMCPYSETTSOWIDE=1
(8-byte acquire/release units). Each launch reports, per case, the best of --reps timed
batches in nanoseconds per instruction; the table gives the median over --launches
launches. Every launch's destination checksums must agree across all builds.

--background runs every launch under `taskpolicy -b` (the darwin background policy,
which macOS schedules on the efficiency cores). Writes results.json and results.md to
--out. Exit status 0 if every launch completed with the same checksums, 1 otherwise.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "unittests", "macOS", "Bench")
sys.path.insert(0, os.path.join(ROOT, "Scripts"))
import macos_bench as mb  # noqa: E402  (shares flags, target and image base)

CONFIGS = {
    "string_off": {"MULTIBLOCK": "1", "TSOENABLED": "1", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "0"},
    "string_on": {"MULTIBLOCK": "1", "TSOENABLED": "1", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "1"},
    "string_on_wide": {"MULTIBLOCK": "1", "TSOENABLED": "1", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "1",
                       "MEMCPYSETTSOWIDE": "1"},
}


def build(out):
    os.makedirs(out, exist_ok=True)
    s = os.path.join(SRC, "strings.c")
    obj = os.path.join(out, "strings-x86_64.o")
    objg = os.path.join(out, "platform_guest.o")
    guest = os.path.join(out, "strings-guest.macho")
    mb.run(["clang", *mb.X86_TARGET, *mb.CFLAGS, "-c", s, "-o", obj])
    mb.run(["clang", *mb.X86_TARGET, *mb.CFLAGS, "-c", os.path.join(SRC, "platform_guest.c"), "-o", objg])
    mb.run(["clang", *mb.X86_TARGET, "-nostdlib", "-static", "-e", "_bench_entry", "-Wl,-image_base," + mb.GUEST_BASE,
            "-Wl,-no_pie", obj, objg, "-o", guest])
    objn = os.path.join(out, "strings-x86_64-native.o")
    exe = os.path.join(out, "strings-x86_64")
    mb.run(["clang", *mb.X86_TARGET, *mb.CFLAGS, "-c", s, "-o", objn])
    mb.run(["clang", *mb.X86_TARGET, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", objn,
            os.path.join(SRC, "platform_native.c"), "-o", exe])
    # An unsigned x86-64 program hangs unkillably at launch under Rosetta 2 here.
    mb.run(["codesign", "--force", "--sign", "-", exe])
    return guest, exe


def parse(stdout):
    res = {}
    done = None
    for line in stdout.splitlines():
        p = line.split()
        if p and p[0] == "S" and len(p) == 5:
            res[f"{p[1]} {p[2]}"] = (int(p[3]) / 100.0, p[4])
        elif p and p[0] == "DONE":
            done = int(p[1])
    return res, done


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--loader", required=True)
    ap.add_argument("--out", default="string-bench")
    ap.add_argument("--launches", type=int, default=3)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--background", action="store_true", help="run every launch under taskpolicy -b (efficiency cores)")
    ap.add_argument("--no-rosetta", action="store_true")
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    guest, exe = build(out)
    loader = os.path.abspath(args.loader)
    runs = []
    if not args.no_rosetta:
        runs.append(("rosetta", ["arch", "-x86_64", exe]))
    for c, opts in CONFIGS.items():
        cmd = [loader]
        for k, v in opts.items():
            cmd += ["--option", f"{k}={v}"]
        runs.append((f"fex:{c}", cmd))
    prefix = ["taskpolicy", "-b"] if args.background else []
    extra = ["--reps", str(args.reps)] + (["--quick"] if args.quick else [])
    data = {label: [] for label, _ in runs}
    problems = []
    for launch in range(args.launches):
        order = runs[launch % len(runs):] + runs[:launch % len(runs)]
        for label, cmd in order:
            full = prefix + cmd + extra + ([guest] if label.startswith("fex:") else [])
            print("+", " ".join(full), flush=True)
            try:
                p = subprocess.run(full, capture_output=True, text=True, timeout=600)
            except subprocess.TimeoutExpired:
                problems.append(f"{label} launch {launch}: timeout")
                continue
            res, done = parse(p.stdout)
            if p.returncode != 0 or done != 0 or not res:
                problems.append(f"{label} launch {launch}: exit {p.returncode}, DONE {done}, {p.stderr.strip()[:200]}")
            data[label].append(res)
    cases = []
    for r in data.values():
        for launch in r:
            for k in launch:
                if k not in cases:
                    cases.append(k)
    sums = {}
    for label, launches in data.items():
        for launch in launches:
            for k, (_, h) in launch.items():
                sums.setdefault(k, set()).add(h)
    for k, hs in sums.items():
        if len(hs) != 1:
            problems.append(f"{k}: destination checksums differ between launches or builds: {sorted(hs)}")
    med = {label: {k: statistics.median([l[k][0] for l in launches if k in l]) for k in cases if any(k in l for l in launches)}
           for label, launches in data.items()}
    labels = [l for l, _ in runs]
    lines = [f"String instructions, ns per instruction, median of {args.launches} launches"
             f"{' under taskpolicy -b' if args.background else ''}", "",
             "| case | " + " | ".join(labels) + " | wide vs on | wide vs off |", "|---|" + "---|" * (len(labels) + 2)]
    for k in cases:
        row = [f"{med[l].get(k, float('nan')):.2f}" for l in labels]
        on, wide, off = (med.get("fex:string_on", {}).get(k), med.get("fex:string_on_wide", {}).get(k),
                         med.get("fex:string_off", {}).get(k))
        r1 = f"{on / wide:.2f}" if on and wide else "-"
        r2 = f"{wide / off:.2f}" if wide and off else "-"
        lines.append(f"| {k} | " + " | ".join(row) + f" | {r1} | {r2} |")
    md = "\n".join(lines) + "\n"
    print(md)
    with open(os.path.join(out, "results.md"), "w") as f:
        f.write(md)
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump({"background": args.background, "launches": args.launches, "reps": args.reps, "configs": CONFIGS,
                   "median_ns": med, "raw": data, "problems": problems}, f, indent=1)
    for p in problems:
        print("PROBLEM:", p)
    print("PASS" if not problems else "FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
