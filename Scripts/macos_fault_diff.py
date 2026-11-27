#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Differential test of fexmac's guest fault delivery and write-protection SMC path.

For every case (a short x86-64 program with one instruction marked FAULT:) and every
multiblock/TSO combination it runs build/Bin/fexmac_fault_diff and compares:

  at-fault  the full guest state when the marked instruction faults on the test page
            against a reference run in which that instruction is replaced by hlt
            (x86: the state before the faulting instruction);
  resume    the state after the page is made accessible and the run resumed, against a
            clean run with the page accessible from the start;
  smc       the result with the test page holding translated code under SMCCHECKS mtrack
            (the marked instruction's write goes through the write-protection path)
            against SMCCHECKS full.

Every difference fails the run unless KNOWN lists it (case, check); a listed difference
that no longer occurs also fails, so the list cannot go stale. Written from the package D
review's harness.

    python3 Scripts/macos_fault_diff.py --tool build/Bin/fexmac_fault_diff [--filter text]
"""

import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile

# (name, source, protection of the test page in the fault run: 0 none, 1 read-only[,
# False: no resume or clean-run comparison, for programs whose clean run jumps to an
# address taken from the test data])
CASES = [
    ("adc-rmw-ro", "stc\nFAULT: adc qword ptr [r15], 0\nhlt", 1),
    ("add-rmw-ro-flags", "mov rax, -1\nFAULT: add qword ptr [r15], rax\npushfq\npop rbx\nhlt", 1),
    ("leave-stack-fault", "mov rbp, r15\nFAULT: leave\nhlt", 0),
    ("pop-mem-dst-fault", "push 77\nFAULT: pop qword ptr [r15]\nhlt", 1),
    ("std-then-fault", "std\nFAULT: mov rax, [r15]\nrep movsb\nhlt", 0),
    ("std-smc-pushf", "std\nFAULT: mov qword ptr [r15], 1\npushfq\npop rbx\ncld\nhlt", 1),
    ("mmx-then-fault", "movq mm0, rax\nFAULT: mov rbx, [r15]\nhlt", 0),
    ("ymm-then-fault", "vpcmpeqb ymm0, ymm0, ymm0\nFAULT: mov rbx, [r15]\nhlt", 0),
    ("ymm-then-store", "vpcmpeqb ymm0, ymm0, ymm0\nFAULT: mov qword ptr [r15], rbx\nhlt", 1),
    ("x87-then-fault", "fld1\nfldpi\nFAULT: mov rbx, [r15]\nhlt", 0),
    ("x87-then-store", "fld1\nfldpi\nFAULT: mov qword ptr [r15], rbx\nfstp qword ptr [rdi]\nhlt", 1),
    ("call-stack-ro", "lea rsp, [r15+0x100]\nFAULT: call 1f\n1: hlt", 1),
    ("push-stack-ro", "lea rsp, [r15+0x100]\nFAULT: push rax\nhlt", 1),
    ("push-mem-src-none", "FAULT: push qword ptr [r15]\nhlt", 0),
    ("ret-stack-none", "mov rsp, r15\nFAULT: ret\nhlt", 0, False),
    ("call-mem-target-none", "FAULT: call qword ptr [r15]\nhlt", 0, False),
    ("partial-al-ah", "mov ah, 0x55\nmov al, 0x66\nFAULT: mov bl, [r15]\nhlt", 0),
    ("zext32-then-fault", "mov eax, 0x55\nFAULT: mov ebx, [r15]\nhlt", 0),
    ("xchg-mem-ro", "FAULT: xchg [r15], rax\nhlt", 1),
    ("cmpxchg-mem-ro", "mov rax, [r15]\nFAULT: cmpxchg [r15], rbx\nhlt", 1),
    ("xadd-mem-ro", "FAULT: xadd [r15], rax\nhlt", 1),
    ("inc-mem-ro-cf", "stc\nFAULT: inc qword ptr [r15]\nhlt", 1),
    ("movsq-dst-ro", "mov rdi, r15\nFAULT: movsq\nhlt", 1),
    ("movsq-src-none", "mov rsi, r15\nFAULT: movsq\nhlt", 0),
    ("load-cross-none", "FAULT: mov rax, [r14]\nhlt", 0),
    ("store-cross-ro", "FAULT: mov [r14], rax\nhlt", 1),
    ("vstore-cross-ro", "FAULT: movups [r14], xmm1\nhlt", 1),
    ("ystore-cross-ro", "FAULT: vmovdqu [r14-12], ymm1\nhlt", 1),
    ("fstp-ro", "fld1\nFAULT: fstp qword ptr [r15]\nhlt", 1),
    ("fxsave-ro", "FAULT: fxsave [r15]\nhlt", 1),
    ("rep-stos-cross", "lea rdi, [r15-64]\nmov rcx, 128\nFAULT: rep stosb\nhlt", 1),
    ("rep-movs-overlap", "lea rsi, [r15-64]\nlea rdi, [r15-63]\nmov rcx, 128\nFAULT: rep movsb\nhlt", 1),
    ("rep-movs-overlap-down", "lea rdi, [r15-64]\nlea rsi, [r15-32]\nmov rcx, 128\nFAULT: rep movsb\nhlt", 1),
    ("rep-movsq-nonoverlap", "lea rdi, [r15-64]\nlea rsi, [r15-4096]\nmov rcx, 32\nFAULT: rep movsq\nhlt", 1),
    ("enter-ro", "lea rsp, [r15+0x100]\nFAULT: enter 16, 0\nhlt", 1),
    ("pushf-ro", "lea rsp, [r15+0x100]\nstc\nFAULT: pushfq\nhlt", 1),
    ("ldmxcsr-then-fault", "mov dword ptr [rdi], 0x3f80\nldmxcsr [rdi]\nFAULT: mov rax, [r15]\nhlt", 0),
    ("wrfsbase-then-fault", "mov rax, 0x12345000\nwrfsbase rax\nFAULT: mov rax, [r15]\nhlt", 0),
    ("flags-multiblock-edge", "add rax, rbx\njmp 1f\n1: FAULT: mov rcx, [r15]\nadc rdx, 0\nhlt", 0),
    ("cmp-then-load-then-jcc", "cmp rax, rbx\nFAULT: mov rcx, [r15]\njb 1f\nmov r8, 1\n1: hlt", 0),
    ("sub-carry-then-load-adc", "mov rax, 1\nsub rax, 2\nFAULT: mov rcx, [r15]\nadc rdx, 0\nhlt", 0),
    ("lock-add-cross-ro", "FAULT: lock add qword ptr [r14], 1\nhlt", 1),
    ("lock-cmpxchg16b-none", "FAULT: lock cmpxchg16b [r15]\nhlt", 0),
] + [
    ("carry-" + ins.split()[0] + "-" + str(i), "stc\nFAULT: " + ins + "\nhlt", 1)
    for i, ins in enumerate([
        "rcl qword ptr [r15], 1", "rcr qword ptr [r15], 3", "sbb qword ptr [r15], 1", "adc dword ptr [r15], 7",
        "rcl byte ptr [r15], 1", "inc qword ptr [r15]\nadc rax, 0", "neg qword ptr [r15]", "shl qword ptr [r15], cl\nadc rax,0",
        "btc qword ptr [r15], 3\nadc rax, 0", "xadd [r15], rax\nadc rdx, 0", "cmpxchg [r15], rbx\nadc rdx,0"])
]

MODES = [(0, 0), (1, 0), (0, 1), (1, 1)]
# The guest-fault comparisons run with each way of translating: SMCCHECKS full (the
# default; its per-instruction check also splits blocks, which can hide a missing flush)
# and none (no check, the code mtrack produces).
FAULT_SMC = ["full", "none"]

# Differences known not to be fixed, as (case, check). See docs/macOS-embedding-interface.md,
# "Guest memory faults", for what each is.
KNOWN_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Source", "Embedding", "tests", "fault_diff_known.txt")

SKIP = {"reason", "trap", "ia", "faddr", "access", "err", "sig", "code", "rip"}


def assemble(src):
    d = tempfile.mkdtemp()
    s, o = os.path.join(d, "t.s"), os.path.join(d, "t.o")
    with open(s, "w") as f:
        f.write(".intel_syntax noprefix\n.text\n" + src + "\n")
    subprocess.check_call(["clang", "-target", "x86_64-linux-gnu", "-c", s, "-o", o])
    b = open(o, "rb").read()
    shoff = struct.unpack_from("<Q", b, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * shentsize) for i in range(shnum)]
    strtab = secs[shstrndx]
    for sec in secs:
        if b[strtab[4] + sec[0]:].split(b"\0")[0] == b".text":
            path = os.path.join(d, "t.bin")
            with open(path, "wb") as f:
                f.write(b[sec[4]:sec[4] + sec[5]])
            return path
    raise RuntimeError("no .text")


def run(tool, path, mb, tso, prot, resume=False, env=None):
    e = dict(os.environ)
    e.update(env or {})
    args = ["perl", "-e", "alarm 30; exec @ARGV", tool, path, str(mb), str(tso), str(prot)] + (["resume"] if resume else [])
    p = subprocess.run(args, capture_output=True, text=True, env=e)
    out = {}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k] = v
    if p.returncode:
        out["_rc"] = str(p.returncode)
    return out


def diff(a, b, pa, pb):
    keys = set(k[2:] for k in a if k.startswith(pa + ".")) | set(k[2:] for k in b if k.startswith(pb + "."))
    return ["%s: got %s want %s" % (k, a.get(pa + "." + k), b.get(pb + "." + k)) for k in sorted(keys) if k not in SKIP and
            a.get(pa + "." + k) != b.get(pb + "." + k)]


def check_case(tool, name, src, prot, clean=True):
    fault = assemble(src.replace("FAULT:", ""))
    # The reference replaces the marked instruction (and nothing else on its line, such
    # as a label) by hlt.
    ref = assemble("\n".join((l.split("FAULT:")[0] + "hlt" if "FAULT:" in l else l) for l in src.splitlines()))
    found = {}
    for mb, tso, smc in [(mb, tso, smc) for mb, tso in MODES for smc in FAULT_SMC + ["smc-path"]]:
        tag = "mb=%d tso=%d smc=%s" % (mb, tso, smc)
        if smc == "smc-path":
            if not clean:
                continue
            w = run(tool, fault, mb, tso, 7, False, {"SMC": "mtrack", "WARM": "1"})
            g = run(tool, fault, mb, tso, 7, False, {"SMC": "full", "WARM": "1"})
            if "_rc" in w or "_rc" in g:
                found.setdefault("smc", []).append(tag + ": died")
            else:
                lines = diff(w, g, "a", "a")
                if lines:
                    found.setdefault("smc", []).append(tag + " (%s code writes): " % w.get("codewrites") + "; ".join(lines))
            continue
        nohash = {"NOHASHPAGE": "1", "SMC": smc}
        f = run(tool, fault, mb, tso, prot, clean, nohash)
        r = run(tool, ref, mb, tso, 3, False, nohash)
        c = run(tool, fault, mb, tso, 3, False, {"SMC": smc}) if clean else {}
        for what, out in (("fault run", f), ("reference run", r), ("clean run", c)):
            if "_rc" in out:
                found.setdefault("died", []).append(tag + " " + what)
        if "_rc" in f or "_rc" in r or "_rc" in c:
            continue
        if f.get("a.reason") != "3" or f.get("a.trap") != "14":
            found.setdefault("no-fault", []).append(tag + " reason " + str(f.get("a.reason")))
        checks = [("at-fault", diff(f, r, "a", "a"))]
        if clean:
            checks.append(("resume", diff(f, c, "b", "a")))
        for check, lines in checks:
            if lines:
                found.setdefault(check, []).append(tag + ": " + "; ".join(lines))
    return name, found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True)
    parser.add_argument("--filter", default="")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    known = set()
    if os.path.exists(KNOWN_FILE):
        for line in open(KNOWN_FILE):
            words = line.split("#", 1)[0].split()
            if len(words) >= 2:
                known.add((words[0], words[1]))
    cases = [c for c in CASES if args.filter in c[0]]
    failures = 0
    seen = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for name, found in pool.map(lambda c: check_case(args.tool, *c), cases):
            for check, lines in sorted(found.items()):
                seen.add((name, check))
                expected = (name, check) in known
                print("%s %s %s" % ("known     " if expected else "UNEXPECTED", name, check))
                if args.verbose or not expected:
                    for line in lines:
                        print("    " + line)
                failures += not expected
    names = set(c[0] for c in cases)
    for name, check in sorted(known):
        if name in names and (name, check) not in seen:
            print("FIXED (remove from %s): %s %s" % (os.path.basename(KNOWN_FILE), name, check))
            failures += 1
    print("%d cases x %d modes x %d ways; %d known differences; %d failures" % (len(cases), len(MODES), len(FAULT_SMC) + 1,
                                                                             len(seen & known), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
