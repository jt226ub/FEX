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
# The four modes run by default. The modes after them are jit_500_tso with one of the round
# 2 vector-ordering experiments on (VECTORTSOSPLIT, TSOBARRIERMERGE); run them with --modes.
DEFAULT_MODES = list(MODES)
for _name, _extra in (("split", {"FEX_VECTORTSOSPLIT": "3"}), ("merge", {"FEX_TSOBARRIERMERGE": "1"}),
                      ("split_merge", {"FEX_VECTORTSOSPLIT": "3", "FEX_TSOBARRIERMERGE": "1"}),
                      ("split16", {"FEX_VECTORTSOSPLIT": "15"})):
    MODES["jit_500_tso_" + _name] = {**MODES["jit_500_tso"], **_extra}
# The self-modifying-code mode is stated: none for every test but the SelfModifyingCode
# ones (full, or mtrack with --smc), so results do not follow fexmac's default (which
# gives registered memory, as the runner's is, full checks).
COMMON_ENV = {"FEX_SILENTLOG": "0", "FEX_DUMPGPRS": "1", "FEX_TSOENABLED": "0", "FEX_SMCCHECKS": "none"}
if sys.platform == "darwin":
    # No Apple chip so far implements FEAT_RNG (the M4 has no hw.optional.arm.FEAT_RNG and
    # `mrs RNDR` raises SIGILL), so RDRAND and RDSEED exist for a guest on a Mac only
    # through FEX's software generator. Turn it on so their tests run that path.
    COMMON_ENV["FEX_SOFTWARERNG"] = "1"
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
# Where the loader places the test's code (before the move), as in HarnessHelpers.h.
CODE_START = 0x10000
# The scratch areas the loader always maps, with a margin below each because tests that
# walk downwards name addresses just under them.
DEFAULT_LOW_RANGES = [(0xDFF00000, 0xE0100000), (0xE7F00000, 0xE8010000)]
HEX_LITERAL = re.compile(r"0x[0-9a-fA-F_]+")
REGION_KEY = re.compile(r'"(0x[0-9a-fA-F_]+)"\s*:\s*"(0x[0-9a-fA-F]+|[0-9]+)"')
REGDATA_ENTRY = re.compile(r'("([A-Za-z0-9]+)"\s*:\s*")(0x[0-9a-fA-F_]+)(")')
# `mov r32, imm32` zero-extends, so for an address below 4GB it is the same as the 64-bit
# move, which can hold the moved address. Rewriting it does not change what the test does.
MOV32_LOW = re.compile(r"^(\s*mov\s+)(e[a-ds][xip]|e[sd]i|e[sb]p|r(?:8|9|1[0-5])d)(\s*,\s*)(0x[0-9a-fA-F_]+)", re.I | re.M)
REG32_TO_64 = {"eax": "rax", "ebx": "rbx", "ecx": "rcx", "edx": "rdx", "esi": "rsi", "edi": "rdi", "ebp": "rbp", "esp": "rsp"}

# Values a test uses as addresses that are not themselves inside a mapped region, so the
# general rule cannot see them. Each entry says why; the instruction under test is not
# changed.
EXTRA_LOW_ADDRESSES = {
    # The test reads [r14 + r15 * 8] with r14 = 0x1000_0000 and r15 = 0x2000_0000, so the
    # address is 0x1_1000_0000, a mapped region. Moving only the base moves the address
    # with the region; a transposed base and index then gives an unmapped address and the
    # test fails by faulting instead of by loading the decoy region's data.
    "Test_FEX_bugs/SIBScaleTranspose.asm": [0x10000000],
}


def low_ranges(text, name=None):
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
    for value in EXTRA_LOW_ADDRESSES.get(name, []):
        ranges.append((value, value + 1))
    return ranges


def relocate_low_memory(text, name=None):
    """Moves every hex constant that names low guest memory up by the relocation."""
    if not LOW_MEMORY_RELOCATION:
        return text
    ranges = low_ranges(text, name)

    def is_low(literal):
        try:
            value = int(literal.replace("_", ""), 16)
        except ValueError:
            return None
        return value if any(low <= value < high for low, high in ranges) else None

    def widen(match):
        if is_low(match.group(4)) is None:
            return match.group(0)
        register = match.group(2).lower()
        wide = REG32_TO_64.get(register, register[:-1])
        return match.group(1) + wide + match.group(3) + match.group(4)

    text = MOV32_LOW.sub(widen, text)

    def move(match):
        value = is_low(match.group(0))
        return match.group(0) if value is None else hex(value + LOW_MEMORY_RELOCATION)

    return HEX_LITERAL.sub(move, text)


def relocate_expected_addresses(original, relocated, assemble_map):
    """Moves expected register values that are addresses the literal rewrite cannot see.

    Two kinds, both found from the test itself:
    - A value within 64KB above the start of a mapped region: an address in that region
      whose low bits a 16-bit operation replaced (`o16 leave` keeps RBP's upper bits).
    - A value within 64KB of the code start in a register the test loads with a
      RIP-relative `lea`: an address in the test's own code. Moving the code also changes
      instruction lengths (a moved immediate needs a longer encoding), so a value that is
      a label's address is mapped through the label, read from the assembler's symbol map
      of the test before and after the move. Any other value there (low bits replaced by
      data) keeps its low bits and gets the moved code's upper bits.
    """
    header = relocated.split("%ifdef CONFIG", 1)
    if len(header) < 2 or '"RegData"' not in header[1]:
        return relocated
    head, rest = header[1].split('"RegData"', 1)
    block, tail = rest.split("}", 1)
    windows = [(low, (high + 0xFFFF) & ~0xFFFF) for low, high in low_ranges(original)]
    body = original.split("%endif", 1)[-1]
    maps = {}

    def code_value(register, value):
        if not re.search(r"\blea\s+%s\s*,\s*\[\s*rel\b" % register, body, re.I):
            return None
        if not maps:
            maps["before"], maps["after"] = assemble_map(original), assemble_map(relocated)
        offset = value - CODE_START
        for symbol, address in maps["before"].items():
            if address == offset and symbol in maps["after"]:
                return CODE_START + LOW_MEMORY_RELOCATION + maps["after"][symbol]
        return value + LOW_MEMORY_RELOCATION

    def fix(match):
        register, literal = match.group(2), match.group(3)
        value = int(literal.replace("_", ""), 16)
        moved = None
        if value < FOUR_GB * 16 and any(low <= value < high for low, high in windows):
            moved = value + LOW_MEMORY_RELOCATION
        elif CODE_START <= value < CODE_START + 0x10000:
            moved = code_value(register, value)
        if moved is None:
            return match.group(0)
        return match.group(1) + hex(moved) + match.group(4)

    block = REGDATA_ENTRY.sub(fix, block)
    return header[0] + "%ifdef CONFIG" + head + '"RegData"' + block + "}" + tail


def symbol_map(nasm_args, text, scratch):
    """Assembles text and returns {label: offset} from nasm's map file."""
    source, mapfile = scratch + "_MAP.asm", scratch + ".map"
    with open(source, "w") as handle:
        handle.write("[map symbols %s]\nBITS 64\n%s\nret\n" % (mapfile, text))
    subprocess.run(nasm_args + [source, "-o", scratch + "_MAP.bin"], capture_output=True, text=True)
    symbols = {}
    if os.path.exists(mapfile):
        with open(mapfile) as handle:
            for line in handle:
                parts = line.split()
                if len(parts) == 3 and all(c in "0123456789ABCDEFabcdef" for c in parts[0]):
                    symbols[parts[2]] = int(parts[0], 16)
    return symbols


def read_list(path):
    """Returns {test name: the comment above it} for one of the test list files."""
    names = {}
    comment = []
    if os.path.exists(path):
        with open(path) as handle:
            for line in handle:
                text = line.strip()
                if text.startswith("#"):
                    words = text.lstrip("#").strip()
                    if words and not words.startswith("---"):
                        comment.append(words)
                    continue
                if not text:
                    comment = []
                    continue
                names[text.split("#", 1)[0].split()[0]] = " ".join(comment)
    return names


FAULT_LINE = re.compile(r"^(?:fexmac: (SIG\w+): .*?|runner: unclaimed (SIG\w+)) at address (0x[0-9a-f]+)(?:, pc 0x[0-9a-f]+ in (host|translated) code)?")
STOP_LINE = re.compile(r"^Stopped: result \S+ reason (\d+) signal (-?\d+) trap (\d+) si_code (-?\d+) at (0x[0-9a-f]+) fault address (0x[0-9a-f]+)")
# fexmac_stop_reason values in the runner's "Stopped:" line.
STOP_KINDS = {"3": "guest-fault", "4": "unsupported-instruction"}


def read_outcomes(path):
    """Returns {test name: {"kind": ..., key: value}} from the words after a test's name.

    `crash fault=<address> signal=SIGSEGV|SIGBUS where=host|translated`: the process dies
    of a host fault at that address, as the runner reports it, with the pc in host code
    or in translated code. `fault=` is required: a bare `crash` would match any crash.
    `guest-fault` or `unsupported-instruction`, with `signal=<n> trap=<n> si_code=<n> at=<rip>`
    and optionally `fault=<address>`: the run stops with that fexmac stop reason (a guest
    exception, a page fault, or code the translator cannot translate) and the given details.
    """
    outcomes = {}
    if os.path.exists(path):
        with open(path) as handle:
            for number, line in enumerate(handle, 1):
                words = line.split("#", 1)[0].split()
                if len(words) >= 2:
                    spec = {"kind": words[1]}
                    for word in words[2:]:
                        key, _, value = word.partition("=")
                        spec[key] = value
                    if spec["kind"] not in ("crash", "guest-fault", "unsupported-instruction", "fail", "timeout"):
                        sys.exit("%s:%d: unknown outcome %s" % (path, number, spec["kind"]))
                    if spec["kind"] == "crash" and "fault" not in spec:
                        sys.exit("%s:%d: a crash entry needs fault=<address>" % (path, number))
                    outcomes[words[0]] = spec
    return outcomes


def observed_outcome(status, detail):
    """What a failed run did, in the terms read_outcomes uses."""
    lines = detail.split(" | ")
    seen = {"kind": "timeout" if status == "timeout" else "crash" if status.startswith("crash") else "fail"}
    for line in lines:
        match = FAULT_LINE.match(line)
        if match and "fault" not in seen:
            seen["signal"] = match.group(1) or match.group(2)
            seen["fault"] = hex(int(match.group(3), 16))
            if match.group(4):
                seen["where"] = match.group(4)
        match = STOP_LINE.match(line)
        if match:
            seen["kind"] = STOP_KINDS.get(match.group(1), "stop-reason-" + match.group(1))
            seen["signal"], seen["trap"], seen["si_code"] = match.group(2), match.group(3), match.group(4)
            seen["at"] = hex(int(match.group(5), 16))
            seen["fault"] = hex(int(match.group(6), 16))
    return seen


def outcome_mismatch(spec, seen):
    """Returns why a failure does not match its listed outcome, or None if it does."""
    for key, value in spec.items():
        actual = seen.get(key)
        if key in ("fault", "at") and actual is not None:
            value = hex(int(value, 16))
        if actual != value:
            return "listed as %s, but %s is %s" % (" ".join("%s=%s" % item for item in spec.items()), key, actual)
    return None


def prepare(root, asm_dir, source, out_dir):
    """Assembles one test and writes its config. Returns (bin, config) or an error string."""
    rel = os.path.relpath(source, asm_dir)
    base = os.path.join(out_dir, rel)
    os.makedirs(os.path.dirname(base), exist_ok=True)
    tmp, binary, config = base + "_TMP.asm", base + ".bin", base + ".config.bin"
    with open(source) as handle:
        text = handle.read()
    includes = os.path.join(root, "unittests", "ASM", "Includes")
    nasm_args = ["nasm", "-w+error=number-overflow", "-i", includes + "/", "-i", os.path.dirname(source) + "/"]
    relocated = relocate_low_memory(text, "Test_" + rel)
    if LOW_MEMORY_RELOCATION:
        # The code moves even when no constant does, so this applies to every test.
        relocated = relocate_expected_addresses(text, relocated, lambda t: symbol_map(nasm_args, t, base))
    with open(tmp, "w") as handle:
        handle.write("BITS 64\n" + relocated + "\nret\n")
    config_source = source
    if relocated != text:
        config_source = base + "_RELOCATED.asm"
        with open(config_source, "w") as handle:
            handle.write(relocated)
    # A moved address that no longer fits its operand must stop the build, not be cut short.
    result = subprocess.run(nasm_args + [tmp, "-o", binary], capture_output=True, text=True)
    if result.returncode != 0:
        if relocated != text and "overflow" in result.stderr:
            # Name the first line that cannot hold its moved address.
            first = result.stderr.strip().splitlines()[0]
            line = first.split(":")[1] if first.count(":") >= 2 else "?"
            try:
                code = relocated.splitlines()[int(line) - 2].strip()
            except (ValueError, IndexError):
                code = ""
            return ("lowmem", "line %s: %s (%s)" % (int(line) - 1 if line.isdigit() else line, code, first.split("error:", 1)[-1].strip()))
        return "nasm: " + (result.stderr.strip().splitlines()[-1][:200] if result.stderr.strip() else "nasm failed")
    result = subprocess.run(
        [sys.executable, os.path.join(root, "Scripts", "json_asm_config_parse.py"), config_source, config],
        capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(config):
        return "config: " + (result.stderr.strip().splitlines()[-1][:200] if result.stderr.strip() else "no config written")
    return binary, config


def run_one(runner, name, mode, binary, config, timeout, cache_env=None, smc="full"):
    """Returns (status, detail, cache) where cache is None or a dict of the runner's counters."""
    env = dict(os.environ)
    env.update(COMMON_ENV)
    env.update(MODES[mode])
    if "SelfModifyingCode" in name:
        # full: the translator checks the code before every instruction. mtrack: fexmac
        # write-protects pages it has translated code from (the runner registers the
        # test's memory) and discards that code when the guest writes them.
        env["FEX_SMCCHECKS"] = smc
    if cache_env:
        env.update(cache_env)
    try:
        result = subprocess.run([runner, binary, config], capture_output=True, text=True, errors="replace", env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "timeout", "no exit within %d seconds" % timeout, None
    cache = None
    match = CACHE_LINE.search(result.stdout)
    if match:
        cache = dict(zip(("compiled", "loaded", "rejected", "queued"), map(int, match.groups())))
        cache["mismatches"] = len(VALIDATE_MISMATCH.findall(result.stdout + result.stderr))
        # The cache's own messages (a database that could not be opened, an entry that
        # could not be written), to explain a warm run that had to compile.
        cache["log"] = [line[:200] for line in (result.stdout + result.stderr).splitlines() if "DiskCache:" in line and "features" not in line][:3]
    if result.returncode == 0:
        return "pass", "", cache
    if result.returncode == SKIP_CODE:
        return "skip", result.stdout.strip().splitlines()[-1][:160] if result.stdout.strip() else "", cache
    lines = [line for line in (result.stdout + result.stderr).strip().splitlines() if not line.startswith("Code cache:")]
    tail = lines[-6:]
    status = "crash(signal %d)" % -result.returncode if result.returncode < 0 else "fail"
    # Lead with the lines that say what happened: the fault report (from the runner, or
    # from the fexmac library when no earlier handler is installed) and the runner's stop
    # line. The expected-outcome check reads them, and results.tsv keeps them.
    key = [line for line in lines if FAULT_LINE.match(line) or STOP_LINE.match(line)][-2:]
    tail = key + [line for line in tail if line not in key]
    return status, " | ".join(line[:200] for line in tail), cache


def directory_bytes(path):
    total = 0
    for folder, _, files in os.walk(path):
        for file in files:
            try:
                total += os.path.getsize(os.path.join(folder, file))
            except OSError:
                pass
    return total


def run_pass(runner, tests, modes, jobs, timeout, cache_env, smc="full"):
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {}
        for mode in modes:
            for name, binary, config in tests:
                futures[pool.submit(run_one, runner, name, mode, binary, config, timeout, cache_env, smc)] = (mode, name)
        for future in concurrent.futures.as_completed(futures):
            results[futures[future]] = future.result()
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--modes", default=",".join(DEFAULT_MODES), help="default: " + ",".join(DEFAULT_MODES) +
                        "; also: " + ",".join(m for m in MODES if m not in DEFAULT_MODES))
    parser.add_argument("--filter", default="", help="only run tests whose name contains this text (several: separate with commas)")
    parser.add_argument("--cache-dir", default="", help="switch the on-disk code cache on, kept in this directory")
    parser.add_argument("--cache-passes", type=int, default=1, choices=(1, 2),
                        help="2: empty the cache directory, run cold, then run warm and compare")
    parser.add_argument("--cache-validate", action="store_true",
                        help="in the warm pass, also compile every block and compare it with the cached code")
    parser.add_argument("--smc", choices=["full", "mtrack"], default="full",
                        help="how the SelfModifyingCode tests detect changed code: checks in every block (full) "
                             "or write protection of translated pages (mtrack)")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    asm_dir = os.path.join(root, "unittests", "ASM")
    runner = os.path.abspath(args.runner)
    os.makedirs(args.out, exist_ok=True)

    # Upstream's lists, as it applies them to the translator on an ARMv8.4-class host,
    # plus this port's own list.
    disabled = {}
    for name in ("Disabled_Tests", "Disabled_Tests_jit", "Disabled_Tests_ARMv8.4"):
        for test, why in read_list(os.path.join(asm_dir, name)).items():
            disabled[test] = "%s: %s" % (name, why or "no reason given")
    expected = {}
    for name in ("Known_Failures", "Known_Failures_jit", "Known_Failures_macOS"):
        for test, why in read_list(os.path.join(asm_dir, name)).items():
            expected[test] = "%s: %s" % (name, why or "no reason given")
    # How each listed test is expected to fail. Only this port's list says; an entry there
    # with an outcome also overrides upstream's lists for the same test.
    outcomes = read_outcomes(os.path.join(asm_dir, "Known_Failures_macOS"))

    # Upstream's tests, then the port's own (unittests/macOS/ASM, outside unittests/ASM so
    # that upstream's CMake, which collects every .asm under unittests/ASM, never turns
    # them into Linux tests). A port test is named Test_macOS/<path>.
    port_dir = os.path.join(root, "unittests", "macOS", "ASM")
    sources = []
    for top, prefix in ((asm_dir, ""), (port_dir, "macOS/")):
        found = []
        for folder, _, files in os.walk(top):
            if os.path.basename(folder) == "Includes":
                continue
            for file in sorted(files):
                if file.endswith(".asm"):
                    found.append(os.path.join(folder, file))
        sources += [(source, top, prefix) for source in sorted(found)]

    tests = []
    prepare_errors = []
    low_memory = []
    not_run = []
    for source, top, prefix in sources:
        name = "Test_" + prefix + os.path.relpath(source, top)
        if args.filter and not any(part in name for part in args.filter.split(",") if part):
            continue
        if name in disabled:
            not_run.append((name, "skip(upstream-list)", disabled[name]))
            continue
        prepared = prepare(root, top, source, os.path.join(args.out, prefix) if prefix else args.out)
        if isinstance(prepared, tuple) and prepared[0] == "lowmem":
            low_memory.append((name, prepared[1]))
            not_run.append((name, "skip(low-memory)", "needs memory below 4GB through a 32-bit operand: " + prepared[1]))
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
        # The runner turns these into fexmac's code_cache_enabled / code_cache_directory.
        cache_env = {"FEX_DISKCACHE": "1", "FEX_DISKCACHEPATH": cache_dir}

    started = time.monotonic()
    results = run_pass(runner, tests, modes, args.jobs, args.timeout, cache_env, args.smc)
    first_seconds = time.monotonic() - started
    cold = None
    if cache_env and args.cache_passes == 2:
        cold = results
        cold_bytes = directory_bytes(cache_dir)
        warm_env = dict(cache_env)
        if args.cache_validate:
            warm_env["FEX_DISKCACHEVALIDATION"] = "1"
        started = time.monotonic()
        results = run_pass(runner, tests, modes, args.jobs, args.timeout, warm_env, args.smc)
        warm_seconds = time.monotonic() - started
    full_results = results
    results = {key: value[:2] for key, value in full_results.items()}

    exit_code = 0
    # Every test appears in every mode, including the ones that were not run, each with
    # the reason. An expected failure carries the reason from its list.
    rows = dict(results)
    for mode in modes:
        for name, status, detail in not_run:
            rows[(mode, name)] = (status, detail)
    with open(os.path.join(args.out, "results.tsv"), "w") as handle:
        for (mode, name), (status, detail) in sorted(rows.items()):
            if status not in ("pass",) and not status.startswith("skip") and name in expected:
                why = outcome_mismatch(outcomes[name], observed_outcome(status, detail)) if name in outcomes else None
                if why:
                    status = "unexpected-" + status
                    detail = "%s [%s]" % (detail, why)
                else:
                    status = "expected-" + status
                    detail = "%s [%s]" % (detail, expected[name])
            handle.write("%s\t%s\t%s\t%s\n" % (mode, name, status, detail))

    print("Tests found: %d   disabled by upstream lists: %d   could not be prepared: %d" %
          (len(sources), len([s for s, t, p in sources if "Test_" + p + os.path.relpath(s, t) in disabled]), len(prepare_errors)))
    for name, status, detail in not_run:
        if status == "skip(upstream-list)":
            print("  DISABLED %s (%s)" % (name, detail))
    print("Skipped because they need memory below 4GB through a 32-bit operand: %d" % len(low_memory))
    for name, why in low_memory:
        print("  LOW MEMORY %s: %s" % (name, why))
    with open(os.path.join(args.out, "skipped-low-memory.txt"), "w") as handle:
        handle.write("".join("%s\t%s\n" % item for item in low_memory))
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
            elif failed and name in outcomes:
                why = outcome_mismatch(outcomes[name], observed_outcome(status, detail))
                if why:
                    unexpected.append((name, status, "%s [%s]" % (why, detail)))
            if not failed and status == "pass" and name in expected:
                fixed.append(name)
        print("\n== %s: %s" % (mode, "  ".join("%s %d" % item for item in sorted(counts.items()))))
        print("   unexpected failures: %d   expected failures that now pass: %d" % (len(unexpected), len(fixed)))
        skipped = sorted((name, results[(mode, name)][1]) for name, _, _ in tests if results[(mode, name)][0] == "skip")
        for name, detail in skipped:
            print("   SKIP %s: %s" % (name, detail))
        for name, status, detail in unexpected[:400]:
            print("   UNEXPECTED %s [%s] %s" % (name, status, detail[:300]))
        for name in fixed[:50]:
            print("   NOW PASSES %s" % name)
        if unexpected:
            exit_code = 1

    if cold is not None:
        exit_code |= report_cache(cold, full_results, modes, tests, cache_dir, first_seconds, warm_seconds, cold_bytes,
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
                elif wc["compiled"] and cold_status == "pass":
                    # A test that stops at code the translator cannot translate (an
                    # expected failure) compiles that block every time: it cannot be cached.
                    compiled.append((name, wc["compiled"], wc["loaded"], cc.get("log", []) + wc.get("log", [])))
            print("   %s: cold compiled %d blocks; warm compiled %d, loaded %d; results changed %d; warm runs that compiled %d; "
                  "rejected entries in %d; not finished %d" % (mode, total_cold, total_compiled, total_loaded, len(changed), len(compiled),
                                                             len(rejected), len(unfinished)))
            for name, before, after, detail in changed[:50]:
                print("   CHANGED %s: cold %s, warm %s %s" % (name, before, after, detail[:200]))
            for name, count, loaded, log in compiled[:50]:
                print("   COMPILED %s: %d blocks compiled, %d loaded %s" % (name, count, loaded, " | ".join(log)))
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
