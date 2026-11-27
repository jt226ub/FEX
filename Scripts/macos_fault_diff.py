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
import re
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

# rep string instructions report their progress at a fault, as x86 does: the at-fault
# reference is the same program run with rcx set to the elements that must be done
# before the fault (their memory effect included), and rcx set to the rest afterwards.
# (name, setup, instruction, element size, elements done, elements in all, protection)
def rep_case(name, setup, ins, done, total, prot=1):
    fault = setup + "\nmov rcx, %d\nFAULT: %s\nhlt" % (total, ins)
    ref = setup + "\nmov rcx, %d\n%s\nmov rcx, %d\nhlt" % (done, ins, total - done)
    return (name, fault, prot, True, ref)

REP_CASES = []
for ins, size in (("rep movsb", 1), ("rep movsw", 2), ("rep movsd", 4), ("rep movsq", 8), ("rep stosb", 1), ("rep stosw", 2),
                  ("rep stosd", 4), ("rep stosq", 8)):
    for k in (1, 8, 31, 32, 33, 64, 65):
        done = k // size
        # Forward into the read-only test page from below: the elements wholly below it.
        REP_CASES.append(rep_case("%s-fwd-%d" % (ins.replace(" ", "-"), k), "lea rdi, [r15-%d]\nlea rsi, [rdi-0x2000]" % k, ins, done, 128))
        # Backward (DF=1) into it from above.
        REP_CASES.append(rep_case("%s-back-%d" % (ins.replace(" ", "-"), k),
                                  "std\nlea rdi, [r15+0x4000+%d]\nlea rsi, [rdi+0x2000]" % (k - size), ins, done, 128))
# Overlapping copies (memmove patterns), forward and backward, element by element.
for d in (1, 3, 8, 31):
    REP_CASES.append(rep_case("rep-movsb-overlap-ahead-%d" % d, "lea rdi, [r15-40]\nlea rsi, [rdi-%d]" % d, "rep movsb", 40, 128))
    REP_CASES.append(rep_case("rep-movsb-overlap-behind-%d" % d, "lea rdi, [r15-40]\nlea rsi, [rdi+%d]" % d, "rep movsb", 40, 128))
    REP_CASES.append(rep_case("rep-movsb-back-overlap-%d" % d, "std\nlea rdi, [r15+0x4000+39]\nlea rsi, [rdi+%d]" % d, "rep movsb", 40,
                              128))
# The source runs into the inaccessible page part-way through a 32-byte chunk while the
# destination is 32-byte aligned (the chunked path): progress is still x86's.
for ins, size in (("rep movsb", 1), ("rep movsd", 4), ("rep movsq", 8)):
    for k in (8, 20, 24, 40, 1000, 4000):
        if k % size:
            continue
        REP_CASES.append(rep_case("%s-src-mid-%d" % (ins.replace(" ", "-"), k), "lea rsi, [r15-%d]\nlea rdi, [r15-0x3100]" % k, ins,
                                  k // size, k // size + 128, 0))
        REP_CASES.append(rep_case("%s-src-back-mid-%d" % (ins.replace(" ", "-"), k),
                                  "std\nlea rsi, [r15+0x4000+%d]\nlea rdi, [r15+0x6100-%d]" % (k - size, size), ins, k // size,
                                  k // size + 128, 0))
# The source page inaccessible instead.
REP_CASES.append(rep_case("rep-movsq-src-none", "lea rsi, [r15-64]\nlea rdi, [rsi-0x2000]", "rep movsq", 8, 32, 0))

# Read-modify-write and stack forms (undo records): every ALU, shift, rotate and bit-test
# form on memory at each size, with CF live afterwards and the flags pushed.
RMW_CASES = []
for ins in ["add {s} [r15], 3", "or {s} [r15], rax", "adc {s} [r15], 7", "sbb {s} [r15], 1", "and {s} [r15], 0x55", "sub {s} [r15], rcx",
            "xor {s} [r15], 1", "inc {s} [r15]", "dec {s} [r15]", "neg {s} [r15]", "not {s} [r15]", "rol {s} [r15], 1", "ror {s} [r15], 3",
            "rcl {s} [r15], 1", "rcr {s} [r15], 2", "shl {s} [r15], cl", "shr {s} [r15], 1", "sar {s} [r15], 3", "bts {s} [r15], 3",
            "btr {s} [r15], 1", "btc {s} [r15], 2", "lock add {s} [r15], 1", "lock sbb {s} [r15], 1"]:
    for size in ("byte ptr", "word ptr", "dword ptr", "qword ptr"):
        if ins.startswith("bt") and size == "byte ptr":
            continue
        text = ins.format(s=size).replace(", rax", ", " + {"byte ptr": "al", "word ptr": "ax", "dword ptr": "eax", "qword ptr": "rax"}[size])
        text = text.replace(", rcx", ", " + {"byte ptr": "cl", "word ptr": "cx", "dword ptr": "ecx", "qword ptr": "rcx"}[size])
        RMW_CASES.append(("rmw-" + text.replace(" ptr", "").replace(" ", "-").replace(",", "").replace("[", "").replace("]", ""),
                          "stc\nFAULT: " + text + "\nadc rbx, 0\npushfq\npop rdx\nhlt", 1))
for ins in ["shld qword ptr [r15], rax, 3", "shrd dword ptr [r15], eax, cl", "xadd qword ptr [r15], rbx", "xadd word ptr [r15], bx",
            "cmpxchg dword ptr [r15], ebx", "xchg qword ptr [r15], rax", "pop qword ptr [r15]", "pop word ptr [r15]"]:
    RMW_CASES.append(("rmw-" + ins.split()[0] + "-" + ins.split()[1], "stc\npush 77\nFAULT: " + ins + "\nadc rbx, 0\npushfq\npop rdx\nhlt", 1))
RMW_CASES.append(("leave-rbp-none", "lea rbp, [r15+8]\nFAULT: leave\nhlt", 0))
RMW_CASES += [
    ("pop-rsp-based-dst", "lea rsp, [r15-16]\nFAULT: pop qword ptr [rsp+8]\nhlt", 1),
    ("pop-sib-dst", "lea rsp, [r15-16]\nlea rax, [r15-8]\nFAULT: pop qword ptr [rax+rax*1+8]\nhlt", 1),
    ("pop-word-mem", "FAULT: pop word ptr [r15]\nhlt", 1),
    ("leave-word", "lea rbp, [r15+8]\nFAULT: data16 leave\nhlt", 0),
]
# Multi-part stores across a page boundary.
RMW_CASES += [
    ("ystore-cross-low-ro", "FAULT: vmovdqu [r15+0x4000-16], ymm1\nhlt", 1),
    ("fxsave-cross-ro", "FAULT: fxsave [r15-256]\nhlt", 1),
    ("xsave-like-movdqu-pair", "FAULT: movdqu [r15-8], xmm2\nhlt", 1),
    ("enter-cross-ro", "lea rsp, [r15+8]\nlea rbp, [r15+0x200]\nFAULT: enter 32, 2\nhlt", 1),
    ("cmpxchg16b-cross-ro", "lea rdi, [r15-8]\nFAULT: lock cmpxchg16b [r15]\nhlt", 1),
    ("push-cross-ro", "lea rsp, [r15+4]\nFAULT: push rax\nhlt", 1),
    ("call-cross-ro", "lea rsp, [r15+4]\nFAULT: call 1f\n1: hlt", 1),
    ("fstp-tbyte-cross-ro", "fld1\nFAULT: fstp tbyte ptr [r15-4]\nhlt", 1),
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


# Cases whose marked instruction overwrites every status flag without reading one, and
# computes them after its store (add/or/and/sub/xor/neg on memory, also locked): the
# incoming status flags are dead there, so a block that does not read them does not
# compute them, and at the fault they are as stale as any dead flag (the documented
# dead-flags limit). For these the at-fault comparison leaves out the status flags
# (CF, PF, AF, ZF, SF, OF); everything else, and the resumed result, is compared in full.
# With SMCCHECKS full every instruction ends a block, so the flags are computed there and
# the at-fault comparison is complete: it shows the reorder leaves them untouched.
DEAD_STATUS_FLAGS_IN = re.compile(r"^(rmw-(add|or|and|sub|xor|neg|lock-add)-|carry-neg-)")
STATUS_FLAGS = 0x8D5


def without_status_flags(out, tag):
    out = dict(out)
    key = tag + ".rflags"
    if key in out:
        out[key] = "%x" % (int(out[key], 16) & ~STATUS_FLAGS)
    return out


def check_case(tool, name, src, prot, clean=True, ref_src=None):
    fault = assemble(src.replace("FAULT:", ""))
    # The reference replaces the marked instruction (and nothing else on its line, such
    # as a label) by hlt, unless the case gives its own.
    ref = assemble(ref_src) if ref_src else assemble("\n".join((l.split("FAULT:")[0] + "hlt" if "FAULT:" in l else l)
                                                               for l in src.splitlines()))
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
        if DEAD_STATUS_FLAGS_IN.match(name) and smc != "full":
            checks = [("at-fault", diff(without_status_flags(f, "a"), without_status_flags(r, "a"), "a", "a"))]
        else:
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
    cases = [c for c in CASES + REP_CASES + RMW_CASES if args.filter in c[0]]
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
