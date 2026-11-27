#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assembles and runs FEX's 64-bit assembly instruction tests with FEXCoreASMTestRunner.

Upstream drives these tests through CMake and its Linux test runner. This script does the
same with the core-only runner so the tests can run on macOS.

    python3 Scripts/macos_asm_tests.py --runner build/Bin/FEXCoreASMTestRunner --out asm-out

Each test runs in every mode upstream uses for the translator. A test fails the run only
if it is not listed in an expected-failure file. A listed test that passes is reported so
the list can be trimmed.

With --cache-dir the on-disk code cache is switched on. --cache-passes 2 empties the
directory, runs everything once to fill it (cold) and again to load from it (warm). The
run then also fails if any test's warm result differs from its cold one, if a warm run
that finished compiled any block instead of loading it, or if any cache entry was
rejected. --cache-validate makes the warm pass compile as well and compare its code with
the cached code byte for byte.
"""

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time

MODES = {
    "jit_1": {"FEX_MAXINST": "1", "FEX_MULTIBLOCK": "0"},
    "jit_500": {"FEX_MAXINST": "500", "FEX_MULTIBLOCK": "0"},
    "jit_500_m": {"FEX_MAXINST": "500", "FEX_MULTIBLOCK": "1"},
    # Not an upstream mode. Upstream runs these tests with x86 memory-ordering emulation
    # off; this mode turns it on, including for vector and block-copy operations, so
    # that the emulation paths are exercised at all.
    "jit_500_tso": {"FEX_MAXINST": "500", "FEX_MULTIBLOCK": "1", "FEX_TSOENABLED": "1", "FEX_VECTORTSOENABLED": "1",
                    "FEX_MEMCPYSETTSOENABLED": "1"},
}
COMMON_ENV = {"FEX_SILENTLOG": "0", "FEX_DUMPGPRS": "1", "FEX_TSOENABLED": "0"}
SKIP_CODE = 125
CACHE_LINE = re.compile(r"^Code cache: compiled (\d+) loaded (\d+) rejected (\d+) queued (\d+)$", re.M)
VALIDATE_MISMATCH = re.compile(r"DiskCache: validate host (code|size) mismatch")

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


def run_one(runner, name, mode, binary, config, timeout, cache_env=None):
    """Returns (status, detail, cache) where cache is None or a dict of the runner's counters."""
    env = dict(os.environ)
    env.update(COMMON_ENV)
    env.update(MODES[mode])
    if "SelfModifyingCode" in name:
        env["FEX_SMCCHECKS"] = "full"
    if cache_env:
        env.update(cache_env)
    try:
        result = subprocess.run([runner, binary, config], capture_output=True, text=True, errors="replace", env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "timeout", "", None
    cache = None
    match = CACHE_LINE.search(result.stdout)
    if match:
        cache = dict(zip(("compiled", "loaded", "rejected", "queued"), map(int, match.groups())))
        cache["mismatches"] = len(VALIDATE_MISMATCH.findall(result.stdout + result.stderr))
    if result.returncode == 0:
        return "pass", "", cache
    if result.returncode == SKIP_CODE:
        return "skip", result.stdout.strip().splitlines()[-1][:160] if result.stdout.strip() else "", cache
    tail = [line for line in (result.stdout + result.stderr).strip().splitlines() if not line.startswith("Code cache:")][-6:]
    status = "crash(signal %d)" % -result.returncode if result.returncode < 0 else "fail"
    return status, " | ".join(line[:160] for line in tail), cache


def directory_bytes(path):
    total = 0
    for folder, _, files in os.walk(path):
        for file in files:
            try:
                total += os.path.getsize(os.path.join(folder, file))
            except OSError:
                pass
    return total


def run_pass(runner, tests, modes, jobs, timeout, cache_env):
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {}
        for mode in modes:
            for name, binary, config in tests:
                futures[pool.submit(run_one, runner, name, mode, binary, config, timeout, cache_env)] = (mode, name)
        for future in concurrent.futures.as_completed(futures):
            results[futures[future]] = future.result()
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--modes", default=",".join(MODES))
    parser.add_argument("--filter", default="", help="only run tests whose name contains this text")
    parser.add_argument("--cache-dir", default="", help="switch the on-disk code cache on, kept in this directory")
    parser.add_argument("--cache-passes", type=int, default=1, choices=(1, 2),
                        help="2: empty the cache directory, run cold, then run warm and compare")
    parser.add_argument("--cache-validate", action="store_true",
                        help="in the warm pass, also compile every block and compare it with the cached code")
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
    cache_env = None
    if args.cache_dir:
        cache_dir = os.path.abspath(args.cache_dir)
        if args.cache_passes == 2 and os.path.isdir(cache_dir):
            shutil.rmtree(cache_dir)
        os.makedirs(cache_dir, exist_ok=True)
        cache_env = {"FEX_DISKCACHE": "1", "FEX_DISKCACHEPATH": cache_dir}

    started = time.monotonic()
    results = run_pass(runner, tests, modes, args.jobs, args.timeout, cache_env)
    first_seconds = time.monotonic() - started
    cold = None
    if cache_env and args.cache_passes == 2:
        cold = results
        cold_bytes = directory_bytes(cache_dir)
        warm_env = dict(cache_env)
        if args.cache_validate:
            warm_env["FEX_DISKCACHEVALIDATION"] = "1"
        started = time.monotonic()
        results = run_pass(runner, tests, modes, args.jobs, args.timeout, warm_env)
        warm_seconds = time.monotonic() - started

    exit_code = 0
    with open(os.path.join(args.out, "results.tsv"), "w") as handle:
        for (mode, name), (status, detail, _) in sorted(results.items()):
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
            status, detail, _ = results[(mode, name)]
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

    if cold is not None:
        exit_code |= report_cache(cold, results, modes, tests, cache_dir, first_seconds, warm_seconds, cold_bytes,
                                  args.cache_validate, os.path.join(args.out, "cache.tsv"))
    elif cache_env:
        print("\nCode cache in %s: %d bytes after %.1f s" % (cache_dir, directory_bytes(cache_dir), first_seconds))
    return exit_code


def report_cache(cold, warm, modes, tests, cache_dir, cold_seconds, warm_seconds, cold_bytes, validated, tsv_path):
    """Compares a cold and a warm pass. Returns 1 if the cache changed a result or was not used."""
    exit_code = 0
    print("\n== code cache: cold %.1f s, warm %.1f s, %d bytes on disk after the cold pass, %d after the warm pass" %
          (cold_seconds, warm_seconds, cold_bytes, directory_bytes(cache_dir)))
    with open(tsv_path, "w") as handle:
        handle.write("mode\ttest\tcold\twarm\tcold_compiled\tcold_loaded\tcold_queued\twarm_compiled\twarm_loaded\twarm_rejected\t"
                     "mismatches\n")
        for mode in modes:
            changed, compiled, rejected, mismatched, unfinished = [], [], [], [], []
            total_cold = total_compiled = total_loaded = 0
            for name, _, _ in tests:
                cold_status, cold_detail, cold_cache = cold[(mode, name)]
                warm_status, warm_detail, warm_cache = warm[(mode, name)]
                cc = cold_cache or {}
                wc = warm_cache or {}
                handle.write("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" %
                             (mode, name, cold_status, warm_status, cc.get("compiled", ""), cc.get("loaded", ""), cc.get("queued", ""),
                              wc.get("compiled", ""),
                              wc.get("loaded", ""), wc.get("rejected", ""), wc.get("mismatches", "")))
                if cold_status != warm_status:
                    changed.append((name, cold_status, warm_status, warm_detail))
                if cold_status == "skip":
                    continue
                if cold_cache is None or warm_cache is None:
                    # The runner did not reach its end (crash or timeout), so it never
                    # saved its translations. Such a test is an expected failure.
                    unfinished.append(name)
                    continue
                total_cold += cc["compiled"]
                total_compiled += wc["compiled"]
                total_loaded += wc["loaded"]
                if wc["rejected"]:
                    rejected.append(name)
                if validated:
                    if wc["mismatches"]:
                        mismatched.append((name, wc["mismatches"]))
                elif wc["compiled"]:
                    compiled.append((name, wc["compiled"], wc["loaded"]))
            print("   %s: cold compiled %d blocks; warm compiled %d, loaded %d; results changed %d; warm runs that compiled %d; "
                  "rejected entries in %d; not finished %d" % (mode, total_cold, total_compiled, total_loaded, len(changed), len(compiled),
                                                             len(rejected), len(unfinished)))
            for name, before, after, detail in changed[:50]:
                print("   CHANGED %s: cold %s, warm %s %s" % (name, before, after, detail[:200]))
            for name, count, loaded in compiled[:50]:
                print("   COMPILED %s: %d blocks compiled, %d loaded" % (name, count, loaded))
            for name in rejected[:50]:
                print("   REJECTED %s" % name)
            for name, count in mismatched[:50]:
                print("   MISMATCH %s: %d blocks differ from a fresh compile" % (name, count))
            # Byte differences against a fresh compile are reported, not failed: upstream's
            # relocation of patchable immediates can legitimately encode the same low bits
            # differently (see docs/macOS-code-cache-design.md).
            if changed or compiled or rejected:
                exit_code = 1
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
