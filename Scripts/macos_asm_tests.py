#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assembles and runs FEX's 64-bit assembly instruction tests with FEXCoreASMTestRunner.

Upstream drives these tests through CMake and its Linux test runner. This script does the
same with the core-only runner so the tests can run on macOS.

    python3 Scripts/macos_asm_tests.py --runner build/Bin/FEXCoreASMTestRunner --out asm-out

Each test runs in every mode upstream uses for the translator. A test fails the run only
if it is not listed in an expected-failure file. A listed test that passes is reported so
the list can be trimmed.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

MODES = {
    "jit_1": {"FEX_MAXINST": "1", "FEX_MULTIBLOCK": "0"},
    "jit_500": {"FEX_MAXINST": "500", "FEX_MULTIBLOCK": "0"},
    "jit_500_m": {"FEX_MAXINST": "500", "FEX_MULTIBLOCK": "1"},
}
COMMON_ENV = {"FEX_SILENTLOG": "0", "FEX_DUMPGPRS": "1", "FEX_TSOENABLED": "0"}
SKIP_CODE = 125

# macOS on Apple silicon reserves the first 4GB of every 64-bit process, so guest memory
# cannot live there. The tests use fixed scratch addresses below 4GB. On macOS each test
# is rewritten so those addresses move up by this amount, matching LowMemoryRelocation in
# HarnessHelpers.h. A test that uses such an address through a 32-bit operand cannot be
# moved and is reported as skipped.
LOW_MEMORY_RELOCATION = 0x10000000000 if sys.platform == "darwin" else 0
FOUR_GB = 0x100000000
# The scratch areas the loader always maps, with a margin below each because tests that
# walk downwards name addresses just under them.
DEFAULT_LOW_RANGES = [(0xDFF00000, 0xE0100000), (0xE7F00000, 0xE8010000)]
HEX_LITERAL = re.compile(r"0x[0-9a-fA-F_]+")
REGION_KEY = re.compile(r'"(0x[0-9a-fA-F_]+)"\s*:\s*"(0x[0-9a-fA-F]+|[0-9]+)"')


def relocate_low_memory(text):
    """Moves every hex constant that names low guest memory up by the relocation."""
    if not LOW_MEMORY_RELOCATION:
        return text
    ranges = list(DEFAULT_LOW_RANGES)
    header = text.split("%ifdef CONFIG", 1)
    if len(header) > 1 and '"MemoryRegions"' in header[1]:
        block = header[1].split('"MemoryRegions"', 1)[1].split("}", 1)[0]
        for key, size in REGION_KEY.findall(block):
            start = int(key.replace("_", ""), 16)
            length = int(size, 0)
            # Also covers the few gigabytes just above 4GB, where macOS loads the program.
            if start < 0x1000000000:
                ranges.append((start, start + max(length, 0x4000)))

    def move(match):
        literal = match.group(0)
        try:
            value = int(literal.replace("_", ""), 16)
        except ValueError:
            return literal
        if any(low <= value < high for low, high in ranges):
            return hex(value + LOW_MEMORY_RELOCATION)
        return literal

    return HEX_LITERAL.sub(move, text)


def read_list(path):
    names = set()
    if os.path.exists(path):
        with open(path) as handle:
            for line in handle:
                line = line.split("#", 1)[0].strip()
                if line:
                    names.add(line)
    return names


def prepare(root, asm_dir, source, out_dir):
    """Assembles one test and writes its config. Returns (bin, config) or an error string."""
    rel = os.path.relpath(source, asm_dir)
    base = os.path.join(out_dir, rel)
    os.makedirs(os.path.dirname(base), exist_ok=True)
    tmp, binary, config = base + "_TMP.asm", base + ".bin", base + ".config.bin"
    with open(source) as handle:
        text = handle.read()
    relocated = relocate_low_memory(text)
    with open(tmp, "w") as handle:
        handle.write("BITS 64\n" + relocated + "\nret\n")
    config_source = source
    if relocated != text:
        config_source = base + "_RELOCATED.asm"
        with open(config_source, "w") as handle:
            handle.write(relocated)
    # A moved address that no longer fits its operand must stop the build, not be cut short.
    result = subprocess.run(
        ["nasm", "-w+error=number-overflow", "-i", os.path.join(asm_dir, "Includes") + "/", "-i", os.path.dirname(source) + "/", tmp, "-o", binary],
        capture_output=True, text=True)
    if result.returncode != 0:
        if relocated != text and "overflow" in result.stderr:
            return "lowmem"
        return "nasm: " + (result.stderr.strip().splitlines()[-1][:200] if result.stderr.strip() else "nasm failed")
    result = subprocess.run(
        [sys.executable, os.path.join(root, "Scripts", "json_asm_config_parse.py"), config_source, config],
        capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(config):
        return "config: " + (result.stderr.strip().splitlines()[-1][:200] if result.stderr.strip() else "no config written")
    return binary, config


def run_one(runner, name, mode, binary, config, timeout):
    env = dict(os.environ)
    env.update(COMMON_ENV)
    env.update(MODES[mode])
    if "SelfModifyingCode" in name:
        env["FEX_SMCCHECKS"] = "full"
    try:
        result = subprocess.run([runner, binary, config], capture_output=True, text=True, errors="replace", env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "timeout", ""
    if result.returncode == 0:
        return "pass", ""
    if result.returncode == SKIP_CODE:
        return "skip", result.stdout.strip().splitlines()[-1][:160] if result.stdout.strip() else ""
    tail = (result.stdout + result.stderr).strip().splitlines()[-6:]
    status = "crash(signal %d)" % -result.returncode if result.returncode < 0 else "fail"
    return status, " | ".join(line[:160] for line in tail)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--modes", default=",".join(MODES))
    parser.add_argument("--filter", default="", help="only run tests whose name contains this text")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    asm_dir = os.path.join(root, "unittests", "ASM")
    runner = os.path.abspath(args.runner)
    os.makedirs(args.out, exist_ok=True)

    # Upstream's lists, as it applies them to the translator on an ARMv8.4-class host,
    # plus this port's own list.
    disabled = set()
    for name in ("Disabled_Tests", "Disabled_Tests_jit", "Disabled_Tests_ARMv8.4"):
        disabled |= read_list(os.path.join(asm_dir, name))
    expected = set()
    for name in ("Known_Failures", "Known_Failures_jit", "Known_Failures_macOS"):
        expected |= read_list(os.path.join(asm_dir, name))

    sources = []
    for folder, _, files in os.walk(asm_dir):
        if os.path.basename(folder) == "Includes":
            continue
        for file in sorted(files):
            if file.endswith(".asm"):
                sources.append(os.path.join(folder, file))
    sources.sort()

    tests = []
    prepare_errors = []
    low_memory = []
    for source in sources:
        name = "Test_" + os.path.relpath(source, asm_dir)
        if args.filter and args.filter not in name:
            continue
        if name in disabled:
            continue
        prepared = prepare(root, asm_dir, source, args.out)
        if prepared == "lowmem":
            low_memory.append(name)
            continue
        if isinstance(prepared, str):
            prepare_errors.append((name, prepared))
            continue
        tests.append((name, prepared[0], prepared[1]))

    modes = [mode for mode in args.modes.split(",") if mode in MODES]
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {}
        for mode in modes:
            for name, binary, config in tests:
                futures[pool.submit(run_one, runner, name, mode, binary, config, args.timeout)] = (mode, name)
        for future in concurrent.futures.as_completed(futures):
            results[futures[future]] = future.result()

    exit_code = 0
    with open(os.path.join(args.out, "results.tsv"), "w") as handle:
        for (mode, name), (status, detail) in sorted(results.items()):
            handle.write("%s\t%s\t%s\t%s\n" % (mode, name, status, detail))

    print("Tests found: %d   disabled by upstream lists: %d   could not be prepared: %d" %
          (len(sources), len([s for s in sources if "Test_" + os.path.relpath(s, asm_dir) in disabled]), len(prepare_errors)))
    print("Skipped because they need memory below 4GB through a 32-bit operand: %d" % len(low_memory))
    with open(os.path.join(args.out, "skipped-low-memory.txt"), "w") as handle:
        handle.write("\n".join(low_memory) + "\n")
    for name, error in prepare_errors[:20]:
        print("  PREPARE %s: %s" % (name, error))

    for mode in modes:
        counts = {}
        unexpected, fixed = [], []
        for name, _, _ in tests:
            status, detail = results[(mode, name)]
            key = status.split("(")[0]
            counts[key] = counts.get(key, 0) + 1
            failed = status not in ("pass", "skip")
            if failed and name not in expected:
                unexpected.append((name, status, detail))
            if not failed and status == "pass" and name in expected:
                fixed.append(name)
        print("\n== %s: %s" % (mode, "  ".join("%s %d" % item for item in sorted(counts.items()))))
        print("   unexpected failures: %d   expected failures that now pass: %d" % (len(unexpected), len(fixed)))
        for name, status, detail in unexpected[:400]:
            print("   UNEXPECTED %s [%s] %s" % (name, status, detail[:300]))
        for name in fixed[:50]:
            print("   NOW PASSES %s" % name)
        if unexpected:
            exit_code = 1
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
