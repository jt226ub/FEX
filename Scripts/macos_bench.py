#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds the benchmark in unittests/macOS/Bench three ways, runs it and reports.

    # The measurement: one command, writes results.json and results.md into --out.
    python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-out [--final] [--write-docs]
    # The correctness check the Mac workflow runs (checksums only, no timing):
    python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-quick --quick [--hardened]

The three builds come from the same C source with the same compiler flags, apart from the
target and the platform file (see unittests/macOS/Bench/bench.h):

  guest    a freestanding x86-64 image (static Mach-O at a fixed address, no libc), run
           by FEXCoreBench through the fexmac interface in each translator configuration;
  rosetta  an x86-64 macOS program, run under Rosetta 2 (skipped if Rosetta is missing);
  native   an arm64 macOS program: the ceiling.

Correctness. Every run must report exactly the expected set of kernels (all of them, or
the --only list; an unknown name is an error), complete, and give the recorded checksum
for each; the builds are also compared with each other (bit-exact; the x87 kernel only
between the x86-64 builds, since long double is 64 bits on arm64). Two configurations
have stated exceptions: with reduced x87 precision the x87 checksum must be the value
recorded for that mode; with the memory-ordering emulation off the two thread kernels may
give any value (the message-passing kernel relies on x86 store order; the spin lock is
released with a plain store that can then be reordered before the critical section's
stores, so an update can be lost), but the run must complete and every other kernel must
be right. The literal mode (exit criterion 7 to the letter: single-threaded kernels, no
system call but exit) is run natively and under FEX and must report every checksum right.

Timing. --launches independent launches of every build and configuration, interleaved
(the order rotates each round, so drift reaches all of them alike); each launch does one
warm-up and --reps timed runs per kernel. Per kernel the report gives the median of the
launch medians and the range of the launch medians. Summary sets, reported separately:
  headline  single-threaded integer, memory and floating-point kernels (all but x87 and
            the thread kernels); geometric mean of the ratio to Rosetta 2 per build;
  x87       x86-64 builds only (Rosetta 2 and FEX), never against native arm64;
  threads   reported with their spread, in no mean.
A kernel enters a mean for a configuration only if it was right in every launch of that
configuration. Time to first kernel result: launch to the first result line of a short
run (`--only calls`), at the first launch of a fresh build and at later launches.

Load. Other processes' CPU use (ps, excluding this script's own process tree) and the load
average are sampled every 2 s and stored in the JSON. A run is labelled FINAL only with
--final and only if the other processes used at most --max-mean-cores on average and
--max-peak-cores at any sample; otherwise it is PROVISIONAL (with --final, exit status 2).

Exit status: 0 all correct, 1 a correctness problem, 2 --final refused because of load.
"""

import argparse
import ctypes
import datetime
import json
import os
import platform
import queue
import re
import resource
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "unittests", "macOS", "Bench")
DOC = os.path.join(ROOT, "docs", "macOS-benchmarks.md")
STATUS_DOC = os.path.join(ROOT, "docs", "macOS-port-status.md")
DOC_BEGIN = "<!-- bench-results:begin -->"
DOC_END = "<!-- bench-results:end -->"
SUM_BEGIN = "<!-- bench-summary:begin -->"
SUM_END = "<!-- bench-summary:end -->"
# The results the committed documents were generated from (written by --write-docs).
DOC_JSON = os.path.join(ROOT, "docs", "macOS-benchmarks-results.json")

# Identical for every build. -ffreestanding (which implies -fno-builtin) is used for the
# kernels in all three builds so the compiler makes the same choices in each.
CFLAGS = ["-O2", "-std=c11", "-ffreestanding", "-ffp-contract=off", "-fno-stack-protector", "-fno-stack-check", "-Wall",
          "-Wextra", "-Werror"]
X86_TARGET = ["-target", "x86_64-apple-macos11"]
ARM_TARGET = ["-target", "arm64-apple-macos11"]
# Above 4GB (macOS allows nothing below) and clear of the ranges macOS reserves.
GUEST_BASE = "0x20000000000"

# The kernels, in the program's order (kernels.c). Every run must report exactly these
# (or the --only subset).
ALL_KERNELS = ["crc32", "hash64", "sort", "interp", "dfa", "rep_movs", "copy_loop", "ptr_chase", "matmul_f64", "matmul_f32x4",
               "nbody", "fft", "x87", "muldiv", "calls", "deep_recursion", "threads_lock", "threads_mp"]
THREAD_KERNELS = ["threads_lock", "threads_mp"]
X87_KERNELS = ["x87"]
HEADLINE_KERNELS = [k for k in ALL_KERNELS if k not in THREAD_KERNELS and k not in X87_KERNELS]
LITERAL_KERNELS = [k for k in ALL_KERNELS if k not in THREAD_KERNELS]

TSO_ALL = {"TSOENABLED": "1", "VECTORTSOENABLED": "1", "MEMCPYSETTSOENABLED": "1"}
BASE = {"MAXINST": "500", "MULTIBLOCK": "1"}
ANY = None  # an allowance: any checksum
# The x87 checksum with X87REDUCEDPRECISION=1 (deterministic; recorded from this port).
X87_REDUCED = {"quick": "7961f3e8d7c11f74", "full": "656e7da3b860426c"}

# name: (options, {kernel: allowance}, description). An allowance is ANY or the checksum
# expected instead of the recorded one, per size.
CONFIGS = {
    "tso_on": ({**BASE, **TSO_ALL}, {}, "all three ordering switches on: the like-for-like comparison with Rosetta 2"),
    "tso_scalar": ({**BASE, "TSOENABLED": "1", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "0"}, {},
                   "ordering for scalar accesses only (FEX's defaults for the vector and string switches)"),
    "tso_vector": ({**BASE, "TSOENABLED": "1", "VECTORTSOENABLED": "1", "MEMCPYSETTSOENABLED": "0"}, {},
                   "scalar and vector ordering, string instructions without"),
    "tso_memcpy": ({**BASE, "TSOENABLED": "1", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "1"}, {},
                   "scalar and string-instruction ordering, vectors without"),
    "tso_on_nomb": ({**BASE, **TSO_ALL, "MULTIBLOCK": "0"}, {}, "all ordering on, multiblock off"),
    "tso_on_x87rp": ({**BASE, **TSO_ALL, "X87REDUCEDPRECISION": "1"}, {"x87": X87_REDUCED},
                     "all ordering on, x87 in 64-bit precision"),
    "tso_off": ({**BASE, "TSOENABLED": "0", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "0"}, {
        "threads_mp": ANY,
        "threads_lock": ANY
    }, "no ordering emulation: not correct for multi-threaded guests; an upper bound for guests known not to need it"),
}
# Round 2 experiments (task E2), off by default in the translator and not in the default
# set: vector ordering as general-register acquire/release accesses (VECTORTSOSPLIT), and a
# vector load's barrier merged with the next vector store's (TSOBARRIERMERGE).
CONFIGS["tso_on_fp64"] = ({**BASE, **TSO_ALL, "VECTORTSOSPLIT": "3"}, {},
                          "as tso_on, vector-register accesses of 8 bytes or less as one acquire load or release store "
                          "through a general register (VECTORTSOSPLIT=3)")
CONFIGS["tso_on_merge"] = ({**BASE, **TSO_ALL, "TSOBARRIERMERGE": "1"}, {},
                           "as tso_on, a vector load's barrier merged into the next vector store's (TSOBARRIERMERGE)")
CONFIGS["tso_on_fp64_merge"] = ({**BASE, **TSO_ALL, "VECTORTSOSPLIT": "3", "TSOBARRIERMERGE": "1"}, {},
                                "as tso_on, with VECTORTSOSPLIT=3 and TSOBARRIERMERGE")
# Recorded, not proposed: 16-byte accesses as two 8-byte halves break x86's aligned
# 16-byte atomicity (the TEAR tests of FEXCoreOrderingTest see torn values).
CONFIGS["tso_on_split16"] = ({**BASE, **TSO_ALL, "VECTORTSOSPLIT": "15"}, {},
                             "as tso_on, every vector access split, 16 bytes as two 8-byte halves (VECTORTSOSPLIT=15; "
                             "not atomic, dropped)")
# Round 2, E2: string instructions in 8-byte acquire/release units (MEMCPYSETTSOWIDE).
CONFIGS["tso_on_wide"] = ({**BASE, **TSO_ALL, "MEMCPYSETTSOWIDE": "1"}, {},
                          "as tso_on, with rep movs/stos in 8-byte acquire loads and release stores")
CONFIGS["tso_memcpy_wide"] = ({**BASE, "TSOENABLED": "1", "VECTORTSOENABLED": "0", "MEMCPYSETTSOENABLED": "1",
                               "MEMCPYSETTSOWIDE": "1"}, {}, "as tso_memcpy, with rep movs/stos in 8-byte acquire loads and release stores")
# The recommended set of round 2 options together.
CONFIGS["tso_on_r2"] = ({**BASE, **TSO_ALL, "VECTORTSOSPLIT": "3", "TSOBARRIERMERGE": "1", "MEMCPYSETTSOWIDE": "1"}, {},
                        "as tso_on, with VECTORTSOSPLIT=3, TSOBARRIERMERGE and MEMCPYSETTSOWIDE (the round 2 set)")
E2_CONFIGS = ["tso_on", "tso_on_fp64", "tso_on_merge", "tso_on_fp64_merge", "tso_on_wide", "tso_on_r2", "tso_on_split16",
              "tso_scalar"]
CONFIGS["tso_on_cache_cold"] = (CONFIGS["tso_on"][0], {}, "as tso_on, with the code cache on in a fresh, empty directory "
                                 "for every launch: everything is compiled and written to the cache")
CONFIGS["tso_on_cache_warm"] = (CONFIGS["tso_on"][0], {}, "as tso_on, with the code cache on in a directory one run "
                                 "with the same arguments filled first: translations are loaded, not compiled")
# The code cache, through FEXCoreBench --code-cache (fexmac's code_cache_* fields), in a
# private directory under --out; never the user's ~/Library/Caches.
CACHE_MODE = {"tso_on_cache_cold": "cold", "tso_on_cache_warm": "warm"}
DEFAULT_CONFIGS = ["tso_on", "tso_scalar", "tso_vector", "tso_memcpy", "tso_on_nomb", "tso_on_x87rp", "tso_off",
                   "tso_on_cache_cold", "tso_on_cache_warm"]
QUICK_CONFIGS = ["tso_on", "tso_off", "tso_on_nomb", "tso_on_x87rp", "tso_on_cache_cold", "tso_on_cache_warm", "tso_on_wide"]
STARTUP_CONFIG = "tso_on"


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
    # A guest that breaks the loader's contract on purpose (contract_guest.c).
    obj_c = os.path.join(out, "contract_guest.o")
    contract = os.path.join(out, "contract-guest.macho")
    run(["clang", *X86_TARGET, *CFLAGS, "-c", os.path.join(SRC, "contract_guest.c"), "-o", obj_c])
    run(["clang", *X86_TARGET, "-nostdlib", "-static", "-e", "_bench_entry", "-Wl,-image_base," + GUEST_BASE, "-Wl,-no_pie",
         obj_c, obj_g, "-o", contract])
    natives = {"contract": contract}
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
    """Runs one process with a real deadline. Returns a dict: exit code (None if it had to
    be abandoned), output lines, seconds from launch to the first START and K lines,
    whether it timed out, and whether it could not be killed."""
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
    CURRENT_CHILD[0] = proc.pid
    lines_q = queue.Queue()

    def reader(stream, tag):
        for line in stream:
            lines_q.put((tag, time.monotonic() - start, line.rstrip("\n")))
        lines_q.put((tag, None, None))

    for stream, tag in ((proc.stdout, "out"), (proc.stderr, "err")):
        threading.Thread(target=reader, args=(stream, tag), daemon=True).start()
    lines, errors, marks = [], [], {"start": None, "first": None}
    open_streams = 2
    timed_out = False
    while open_streams:
        left = timeout - (time.monotonic() - start)
        if left <= 0:
            timed_out = True
            break
        try:
            tag, at, line = lines_q.get(timeout=min(left, 1.0))
        except queue.Empty:
            continue
        if line is None:
            open_streams -= 1
            continue
        if tag == "err":
            errors.append(line)
            continue
        if marks["start"] is None and line.startswith("START"):
            marks["start"] = at
        if marks["first"] is None and line.startswith("K "):
            marks["first"] = at
        lines.append(line)
    unkillable = False
    if timed_out:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            pass
    try:
        code = proc.wait(timeout=10 if timed_out else max(1, timeout - (time.monotonic() - start)))
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            code = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            # For example a process stuck in Rosetta's launch: report it and go on.
            code, unkillable = None, True
            timed_out = True
    CURRENT_CHILD[0] = None
    return {"exit": code, "lines": lines, "stderr": errors, "start_s": marks["start"], "first_result_s": marks["first"],
            "timed_out": timed_out, "unkillable": unkillable, "seconds": time.monotonic() - start}


def parse(lines):
    times, sums, verdicts, errors, end = {}, {}, {}, [], None
    for line in lines:
        f = line.split()
        if not f:
            continue
        if f[0] == "K" and len(f) == 5:
            times.setdefault(f[1], []).append(int(f[3]))
        elif f[0] == "C" and len(f) >= 4:
            verdicts[f[1]] = f[2]
            sums[f[1]] = f[3]
        elif f[0] == "END":
            end = line
        elif f[0] == "ERROR":
            errors.append(line)
    return times, sums, verdicts, errors, end


class HostCPU:
    """Whole-machine CPU time and this script's own, from the kernel's counters (not ps,
    whose %cpu is a decaying average that barely sees short-lived processes)."""

    TICKS_PER_SECOND = 100  # host_processor_info's CPU_STATE ticks (checked: 100 per s per CPU)

    class _RUsageV0(ctypes.Structure):
        _fields_ = [("uuid", ctypes.c_uint8 * 16), ("user", ctypes.c_uint64), ("system", ctypes.c_uint64),
                    ("rest", ctypes.c_uint64 * 8)]

    class _Timebase(ctypes.Structure):
        _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]

    def __init__(self):
        self.lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
        self.lib.mach_host_self.restype = ctypes.c_uint
        self.task = ctypes.c_uint.in_dll(self.lib, "mach_task_self_")
        tb = HostCPU._Timebase()
        self.lib.mach_timebase_info(ctypes.byref(tb))
        self.abs_to_s = tb.numer / tb.denom / 1e9

    def busy_seconds(self):
        """CPU seconds spent busy (user, system, nice) on all processors since boot."""
        count, n = ctypes.c_uint(), ctypes.c_uint()
        info = ctypes.POINTER(ctypes.c_int)()
        if self.lib.host_processor_info(self.lib.mach_host_self(), 2, ctypes.byref(count), ctypes.byref(info),
                                        ctypes.byref(n)) != 0:
            raise OSError("host_processor_info failed")
        busy = 0
        for c in range(count.value):
            user, system, _idle, nice = (info[c * 4 + k] & 0xFFFFFFFF for k in range(4))
            busy += user + system + nice
        self.lib.vm_deallocate(self.task, ctypes.cast(info, ctypes.c_void_p), ctypes.c_size_t(n.value * 4))
        return busy / HostCPU.TICKS_PER_SECOND

    def process_seconds(self, pid):
        """CPU seconds of a running (or not yet reaped) process; 0 if it is gone."""
        ru = HostCPU._RUsageV0()
        if self.lib.proc_pid_rusage(ctypes.c_int(pid), 0, ctypes.byref(ru)) != 0:
            return 0.0
        return (ru.user + ru.system) * self.abs_to_s


# The benchmark process now running, if any, so its CPU time is not counted as other load.
CURRENT_CHILD = [None]


class LoadSampler:
    """Samples, every interval seconds, how many cores processes other than this script
    and the programs it runs kept busy, from whole-machine CPU ticks minus this script's
    own CPU time (itself, its reaped children, and the child now running); and the load
    average."""

    def __init__(self, interval=2.0):
        self.interval = interval
        self.samples = []
        self.cpu = HostCPU()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, daemon=True)

    def _own_seconds(self):
        me = resource.getrusage(resource.RUSAGE_SELF)
        ch = resource.getrusage(resource.RUSAGE_CHILDREN)
        total = me.ru_utime + me.ru_stime + ch.ru_utime + ch.ru_stime
        pid = CURRENT_CHILD[0]
        if pid:
            total += self.cpu.process_seconds(pid)
        return total

    def _loop(self):
        prev = (time.monotonic(), self.cpu.busy_seconds(), self._own_seconds())
        while not self._stop.wait(self.interval):
            now = (time.monotonic(), self.cpu.busy_seconds(), self._own_seconds())
            wall = now[0] - prev[0]
            if wall > 0:
                other = max(0.0, (now[1] - prev[1]) - (now[2] - prev[2])) / wall
                own = max(0.0, now[2] - prev[2]) / wall
                self.samples.append({"t": time.time(), "other_cores": round(other, 2), "own_cores": round(own, 2),
                                     "loadavg": os.getloadavg()[0]})
            prev = now

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=10)

    def summary(self):
        c = [s["other_cores"] for s in self.samples]
        la = [s["loadavg"] for s in self.samples]
        if not c:
            return {"samples": 0}
        return {"samples": len(c), "other_cores_mean": round(statistics.mean(c), 2), "other_cores_max": round(max(c), 2),
                "own_cores_mean": round(statistics.mean(s["own_cores"] for s in self.samples), 2),
                "loadavg_mean": round(statistics.mean(la), 2), "loadavg_max": round(max(la), 2),
                "method": "host_processor_info CPU ticks minus this script's own CPU time (getrusage, proc_pid_rusage)"}


# (case, extra loader arguments, expected exit status: a negative number is death by that
# signal). 101 is FEXCoreBench's "the guest broke the contract".
CONTRACT_CASES = [
    ("ok", [], 0),
    ("ok", ["--literal"], 0),
    ("write", [], 0),
    ("write", ["--literal"], 101),
    ("unjoined", [], 101),
    ("worker-exit", [], 101),
    ("main-thread-exit", [], 101),
    ("bad-syscall", [], 101),
    ("ud2", [], -signal.SIGILL),
]


def check_contract(loader, image, timeout, problems):
    """Runs the contract-breaking guest; each case must end with its expected status."""
    for case, extra, want in CONTRACT_CASES:
        r = execute([loader, *extra, "--only", case, image], timeout)
        ok = not r["timed_out"] and r["exit"] == want
        print(f"contract {case} {' '.join(extra)}: exit {r['exit']} (expected {want})" +
              ("" if ok else f" FAIL: {' / '.join(r['stderr'][-2:])}"), flush=True)
        if not ok:
            problems.append(f"loader contract case {case} {' '.join(extra)}: exit {r['exit']}, expected {want}")


def check_run(label, r, want, allowed, size, problems, literal=False):
    """Checks one parsed run. Returns {kernel: True if its checksum is right (or the
    allowed alternative value)}."""
    ok = {}
    for line in r["stderr"]:
        f = line.split()
        if line.startswith("FEXCoreBench: translation:") and len(f) >= 12:
            r["counters"] = {"compiled": int(f[3]), "loaded": int(f[5]), "rejected": int(f[7]), "queued": int(f[9]),
                             "cache": f[11]}
    if r["timed_out"]:
        problems.append(f"{label}: timed out after {r['seconds']:.0f} s" + (" and could not be killed" if r["unkillable"] else ""))
        return ok
    if literal:
        report = [l for l in r["stderr"] if "kernel(s) run" in l]
        n = None
        for l in report:
            f = l.replace(",", " ").split()
            for i, w in enumerate(f):
                if w == "kernel(s)" and i > 0 and f[i - 1].isdigit():
                    n = int(f[i - 1])
        if n != len(LITERAL_KERNELS):
            problems.append(f"{label}: literal mode ran {n} kernel(s), expected {len(LITERAL_KERNELS)}")
        if r["exit"] != 0:
            problems.append(f"{label}: literal mode exit status {r['exit']} (expected 0): {' / '.join(r['stderr'][-3:])}")
        if r["lines"]:
            problems.append(f"{label}: literal mode wrote output: {r['lines'][:2]}")
        return ok
    times, sums, verdicts, errors, end = parse(r["lines"])
    r.update({"times": times, "sums": sums, "verdicts": verdicts})
    for line in r["lines"]:
        f = line.split()
        if len(f) == 3 and f[0] == "CLOCK" and f[2].isdigit():
            r["clock_ns"] = int(f[2])
    for e in errors:
        problems.append(f"{label}: {e}")
    if end is None:
        problems.append(f"{label}: did not finish (exit {r['exit']}); stderr: {' / '.join(r['stderr'][-3:])}")
    got = list(verdicts)
    if got != want:
        problems.append(f"{label}: ran kernels {got}, expected {want}")
    wrong = 0
    for k in want:
        v = verdicts.get(k)
        if v is None:
            continue
        if v != "ok":
            wrong += 1
        if k in allowed:
            a = allowed[k]
            ok[k] = v == "ok" if a is ANY else sums.get(k) == a[size]
            if a is not ANY and not ok[k]:
                problems.append(f"{label}: {k} checksum {sums.get(k)}, expected {a[size]} for this configuration")
        else:
            ok[k] = v == "ok"
            if not ok[k]:
                problems.append(f"{label}: {k} {v} (checksum {sums.get(k)})")
    expected_exit = 0 if wrong == 0 else 1
    if end is not None and r["exit"] != expected_exit:
        problems.append(f"{label}: exit status {r['exit']}, expected {expected_exit}")
    return ok


def launch_median(r, k):
    t = r.get("times", {}).get(k, [])
    return statistics.median(t[1:]) / 1e6 if len(t) > 1 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--loader", help="FEXCoreBench (required except with --check-docs and --measure-load)")
    ap.add_argument("--check-docs", nargs="?", const=DOC_JSON, metavar="JSON",
                    help="check that the documents' generated blocks match this results file (default: the committed "
                    "one) and that no measured figure appears outside them")
    ap.add_argument("--out", default="bench-out")
    ap.add_argument("--quick", action="store_true", help="correctness only: small sizes, one launch, no timing report")
    ap.add_argument("--launches", type=int, default=5)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--only", default="", help="comma-separated kernel names")
    ap.add_argument("--configs", default="", help="comma-separated: " + ",".join(CONFIGS) + "; or e2 for " + ",".join(E2_CONFIGS))
    ap.add_argument("--qos", choices=["default", "background"], default="default",
                    help="background runs every build under taskpolicy -b (background QoS, which macOS keeps on the "
                    "efficiency cores of an M-series chip)")
    ap.add_argument("--hardened", action="store_true", help="run a copy of the loader signed for the hardened runtime")
    ap.add_argument("--no-rosetta", action="store_true")
    ap.add_argument("--timeout", type=int, default=0, help="seconds per launch (default 300, quick 120)")
    ap.add_argument("--final", action="store_true", help="label the run FINAL if the machine was quiet enough")
    ap.add_argument("--max-mean-cores", type=float, default=0.5)
    ap.add_argument("--max-peak-cores", type=float, default=1.5)
    ap.add_argument("--measure-load", type=float, default=0, metavar="SECONDS",
                    help="only sample the machine's load for this long and say whether --final would accept it")
    ap.add_argument("--measure-load-own", default="", metavar="COMMAND",
                    help="with --measure-load: run this command as the script's own child meanwhile (it must not count)")
    ap.add_argument("--write-docs", action="store_true", help=f"replace the generated block in {os.path.relpath(DOC, ROOT)}")
    args = ap.parse_args()
    if args.measure_load:
        return measure_load(args)
    if args.check_docs:
        return check_docs(args.check_docs)
    if not args.loader:
        ap.error("--loader is required")

    only = [k for k in args.only.split(",") if k] if args.only else []
    unknown = [k for k in only if k not in ALL_KERNELS]
    if unknown or (args.only and not only):
        sys.exit(f"unknown kernel name(s) in --only: {unknown or args.only}; known: {','.join(ALL_KERNELS)}")
    want = [k for k in ALL_KERNELS if not only or k in only]
    configs = [c for c in (args.configs.split(",") if args.configs else (QUICK_CONFIGS if args.quick else DEFAULT_CONFIGS)) if c]
    if configs == ["e2"]:
        configs = list(E2_CONFIGS)
    for c in configs:
        if c not in CONFIGS:
            sys.exit(f"unknown configuration {c}")
    size = "quick" if args.quick else "full"
    reps = 1 if args.quick else args.reps
    launches = 1 if args.quick else args.launches
    timeout = args.timeout or (120 if args.quick else 300)

    out = os.path.abspath(args.out)
    guest, natives = build(out)
    loader = os.path.abspath(args.loader)
    if args.hardened:
        hardened = os.path.join(out, "FEXCoreBench-hardened")
        shutil.copy2(loader, hardened)
        run(["codesign", "--force", "--sign", "-", "--options", "runtime", "--entitlements",
             os.path.join(ROOT, "Data", "macOS", "jit.entitlements"), hardened])
        loader = hardened

    cache_root = os.path.join(out, "code-cache")
    if os.path.isdir(cache_root):
        shutil.rmtree(cache_root)
    os.makedirs(cache_root, mode=0o700)
    cold_count = [0]

    def fex_cmd(c, extra):
        cmd = [loader]
        for name, value in CONFIGS[c][0].items():
            cmd += ["--option", f"{name}={value}"]
        mode = CACHE_MODE.get(c)
        if mode == "cold":
            cold_count[0] += 1
            cmd += ["--code-cache", os.path.join(cache_root, f"cold-{cold_count[0]}")]
        elif mode == "warm":
            cmd += ["--code-cache", os.path.join(cache_root, "warm")]
        return cmd + extra + [guest]

    have_rosetta = not args.no_rosetta and rosetta_available()
    if not have_rosetta:
        print("Rosetta 2 is not available: skipping the x86-64 native build", flush=True)

    # label: (command builder taking the program arguments, allowances)
    qos = ["taskpolicy", "-b"] if args.qos == "background" else []
    builds = [("native-arm64", lambda a: [*qos, natives["arm64"], *a], {})]
    if have_rosetta:
        builds.append(("rosetta", lambda a: [*qos, "arch", "-x86_64", natives["x86_64"], *a], {}))
    for c in configs:
        builds.append((f"fex:{c}", (lambda c: lambda a: [*qos, *fex_cmd(c, a)])(c), CONFIGS[c][1]))
    labels = [b[0] for b in builds]

    sampler = LoadSampler()
    sampler.start()
    problems = []
    started_at = datetime.datetime.now().astimezone()

    # Time to first kernel result at the first launch of the fresh builds (before
    # anything else runs them), then the literal mode.
    probe_args = ["--quick", "--reps", "0", "--only", "calls"]
    probe_labels = [l for l in labels if not l.startswith("fex:") or l == f"fex:{STARTUP_CONFIG}"]
    startup = {l: {"first_launch": None, "later": []} for l in probe_labels}
    for label, cmd_for, _ in builds:
        if label in startup:
            r = execute(cmd_for(probe_args), timeout)
            check_run(f"{label} (startup probe)", r, ["calls"], {}, "quick", problems)
            startup[label]["first_launch"] = r["first_result_s"]

    check_contract(loader, natives["contract"], 60, problems)

    lit_size = ["--quick"] if args.quick else []
    literal_runs = {}
    for label, cmd_for, _ in builds:
        if label == "native-arm64" or label in (f"fex:{STARTUP_CONFIG}", "fex:tso_off"):
            extra = ["--expect-kernels", str(len(LITERAL_KERNELS))] if label.startswith("fex:") else []
            r = execute(cmd_for(["--literal", *lit_size, *extra]), timeout)
            check_run(f"{label} (literal mode)", r, [], {}, size, problems, literal=True)
            literal_runs[label] = {"exit": r["exit"], "stderr": r["stderr"], "seconds": round(r["seconds"], 3)}
            print(f"{label} literal mode: exit {r['exit']}; {r['stderr'][-1] if r['stderr'] else ''}", flush=True)

    # Interleaved launches: round i starts at build i (mod n).
    common = (["--quick"] if args.quick else []) + ["--reps", str(reps)] + (["--only", ",".join(only)] if only else [])
    # Fill the warm cache with one run of exactly what the warm launches run.
    for label, cmd_for, allowed in builds:
        if CACHE_MODE.get(label[4:]) == "warm":
            r = execute(cmd_for(common), timeout)
            check_run(f"{label} (filling the cache)", r, want, allowed, size, problems)
            print(f"{label}: cache filled; {r.get('counters')}", flush=True)
    results = {l: [] for l in labels}
    for i in range(launches):
        order = builds[i % len(builds):] + builds[:i % len(builds)]
        for label, cmd_for, allowed in order:
            r = execute(cmd_for(common), timeout)
            r["ok"] = check_run(f"{label} launch {i + 1}", r, want, allowed, size, problems)
            r["launch"] = i + 1
            mode = CACHE_MODE.get(label[4:])
            c = r.get("counters")
            if label.startswith("fex:") and not c:
                problems.append(f"{label} launch {i + 1}: no translation counters reported")
            elif mode == "warm" and (c["loaded"] == 0 or c["compiled"] > c["loaded"] // 20):
                problems.append(f"{label} launch {i + 1}: warm cache compiled {c['compiled']}, loaded {c['loaded']}")
            elif mode == "cold" and (c["compiled"] == 0 or c["queued"] == 0):
                problems.append(f"{label} launch {i + 1}: cold cache compiled {c['compiled']}, queued {c['queued']}")
            results[label].append(r)
            bad = sorted(k for k, v in r["ok"].items() if not v)
            print(f"{label} launch {i + 1}: exit {r['exit']}, {len(r['ok'])} kernels" +
                  (f", wrong (allowed): {', '.join(bad)}" if bad else ""), flush=True)
        if not args.quick:
            for label, cmd_for, _ in builds:
                if label in startup:
                    p = execute(cmd_for(probe_args), timeout)
                    if p["first_result_s"] is not None:
                        startup[label]["later"].append(p["first_result_s"])

    # Cross-build comparison of the checksums.
    ref_arm = results["native-arm64"][0].get("sums", {})
    ref_x86 = results["rosetta"][0].get("sums", {}) if have_rosetta else {}
    for label in labels:
        allowed = next(b[2] for b in builds if b[0] == label)
        for r in results[label]:
            for k, s in r.get("sums", {}).items():
                if k in allowed:
                    continue
                if k in X87_KERNELS:
                    if label != "native-arm64" and ref_x86.get(k) not in (None, s):
                        problems.append(f"{label} launch {r['launch']}: {k} checksum {s} differs from Rosetta 2's {ref_x86[k]}")
                elif ref_arm.get(k) not in (None, s):
                    problems.append(f"{label} launch {r['launch']}: {k} checksum {s} differs from native arm64's {ref_arm[k]}")

    sampler.stop()
    load = sampler.summary()
    quiet = quiet_enough(load, args)
    describe = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty"], capture_output=True,
                              text=True).stdout.strip()
    dirty = describe.endswith("-dirty") or not describe
    final = args.final and quiet and not args.quick and not dirty
    status = "FINAL" if final else "PROVISIONAL"

    summary = None if args.quick else summarise(results, labels, startup, want)
    meta = {
        "status": status,
        "started": started_at.isoformat(timespec="seconds"),
        "finished": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "machine": machine_name(),
        "macos": platform.mac_ver()[0],
        "clang": subprocess.run(["clang", "--version"], capture_output=True, text=True).stdout.splitlines()[0],
        "git": describe,
        "quick": args.quick,
        "launches": launches,
        "reps": reps,
        "hardened": args.hardened,
        "qos": args.qos,
        "cflags": CFLAGS,
        "configs": {c: {"options": CONFIGS[c][0], "what": CONFIGS[c][2]} for c in configs},
        "load": load,
        "load_thresholds": {"max_mean_cores": args.max_mean_cores, "max_peak_cores": args.max_peak_cores},
        "load_samples": sampler.samples,
    }
    slim = {l: [{k: r.get(k) for k in ("launch", "exit", "times", "sums", "verdicts", "ok", "counters", "first_result_s",
                                        "clock_ns", "stderr", "timed_out", "seconds")}
                for r in results[l]] for l in labels}
    name = "results-quick" if args.quick else "results"
    with open(os.path.join(out, name + ".json"), "w") as f:
        json.dump({"meta": meta, "literal": literal_runs, "startup": startup, "summary": summary, "results": slim}, f, indent=1)
    if summary:
        md = markdown(meta, summary, labels)
        sm = summary_markdown(meta, summary, labels)
        with open(os.path.join(out, name + ".md"), "w") as f:
            f.write(sm + "\n" + md)
        print("\n" + sm + "\n" + md)
        if args.write_docs:
            shutil.copy2(os.path.join(out, name + ".json"), DOC_JSON)
            write_docs(md, sm)
    print(f"load during the run: {load}")
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print("  " + p)
        return 1
    print("PASS: every checksum as expected")
    if args.final and dirty:
        print(f"NOT FINAL: the source tree is not a clean commit ({describe or 'unknown'}); commit first")
        return 2
    if args.final and not final:
        print(f"NOT FINAL: other processes used {load.get('other_cores_mean')} cores on average and "
              f"{load.get('other_cores_max')} at most (limits {args.max_mean_cores} and {args.max_peak_cores}); "
              "the results are labelled PROVISIONAL")
        return 2
    return 0


def quiet_enough(load, args):
    return load.get("samples", 0) > 0 and load["other_cores_mean"] <= args.max_mean_cores and \
        load["other_cores_max"] <= args.max_peak_cores


def measure_load(args):
    """Samples the load for --measure-load seconds (running --measure-load-own as this
    script's child meanwhile, if given) and prints what --final would decide."""
    sampler = LoadSampler()
    sampler.start()
    if args.measure_load_own:
        r = execute(["/bin/sh", "-c", args.measure_load_own], args.measure_load)
        print(f"own child: exit {r['exit']}, {r['seconds']:.1f} s")
    else:
        time.sleep(args.measure_load)
    sampler.stop()
    load = sampler.summary()
    print(json.dumps(load))
    ok = quiet_enough(load, args)
    print("quiet enough for --final" if ok else "too busy for --final")
    return 0 if ok else 2


def machine_name():
    try:
        chip = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
        p = subprocess.run(["sysctl", "-n", "hw.perflevel0.logicalcpu"], capture_output=True, text=True).stdout.strip()
        e = subprocess.run(["sysctl", "-n", "hw.perflevel1.logicalcpu"], capture_output=True, text=True).stdout.strip()
        mem = int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout.strip()) >> 30
        return f"{chip}, {p} performance + {e} efficiency cores, {mem} GB"
    except (OSError, ValueError):
        return platform.machine()


def summarise(results, labels, startup, want):
    per = {}
    for label in labels:
        per[label] = {}
        for k in want:
            meds = [m for m in (launch_median(r, k) for r in results[label]) if m is not None]
            right = all(r.get("ok", {}).get(k, False) for r in results[label])
            wrong_launches = sum(1 for r in results[label] if not r.get("ok", {}).get(k, False))
            per[label][k] = {"median": statistics.median(meds) if meds else None, "min": min(meds) if meds else None,
                             "max": max(meds) if meds else None, "launch_medians": meds, "right": right,
                             "wrong_launches": wrong_launches}

    def geomean(label, kernels):
        if "rosetta" not in per:
            return None, []
        used = [k for k in kernels if k in per[label] and per[label][k]["right"] and per[label][k]["median"] and
                per["rosetta"].get(k, {}).get("median") and per["rosetta"][k]["right"]]
        if not used:
            return None, []
        return statistics.geometric_mean([per[label][k]["median"] / per["rosetta"][k]["median"] for k in used]), used

    means = {}
    for label in labels:
        g, used = geomean(label, [k for k in HEADLINE_KERNELS if k in want])
        means[label] = {"headline": g, "headline_kernels": used}
        if label != "native-arm64":
            gx, usedx = geomean(label, [k for k in X87_KERNELS if k in want])
            means[label]["x87"] = gx
    translation = {}
    for label in labels:
        if not label.startswith("fex:"):
            continue
        rs = results[label]

        def stat(vals):
            vals = [v for v in vals if v is not None]
            return {"median": statistics.median(vals), "min": min(vals), "max": max(vals)} if vals else None

        translation[label] = {
            "compiled": stat([r.get("counters", {}).get("compiled") for r in rs]),
            "loaded": stat([r.get("counters", {}).get("loaded") for r in rs]),
            "first_result_s": stat([r.get("first_result_s") for r in rs]),
            "launch_s": stat([r.get("seconds") for r in rs]),
        }
    st = {}
    for l, v in startup.items():
        later = v["later"]
        st[l] = {"first_launch": v["first_launch"], "later_median": statistics.median(later) if later else None,
                 "later_min": min(later) if later else None, "later_max": max(later) if later else None}
    clock = {}
    for label in labels:
        vals = [r.get("clock_ns") for r in results[label] if r.get("clock_ns") is not None]
        if vals:
            clock[label] = statistics.median(vals)
    return {"per_kernel": per, "means": means, "startup": st, "translation": translation, "clock_ns": clock}


def markdown(meta, s, labels):
    short = {l: l.replace("native-arm64", "native arm64").replace("rosetta", "Rosetta 2").replace("fex:", "") for l in labels}
    per, means = s["per_kernel"], s["means"]
    load = meta["load"]
    o = [DOC_BEGIN, "<!-- Generated by Scripts/macos_bench.py; replace by running it again, do not edit by hand. -->", ""]
    if meta["status"] == "FINAL":
        o.append(f"**FINAL** measurement: other processes used {load.get('other_cores_mean')} cores on average "
                 f"(at most {load.get('other_cores_max')}) during the run.")
    else:
        o.append(f"**PROVISIONAL** numbers: other work was loading the machine (other processes used "
                 f"{load.get('other_cores_mean')} cores on average, at most {load.get('other_cores_max')}, of 10; "
                 f"load average {load.get('loadavg_mean')} on average). They show the shape, not final values.")
    o += ["", f"{meta['machine']}, macOS {meta['macos']}, {meta['clang']}; commit `{meta['git']}`; "
          f"{meta['started']} to {meta['finished']}. {meta['launches']} launches per build and configuration, interleaved, "
          f"each with one warm-up and {meta['reps']} timed runs per kernel. Cells: median of the launch medians in ms "
          "[lowest-highest launch median]. Ratios are to Rosetta 2's median (below 1: faster than Rosetta 2).", ""]
    o.append("FEX configurations (all with `MAXINST=500 MULTIBLOCK=1` unless stated):")
    o.append("")
    for c, v in meta["configs"].items():
        opts = " ".join(f"{k}={x}" for k, x in v["options"].items() if k not in ("MAXINST",))
        o.append(f"- `{c}`: {v['what']} (`{opts}`)")
    o.append("")

    def cell(l, k):
        p = per[l].get(k)
        if not p or p["median"] is None:
            return "-"
        c = f"{p['median']:.1f} [{p['min']:.1f}-{p['max']:.1f}]"
        if not p["right"]:
            c += f" wrong in {p['wrong_launches']}"
        return c

    def ratio(l, k):
        p, r = per[l].get(k), per.get("rosetta", {}).get(k)
        if not p or not r or not p["median"] or not r["median"]:
            return "-"
        return f"{p['median'] / r['median']:.2f}" + ("" if p["right"] else " (wrong)")

    def table(title, kernels, cols, with_ratio=True, mean_key=None):
        o.append(f"#### {title}")
        o.append("")
        o.append("| kernel | " + " | ".join(short[l] for l in cols) + " |")
        o.append("|---" * (len(cols) + 1) + "|")
        for k in kernels:
            o.append(f"| {k} | " + " | ".join(cell(l, k) for l in cols) + " |")
        if with_ratio and "rosetta" in per:
            o.append("")
            o.append("Ratio to Rosetta 2:")
            o.append("")
            o.append("| kernel | " + " | ".join(short[l] for l in cols if l != "rosetta") + " |")
            o.append("|---" * (len([l for l in cols if l != "rosetta"]) + 1) + "|")
            for k in kernels:
                o.append(f"| {k} | " + " | ".join(ratio(l, k) for l in cols if l != "rosetta") + " |")
            if mean_key:
                row = []
                for l in cols:
                    if l == "rosetta":
                        continue
                    g = means[l].get(mean_key)
                    n = len(means[l].get("headline_kernels", [])) if mean_key == "headline" else 1
                    row.append(f"**{g:.2f}** ({n} kernels)" if g else "-")
                o.append("| **geometric mean** | " + " | ".join(row) + " |")
        o.append("")

    kernels_present = list(per[labels[0]].keys())
    head = [k for k in HEADLINE_KERNELS if k in kernels_present]
    if head:
        table("Headline: single-threaded integer, memory and floating-point kernels", head, labels, True, "headline")
    x87 = [k for k in X87_KERNELS if k in kernels_present]
    if x87:
        table("x87 (x86-64 builds only; native arm64 has no 80-bit type)", x87, [l for l in labels if l != "native-arm64"],
              True)
    thr = [k for k in THREAD_KERNELS if k in kernels_present]
    if thr:
        table("Thread kernels (in no mean; spread is large)", thr, labels, False)
    st = s["startup"]
    if st:
        o.append("#### Time to first kernel result (`--only calls --quick`), seconds")
        o.append("")
        cols = list(st)
        o.append("| | " + " | ".join(short[l] for l in cols) + " |")
        o.append("|---" * (len(cols) + 1) + "|")

        def f(x):
            return f"{x:.3f}" if x is not None else "-"

        o.append("| first launch of the fresh build | " + " | ".join(f(st[l]["first_launch"]) for l in cols) + " |")
        o.append("| later launches, median [range] | " + " | ".join(
            f"{f(st[l]['later_median'])} [{f(st[l]['later_min'])}-{f(st[l]['later_max'])}]" for l in cols) + " |")
        o.append("")
    tr = s.get("translation") or {}
    if tr:
        o.append("#### Translation and the code cache (per launch of the full program; median [range])")
        o.append("")
        o.append("Blocks compiled and loaded come from `fexmac_context_get_counters`. First result: launch to the end of "
                 "the first kernel's warm-up. Launch: the whole run, all kernels, every repetition.")
        o.append("")
        o.append("| configuration | blocks compiled | blocks loaded from the cache | first result, s | launch, s |")
        o.append("|---|---|---|---|---|")

        def fs(x, fmt):
            return "-" if not x else f"{x['median']:{fmt}} [{x['min']:{fmt}}-{x['max']:{fmt}}]"

        for l, v in tr.items():
            o.append(f"| {short[l]} | {fs(v['compiled'], '.0f')} | {fs(v['loaded'], '.0f')} | {fs(v['first_result_s'], '.3f')} | "
                     f"{fs(v['launch_s'], '.2f')} |")
        o.append("")
    o.append(DOC_END)
    return "\n".join(o) + "\n"


def replace_block(text, begin, end, content):
    if begin not in text or end not in text:
        return None
    a = text.index(begin)
    b = text.index(end) + len(end) + 1
    return text[:a] + content + text[b:]


def write_docs(md, sm):
    """Replaces the generated blocks: results and summary in the benchmarks document,
    the summary in the status document."""
    for path, blocks in ((DOC, ((DOC_BEGIN, DOC_END, md), (SUM_BEGIN, SUM_END, sm))), (STATUS_DOC, ((SUM_BEGIN, SUM_END, sm),))):
        with open(path) as f:
            text = f.read()
        for begin, end, content in blocks:
            new = replace_block(text, begin, end, content)
            if new is None:
                print(f"{path} has no {begin} ... {end} block; not changed")
                return
            text = new
        with open(path, "w") as f:
            f.write(text)
        print(f"updated {path}")


def fmt_ratio(x):
    return "-" if x is None else f"{x:.2f}"


def summary_markdown(meta, s, labels):
    """The figures the documents' prose may rely on, all from the results."""
    per, means, tr = s["per_kernel"], s["means"], s.get("translation", {})
    load = meta["load"]

    def kmed(label, k):
        p = per.get(label, {}).get(k)
        return p["median"] if p and p["median"] else None

    def ratio(label, k):
        a, b = kmed(label, k), kmed("rosetta", k)
        return a / b if a and b else None

    have = lambda c: f"fex:{c}" in per  # noqa: E731
    o = [SUM_BEGIN, "<!-- Generated by Scripts/macos_bench.py from docs/macOS-benchmarks-results.json; "
         "do not edit by hand. -->", ""]
    status = "**FINAL**" if meta["status"] == "FINAL" else "**PROVISIONAL**"
    o.append(f"{status} measurement of `{meta['git']}`, {meta['started']}: other processes kept "
             f"{load.get('other_cores_mean')} cores busy on average (at most {load.get('other_cores_max')}) of the "
             f"machine's 10; load average {load.get('loadavg_mean')}." +
             ("" if meta["status"] == "FINAL" else " Provisional numbers show the shape, not final values."))
    o.append("")
    names = [("tso_on", "all ordering on (`tso_on`), like for like with Rosetta 2's hardware TSO"),
             ("tso_scalar", "scalar ordering only (`tso_scalar`, FEX's defaults)"),
             ("tso_vector", "scalar and vector ordering (`tso_vector`)"),
             ("tso_memcpy", "scalar and string-instruction ordering (`tso_memcpy`)"),
             ("tso_on_nomb", "all on, multiblock off"), ("tso_on_x87rp", "all on, reduced x87 precision"),
             ("tso_on_cache_cold", "all on, code cache cold"), ("tso_on_cache_warm", "all on, code cache warm")]
    parts = [f"{what} **{fmt_ratio(means.get('fex:' + c, {}).get('headline'))}**" for c, what in names if have(c)]
    if "native-arm64" in means:
        parts.append(f"native arm64 {fmt_ratio(means['native-arm64'].get('headline'))}")
    if have("tso_off"):
        parts.append(f"ordering off {fmt_ratio(means['fex:tso_off'].get('headline'))} (an upper bound only: not correct "
                     "for multi-threaded guests)")
    n = len(means.get("fex:tso_on", {}).get("headline_kernels", []))
    o.append(f"- **Headline** ({n} single-threaded integer, memory and floating-point kernels), geometric mean of the "
             "time relative to Rosetta 2: " + "; ".join(parts) + ".")
    head = [k for k in HEADLINE_KERNELS if k in per.get("fex:tso_on", {})]
    if have("tso_on") and "rosetta" in per:
        worst = sorted((k for k in head if ratio("fex:tso_on", k)), key=lambda k: -ratio("fex:tso_on", k))[:6]
        items = []
        for k in worst:
            extra = []
            for c in ("tso_scalar", "tso_off"):
                if have(c):
                    extra.append(f"{c} {fmt_ratio(ratio('fex:' + c, k))}")
            items.append(f"`{k}` {fmt_ratio(ratio('fex:tso_on', k))} ({', '.join(extra)})")
        o.append("- **Slowest against Rosetta 2 with all ordering on** (ratio; in brackets the same kernel with scalar "
                 "ordering only and with ordering off): " + "; ".join(items) + ".")
    if have("tso_vector") and have("tso_scalar"):
        v = [(k, ratio("fex:tso_vector", k), ratio("fex:tso_scalar", k)) for k in head]
        v = [x for x in v if x[1] and x[2] and x[1] / x[2] >= 1.5]
        o.append("- **Vector ordering** (`tso_vector` against `tso_scalar`, ratios to Rosetta 2), kernels it slows by "
                 "half or more: " + ("; ".join(f"`{k}` {fmt_ratio(a)} against {fmt_ratio(b)}" for k, a, b in v) or "none") +
                 ".")
    if have("tso_memcpy") and have("tso_scalar"):
        v = [(k, ratio("fex:tso_memcpy", k), ratio("fex:tso_scalar", k)) for k in head]
        v = [x for x in v if x[1] and x[2] and x[1] / x[2] >= 1.5]
        o.append("- **String-instruction ordering** (`tso_memcpy` against `tso_scalar`), kernels it slows by half or more: " +
                 ("; ".join(f"`{k}` {fmt_ratio(a)} against {fmt_ratio(b)}" for k, a, b in v) or "none") + ".")
    if have("tso_on_nomb") and have("tso_on"):
        v = [(k, ratio("fex:tso_on_nomb", k), ratio("fex:tso_on", k)) for k in head]
        v = [x for x in v if x[1] and x[2] and x[1] / x[2] >= 1.5]
        o.append("- **Multiblock off** (`tso_on_nomb` against `tso_on`), kernels it slows by half or more: " +
                 ("; ".join(f"`{k}` {fmt_ratio(a)} against {fmt_ratio(b)}" for k, a, b in v) or "none") + ".")
    if "rosetta" in per and "x87" in per["rosetta"]:
        xs = [f"Rosetta 2 {kmed('rosetta', 'x87'):.0f} ms"]
        for c in ("tso_on", "tso_on_x87rp"):
            if have(c):
                xs.append(f"`{c}` {fmt_ratio(ratio('fex:' + c, 'x87'))} of Rosetta 2's time")
        o.append("- **x87** (x86-64 builds only): " + "; ".join(xs) + ".")
    thr = [k for k in THREAD_KERNELS if k in per.get("fex:tso_on", {})]
    if thr and "rosetta" in per:
        items = []
        for k in thr:
            p = per["fex:tso_on"][k]
            items.append(f"`{k}` {fmt_ratio(ratio('fex:tso_on', k))} (launch medians {p['min']:.1f} to {p['max']:.1f} ms; "
                         f"Rosetta 2 {per['rosetta'][k]['min']:.1f} to {per['rosetta'][k]['max']:.1f} ms)")
        o.append("- **Thread kernels**, all ordering on against Rosetta 2 (in no mean): " + "; ".join(items) + ".")
    if tr:
        def m(c, key, f=".0f"):
            x = tr.get("fex:" + c, {}).get(key)
            return "-" if not x else f"{x['median']:{f}}"
        items = [f"`tso_on` compiles {m('tso_on', 'compiled')} blocks per launch"]
        if have("tso_on_nomb"):
            items.append(f"{m('tso_on_nomb', 'compiled')} with multiblock off")
        if have("tso_on_cache_cold"):
            items.append(f"a cold-cache launch compiles {m('tso_on_cache_cold', 'compiled')} and loads "
                         f"{m('tso_on_cache_cold', 'loaded')}")
        if have("tso_on_cache_warm"):
            items.append(f"a warm-cache launch compiles {m('tso_on_cache_warm', 'compiled')} and loads "
                         f"{m('tso_on_cache_warm', 'loaded')}")
        firsts = [f"`{c}` {m(c, 'first_result_s', '.3f')} s" for c in ("tso_on", "tso_on_cache_cold", "tso_on_cache_warm")
                  if have(c)]
        launches = [f"`{c}` {m(c, 'launch_s', '.2f')} s" for c in ("tso_on", "tso_on_cache_cold", "tso_on_cache_warm")
                    if have(c)]
        o.append("- **Translation and the code cache**: " + "; ".join(items) + ". Launch to first kernel result: " +
                 ", ".join(firsts) + "; whole launch: " + ", ".join(launches) + ".")
    clock = s.get("clock_ns", {})
    if clock:
        o.append("- **Clock read** (calibrated by the program, subtracted from the times): " +
                 ", ".join(f"{l.replace('fex:', '')} {v:.0f} ns" for l, v in clock.items() if l in ("native-arm64", "rosetta",
                                                                                                    "fex:tso_on")) + ".")
    o.append(SUM_END)
    return "\n".join(o) + "\n"


# Figures that only a measurement can give: ratios ("2.1x", "3 times"), times with a
# unit, percentages and bare decimals (not dotted versions such as 27.0.1).
FIGURE_PATTERNS = [
    re.compile(r"(?<![\w.])\d+(?:\.\d+)?(?:x|×)(?![0-9A-Za-z_])"),
    re.compile(r"(?<![\w.])\d+(?:\.\d+)? times\b"),
    re.compile(r"(?<![\w.])\d+(?:\.\d+)?\s?(?:ms|µs|us|ns)\b"),
    re.compile(r"(?<![\w.])\d+\.\d+\s?s\b"),
    re.compile(r"(?<![\w.])\d+(?:\.\d+)?\s?%"),
    re.compile(r"(?<![\w.])\d+\.\d+(?![\w.])"),
]


def unblocked(text, blocks):
    for begin, end in blocks:
        while begin in text and end in text:
            a = text.index(begin)
            b = text.index(end) + len(end)
            text = text[:a] + text[b:]
    return text


def check_docs(json_path):
    """Fails if a generated block differs from what this results file gives, or if a
    measured figure appears outside the generated blocks (anywhere in the benchmarks
    document; in the status document's benchmark section and its summary bullet)."""
    with open(json_path) as f:
        data = json.load(f)
    meta, summary = data["meta"], data["summary"]
    labels = list(data["results"])
    md, sm = markdown(meta, summary, labels), summary_markdown(meta, summary, labels)
    bad = []
    with open(DOC) as f:
        doc = f.read()
    with open(STATUS_DOC) as f:
        status = f.read()
    for path, text, want in ((DOC, doc, ((DOC_BEGIN, DOC_END, md), (SUM_BEGIN, SUM_END, sm))),
                             (STATUS_DOC, status, ((SUM_BEGIN, SUM_END, sm),))):
        for begin, end, content in want:
            if begin not in text or end not in text:
                bad.append(f"{os.path.relpath(path, ROOT)}: no {begin} block")
                continue
            got = text[text.index(begin):text.index(end) + len(end) + 1]
            if got != content:
                bad.append(f"{os.path.relpath(path, ROOT)}: the {begin} block is not what {json_path} gives "
                           "(run the measurement with --write-docs)")
    regions = [(DOC, unblocked(doc, [(DOC_BEGIN, DOC_END), (SUM_BEGIN, SUM_END)]))]
    st = unblocked(status, [(SUM_BEGIN, SUM_END)])
    section = st[st.index("## Guest program and benchmark"):] if "## Guest program and benchmark" in st else ""
    section = section[:section.find("\n## ", 3)] if "\n## " in section[3:] else section
    bullet = st[st.index("- **Runs a real guest program"):] if "- **Runs a real guest program" in st else ""
    bullet = bullet[:bullet.find("\n- **", 3)]
    regions += [(STATUS_DOC, section), (STATUS_DOC, bullet)]
    for path, text in regions:
        in_code = False
        for line in text.splitlines():
            if line.startswith("```"):
                in_code = not in_code
            if in_code:
                continue
            for pat in FIGURE_PATTERNS:
                for mt in pat.finditer(line):
                    bad.append(f"{os.path.relpath(path, ROOT)}: figure outside the generated blocks: '{mt.group(0)}' in: "
                               f"{line.strip()[:100]}")
    for b in bad:
        print(b)
    print("docs check: " + ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
