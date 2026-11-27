#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks that each benchmark kernel's checksum covers what the kernel computes.

    python3 Scripts/macos_bench_mutations.py [--out DIR] [--no-rosetta]

For each mutation below, a copy of unittests/macOS/Bench/kernels.c is changed in one
place (a shorter string instruction, a skipped element, a wrong operation, a lost
update ...), built natively for arm64 (and for x86-64 under Rosetta 2 for the mutations
that only exist in the x86-64 code), and run in quick mode on the mutated kernel. The
kernel's checksum must differ from the recorded one: a mutation that leaves it unchanged
means the checksum does not see that part of the work, and fails this test. The
unchanged source must give the recorded checksums. Exit status 0 if every mutation is
caught.
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "unittests", "macOS", "Bench")
CFLAGS = ["-O2", "-std=c11", "-ffreestanding", "-ffp-contract=off", "-fno-stack-protector", "-fno-stack-check", "-w"]

# (kernel, what the mutation does, text in kernels.c (must occur once), replacement,
#  architectures to check it on)
ARM = ("arm64",)
X86 = ("x86_64",)
MUTATIONS = [
    ("crc32", "last byte of each pass not processed", "for (uint32_t i = 0; i < BYTES_MAX; ++i) {\n      crc =",
     "for (uint32_t i = 0; i < BYTES_MAX - 1; ++i) {\n      crc =", ARM),
    ("crc32", "polynomial off by one bit", "0xEDB88320u", "0xEDB88321u", ARM),
    ("hash64", "last 16-byte block skipped", "for (uint32_t i = 0; i < BYTES_MAX; i += 16) {",
     "for (uint32_t i = 0; i < BYTES_MAX - 16; i += 16) {", ARM),
    ("hash64", "high half of the 128-bit product dropped", "b = (uint64_t)(m >> 64) + (b << 5) + i;", "b = (b << 5) + i;", ARM),
    ("sort", "last key left out of the sort", "quick_sort(g_sort, n);", "quick_sort(g_sort, n - 1);", ARM),
    ("sort", "insertion sort stops one short", "for (long i = 1; i < n; ++i) {", "for (long i = 1; i < n - 1; ++i) {", ARM),
    ("interp", "xor executed as or", "case OP_XOR: r[in.a] = r[in.b] ^ r[in.c];", "case OP_XOR: r[in.a] = r[in.b] | r[in.c];", ARM),
    ("interp", "indirect call to the wrong function", "vm_funcs[r[in.b] & 3]", "vm_funcs[(r[in.b] + 1) & 3]", ARM),
    ("interp", "conditional branch never taken", "if (r[in.a]) {\n        pc = prog + in.imm;",
     "if (r[in.a] && in.imm != 10) {\n        pc = prog + in.imm;", ARM),
    ("dfa", "numbers counted twice", "++numbers;", "numbers += 2;", ARM),
    ("dfa", "'9' classified as a letter", "g_class[c] = CL_DIGIT;", "g_class[c] = c == '9' ? CL_ALPHA : CL_DIGIT;", ARM),
    ("dfa", "identifier length off by one", "++idents;\n          h = mix(h, len);", "++idents;\n          h = mix(h, len + 1);",
     ARM),
    ("rep_movs", "rep stosb one byte short", "fill_bytes(g_dst, (uint8_t)(t * 3 + 1), REG1 - t % 13);",
     "fill_bytes(g_dst, (uint8_t)(t * 3 + 1), REG1 - t % 13 - 1);", ARM),
    ("rep_movs", "rep movsb 600 bytes short", "REG1 - 128 - t % 61);", "REG1 - 128 - t % 61 - 600);", ARM),
    ("rep_movs", "rep movsb 1 byte short", "REG1 - 128 - t % 61);", "REG1 - 128 - t % 61 - 1);", ARM),
    ("rep_movs", "rep stosq one quadword short", "(REG2 - REG1) / 8 - t % 5);", "(REG2 - REG1) / 8 - t % 5 - 1);", ARM),
    ("rep_movs", "rep movsq 40 quadwords short", "(REG2 - REG1) / 8 - 4 - t % 7);", "(REG2 - REG1) / 8 - 4 - t % 7 - 40);",
     ARM),
    ("rep_movs", "rep movsq 1 quadword short", "(REG2 - REG1) / 8 - 4 - t % 7);", "(REG2 - REG1) / 8 - 4 - t % 7 - 1);",
     ARM),
    ("rep_movs", "rep movsl one doubleword short", "(REG3 - REG2) / 4 - 8 - t % 11);", "(REG3 - REG2) / 4 - 8 - t % 11 - 1);",
     ARM),
    ("rep_movs", "overlapping forward copy one byte further", "copy_bytes(g_dst + REG3 + 1 + t % 5",
     "copy_bytes(g_dst + REG3 + 2 + t % 5", ARM),
    ("rep_movs", "overlapping forward copy one byte short", "(REG4 - REG3) - 64 - t % 23);", "(REG4 - REG3) - 64 - t % 23 - 1);",
     ARM),
    ("rep_movs", "backward copy one byte short", "(DST_BYTES - REG4) / 2 - t % 31);", "(DST_BYTES - REG4) / 2 - t % 31 - 1);",
     ARM),
    ("rep_movs", "backward copy run forwards (arm64 loop)", "for (size_t i = n; i-- > 0;) {", "for (size_t i = 0; i < n; ++i) {",
     ARM),
    ("rep_movs", "rep stosq executed as rep stosl (x86-64)", '"rep stosq"', '"rep stosl"', X86),
    ("rep_movs", "rep movsl executed as rep movsw (x86-64)", '"rep movsl"', '"rep movsw"', X86),
    ("rep_movs", "direction flag not set for the backward copy (x86-64)", '"std\\n\\trep movsb\\n\\tcld"',
     '"rep movsb\\n\\tcld"', X86),
    ("copy_loop", "copy 64 bytes short", "const uint32_t n = COPY_BYTES / 8 - k % 3;", "const uint32_t n = COPY_BYTES / 8 - k % 3 - 8;",
     ARM),
    ("copy_loop", "copy one quadword short", "const uint32_t n = COPY_BYTES / 8 - k % 3;",
     "const uint32_t n = COPY_BYTES / 8 - k % 3 - 1;", ARM),
    ("copy_loop", "one element of each pass not xored", "d[i] = s[i] ^ key;", "d[i] = s[i] ^ (i == 1000 ? 0 : key);", ARM),
    ("ptr_chase", "last step not taken", "for (uint32_t i = 0; i < steps; ++i) {\n    sum +=",
     "for (uint32_t i = 0; i + 1 < steps; ++i) {\n    sum +=", ARM),
    ("ptr_chase", "one value misread", "sum += n->value ^ i;", "sum += n->value ^ (i == 500 ? 0 : i);", ARM),
    ("matmul_f64", "last term of each dot product left out", "for (int k = 0; k < MAT_N; ++k) {\n          acc +=",
     "for (int k = 0; k < MAT_N - 1; ++k) {\n          acc +=", ARM),
    ("matmul_f64", "one element wrong", "g_mc[i][j] = acc;", "g_mc[i][j] = (i == 5 && j == 7) ? acc + 1.0 : acc;", ARM),
    ("matmul_f32x4", "last row of B left out", "for (int k = 0; k < VMAT_N; ++k) {\n        const float a",
     "for (int k = 0; k < VMAT_N - 1; ++k) {\n        const float a", ARM),
    ("matmul_f32x4", "one lane initialised wrong", "(f32x4) {(float)r, (float)r, (float)r, (float)r}",
     "(f32x4) {(float)r, (float)r, (float)r, (float)r + 1.0f}", ARM),
    ("nbody", "last pair interaction left out", "for (int j = i + 1; j < BODIES; ++j) {",
     "for (int j = i + 1; j < BODIES - (i == 0); ++j) {", ARM),
    ("nbody", "wrong mass in one velocity component", "vz[j] -= dz * m[i] * inv;", "vz[j] -= dz * m[j] * inv;", ARM),
    ("nbody", "one position component not integrated at the last step", "pz[i] += dt * vz[i];",
     "pz[i] += (t == steps - 1 && i == 3) ? 0.0 : dt * vz[i];", ARM),
    ("fft", "last butterfly stage skipped", "for (int len = 2; len <= FFT_N; len <<= 1) {",
     "for (int len = 2; len < FFT_N; len <<= 1) {", ARM),
    ("fft", "imaginary part scaled wrong", "im[i] *= 1.0 / FFT_N;", "im[i] *= 2.0 / FFT_N;", ARM),
    ("x87", "product term changed", "prod *= 1.0L + 1.0L / (li + 3.0L);", "prod *= 1.0L + 1.0L / (li + 4.0L);", ARM + X86),
    ("x87", "zeta term changed", "zeta += 1.0L / (li * li);", "zeta += 1.0L / (li * li + 1.0L);", ARM + X86),
    ("muldiv", "candidates shifted by one", "const uint64_t n = start + 2 * i;", "const uint64_t n = start + 2 * i + 2;", ARM),
    ("muldiv", "wrong divisor in the remainder", "acc += a / b + a % b;", "acc += a / b + a % (b | 2);", ARM),
    ("muldiv", "small divisors replaced by large ones", "const uint64_t small = (r & 0xFFFF) | 1;",
     "const uint64_t small = (r & 0xFFFF) | (1ull << 40);", ARM),
    ("muldiv", "128-by-64 division with the high half dropped", "div128(r % d, a, d, &q, &rem);", "div128(0, a, d, &q, &rem);",
     ARM),
    ("muldiv", "128-by-64 quotient low bit lost", "acc = mix(acc, q) + rem;", "acc = mix(acc, q & ~1ull) + rem;", ARM),
    ("muldiv", "signed 128-by-64 division with a positive dividend only",
     "const __int128 n = (__int128)(int64_t)x * ((__int128)1 << 40) + 12345;",
     "const __int128 n = (__int128)(x >> 1) * ((__int128)1 << 40) + 12345;", ARM + X86),
    ("muldiv", "divq given a zero high half (x86-64)", '__asm__("divq %4" : "=a"(*q), "=d"(*r) : "a"(lo), "d"(hi), "rm"(d) : "cc");',
     '__asm__("divq %4" : "=a"(*q), "=d"(*r) : "a"(lo), "d"(0ull), "rm"(d) : "cc");', X86),
    ("muldiv", "modular inverse one Newton step short", "for (int i = 0; i < 6; ++i) {\n    inv *=",
     "for (int i = 0; i < 4; ++i) {\n    inv *=", ARM),
    ("calls", "base case wrong", "return n < 2 ? n : fib(n - 1) + fib(n - 2);", "return n < 2 ? n + (n == 1) : fib(n - 1) + fib(n - 2);",
     ARM),
    ("deep_recursion", "one level of the recursion off by one", "return mix(r, depth);",
     "return mix(r, depth + (depth == 1000));", ARM),
    ("deep_recursion", "one level shallower", "descend(50000, (uint64_t)r)", "descend(49999, (uint64_t)r)", ARM),
    ("threads_lock", "one lost update", "g_shared.counter += 1;", "g_shared.counter += (a->id == 2 && i == 7) ? 0 : 1;", ARM),
    ("threads_lock", "one sum update lost", "g_shared.sum += v;", "g_shared.sum += (a->id == 3 && i == 9) ? 0 : v;", ARM),
    ("threads_lock", "atomic add of the wrong value", "__atomic_fetch_add(&g_shared.atomic_sum, v >> 3, __ATOMIC_SEQ_CST);",
     "__atomic_fetch_add(&g_shared.atomic_sum, v >> 4, __ATOMIC_SEQ_CST);", ARM),
    ("threads_mp", "one message read wrong", "h = mix(h, r->data[t % RING]);", "h = mix(h, i == 100 ? 0 : r->data[t % RING]);", ARM),
    ("threads_mp", "one message sent wrong", "r->data[h % RING] = splitmix(&s);",
     "r->data[h % RING] = splitmix(&s) ^ (i == 77);", ARM),
]


def build(kernels_c, arch, out):
    target = ["-target", f"{arch}-apple-macos11"]
    obj = out + ".o"
    subprocess.run(["clang", *target, *CFLAGS, "-I", SRC, "-c", kernels_c, "-o", obj], check=True)
    subprocess.run(["clang", *target, "-O2", "-w", obj, os.path.join(SRC, "platform_native.c"), "-I", SRC, "-o", out], check=True)
    if arch == "x86_64":
        # Rosetta 2 hangs at the launch of an unsigned x86-64 program on macOS 27.
        subprocess.run(["codesign", "--force", "--sign", "-", out], check=True, capture_output=True)
    return out


def verdict(exe, arch, kernel):
    cmd = (["arch", "-x86_64"] if arch == "x86_64" else []) + [exe, "--quick", "--reps", "0", "--only", kernel]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    except subprocess.TimeoutExpired:
        return "TIMEOUT", ""
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "C" and f[1] == kernel:
            return f[2], f[3]
    return "NO-RESULT", ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=None)
    ap.add_argument("--no-rosetta", action="store_true")
    args = ap.parse_args()
    out = args.out or tempfile.mkdtemp(prefix="bench-mutations-")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(SRC, "kernels.c")) as f:
        original = f.read()
    archs = ["arm64"]
    if not args.no_rosetta and subprocess.run(["arch", "-x86_64", "/usr/bin/true"], capture_output=True).returncode == 0:
        archs.append("x86_64")
    else:
        print("Rosetta 2 not available: the x86-64-only mutations are skipped")

    failures = 0
    base = {}
    for arch in archs:
        exe = build(os.path.join(SRC, "kernels.c"), arch, os.path.join(out, f"base-{arch}"))
        for k in sorted({m[0] for m in MUTATIONS}):
            v, s = verdict(exe, arch, k)
            base[(arch, k)] = s
            if v != "ok":
                print(f"FAIL unchanged source, {arch}: {k} {v}")
                failures += 1
    for n, (kernel, what, old, new, on) in enumerate(MUTATIONS):
        if original.count(old) != 1:
            print(f"FAIL mutation {n} ({kernel}: {what}): text occurs {original.count(old)} times in kernels.c")
            failures += 1
            continue
        path = os.path.join(out, f"m{n}.c")
        with open(path, "w") as f:
            f.write(original.replace(old, new))
        for arch in on:
            if arch not in archs:
                print(f"skip {kernel}: {what} ({arch})")
                continue
            exe = build(path, arch, os.path.join(out, f"m{n}-{arch}"))
            v, s = verdict(exe, arch, kernel)
            caught = v in ("MISMATCH", "VARIES") or (v not in ("ok",) and s != base[(arch, kernel)])
            print(f"{'ok  ' if caught else 'FAIL'} {kernel:15} {arch:7} {what}: {v} {s}")
            failures += 0 if caught else 1
    print(f"{'PASS' if failures == 0 else 'FAIL'}: {len(MUTATIONS)} mutations, {failures} not caught")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
