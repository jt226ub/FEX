#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests the on-disk code cache on macOS: damaged files, mismatched keys, moving guest
code, several threads, and the directory rules.

    python3 Scripts/macos_code_cache_tests.py --runner build/Bin/FEXCoreASMTestRunner \\
        --cache-test build/Bin/FEXCoreCacheTest --work cache-tests

The warm-run check over the whole instruction suite is separate:
    python3 Scripts/macos_asm_tests.py ... --cache-dir DIR --cache-passes 2

Every case must end with the guest program giving the right answer and the process
exiting normally. A damaged or mismatched cache may only make the core compile instead
of load; it must never crash, hang or change a result.
"""

import argparse
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import time

CACHE_LINE = re.compile(r"^Code cache: compiled (\d+) loaded (\d+) rejected (\d+) queued (\d+)$", re.M)
TIMEOUT = 60

# On-disk layout (see FEXCore/Source/Interface/Core/DiskCache.{h,cpp}).
FOZ_MAGIC = 16
FOZ_KEY = 40
FOZ_HEADER = 16  # payload_size, format, crc, uncompressed_size
MESA_ENTRY = 28  # hash u64, size u32, last_access u64, offset u64 (packed)
INDEX_EXTRA = 32  # guest hash 16, footprint 8, guest size 4, extents count 4
BLOB_HEADER = 52  # GuestSize, HostSize, EntryPointCount, SmallRelocCount, ThunkRelocCount, GuestHash[16], IntegrityHash[16]

BASE_ENV = {"FEX_SILENTLOG": "0", "FEX_TSOENABLED": "0", "FEX_MAXINST": "500", "FEX_MULTIBLOCK": "0"}

failures = []
passes = 0


def check(name, condition, detail=""):
    global passes
    if condition:
        passes += 1
        print("  ok   %s" % name)
    else:
        failures.append(name)
        print("  FAIL %s %s" % (name, detail))


def run(cmd, cache_dir, extra_env=None):
    env = dict(os.environ)
    env.update(BASE_ENV)
    if cache_dir is not None:
        env.update({"FEX_DISKCACHE": "1", "FEX_DISKCACHEPATH": cache_dir})
    if extra_env:
        env.update(extra_env)
    started = time.monotonic()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=env, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return {"rc": "timeout", "out": "", "seconds": TIMEOUT}
    out = result.stdout + result.stderr
    info = {"rc": result.returncode, "out": out, "seconds": time.monotonic() - started}
    match = CACHE_LINE.search(result.stdout)
    if match:
        info.update(zip(("compiled", "loaded", "rejected", "queued"), map(int, match.groups())))
    return info


def summary(info):
    return "rc=%s compiled=%s loaded=%s rejected=%s queued=%s" % (info["rc"], info.get("compiled"), info.get("loaded"),
                                                                 info.get("rejected"), info.get("queued"))


def db_files(cache_dir):
    """Returns [(cache.foz, index.foz)] for every database under cache_dir."""
    pairs = []
    for folder, _, files in os.walk(cache_dir):
        for file in sorted(files):
            if file.endswith("_idx.foz"):
                pairs.append((os.path.join(folder, file[:-len("_idx.foz")] + ".foz"), os.path.join(folder, file)))
    return pairs


def parse_index(index_path):
    """Returns the index records: dicts with the record offset in the index file, and the
    offset and size of the blob in the cache file it points to."""
    data = open(index_path, "rb").read()
    records = []
    offset = FOZ_MAGIC
    while offset + FOZ_KEY + FOZ_HEADER <= len(data):
        payload = struct.unpack_from("<I", data, offset + FOZ_KEY)[0]
        if offset + FOZ_KEY + FOZ_HEADER + payload > len(data):
            break
        body = offset + FOZ_KEY + FOZ_HEADER
        _, size, _, blob = struct.unpack_from("<QIQQ", data, body)
        records.append({"record": offset, "body": body, "blob": blob, "size": size, "metadata": data[offset + FOZ_KEY - 1] == 0xFF})
        offset = body + payload
    return records


def flip(path, offset, bit=0):
    with open(path, "r+b") as handle:
        handle.seek(offset)
        byte = handle.read(1)
        handle.seek(offset)
        handle.write(bytes([byte[0] ^ (1 << bit)]))


def poke32(path, offset, value):
    with open(path, "r+b") as handle:
        handle.seek(offset)
        handle.write(struct.pack("<I", value))


def truncate(path, size):
    with open(path, "r+b") as handle:
        handle.truncate(size)


def fresh_copy(source, work, name):
    target = os.path.join(work, name)
    if os.path.exists(target):
        shutil.rmtree(target)
    shutil.copytree(source, target)
    return target


def load_asm_helper(root):
    spec = importlib.util.spec_from_file_location("macos_asm_tests", os.path.join(root, "Scripts", "macos_asm_tests.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--cache-test", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--test", default="TwoByte/0F_B0_3.asm", help="instruction test used for the file-damage cases")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    work = os.path.abspath(args.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    runner = os.path.abspath(args.runner)
    cache_test = os.path.abspath(args.cache_test)

    asm = load_asm_helper(root)
    asm_dir = os.path.join(root, "unittests", "ASM")
    prepared = asm.prepare(root, asm_dir, os.path.join(asm_dir, args.test), os.path.join(work, "asm"))
    if isinstance(prepared, str):
        print("could not prepare %s: %s" % (args.test, prepared))
        return 1
    test_cmd = [runner, prepared[0], prepared[1]]

    # 1. A cold run fills the cache and a warm run loads all of it.
    print("== baseline")
    pristine = os.path.join(work, "pristine")
    cold = run(test_cmd, pristine)
    check("cold run passes and queues entries", cold["rc"] == 0 and cold.get("queued", 0) > 0, summary(cold))
    warm = run(test_cmd, pristine)
    check("warm run passes, compiles nothing, loads everything", warm["rc"] == 0 and warm.get("compiled") == 0 and warm.get("loaded", 0) > 0,
          summary(warm))
    total_blocks = warm.get("loaded", 0)
    (cache_file, index_file), = db_files(pristine)
    records = parse_index(index_file)
    data_records = [r for r in records if not r["metadata"]]
    metadata = [r for r in records if r["metadata"]]
    check("index parses: one description and the data entries", len(metadata) >= 1 and len(data_records) > 0,
          "%d records" % len(records))

    # 2. Damaged files. "fallback" cases must make the core compile at least one block;
    # every case must still pass.
    print("== damaged cache files")
    cache_rel = os.path.relpath(cache_file, pristine)
    index_rel = os.path.relpath(index_file, pristine)
    blob = data_records[0]
    header = open(cache_file, "rb").read()[blob["blob"]:blob["blob"] + BLOB_HEADER]
    guest_size, host_size, entry_count, small_count, thunk_count = struct.unpack_from("<5I", header)
    code_start = blob["blob"] + BLOB_HEADER
    entries_start = code_start + host_size
    relocs_start = entries_start + entry_count * 12
    guest_copy_start = relocs_start + small_count * 14 + thunk_count * 37
    cases = [
        ("zero-length cache file", lambda c, i: truncate(c, 0), "fallback"),
        ("zero-length index file", lambda c, i: truncate(i, 0), "fallback"),
        ("both files zero length", lambda c, i: (truncate(c, 0), truncate(i, 0)), "fallback"),
        ("cache file cut to its magic", lambda c, i: truncate(c, FOZ_MAGIC), "fallback"),
        ("cache file cut in half", lambda c, i: truncate(c, os.path.getsize(c) // 2), "fallback"),
        ("cache file cut inside the first entry's code", lambda c, i: truncate(c, code_start + host_size // 2), "fallback"),
        ("index cut inside its last record", lambda c, i: truncate(i, os.path.getsize(i) - 5), "fallback"),
        ("index cut inside the first data record", lambda c, i: truncate(i, blob["record"] + 30), "fallback"),
        ("cache file magic flipped", lambda c, i: flip(c, 3), "fallback"),
        ("index magic flipped", lambda c, i: flip(i, 5), "fallback"),
        ("description blob flipped", lambda c, i: flip(c, metadata[0]["blob"] + 1), "fallback"),
        ("blob GuestSize flipped", lambda c, i: flip(c, blob["blob"] + 0, 3), "fallback"),
        ("blob HostSize flipped", lambda c, i: flip(c, blob["blob"] + 4, 4), "fallback"),
        ("blob EntryPointCount flipped", lambda c, i: flip(c, blob["blob"] + 8, 0), "fallback"),
        ("blob SmallRelocCount flipped", lambda c, i: flip(c, blob["blob"] + 12, 0), "fallback"),
        ("blob ThunkRelocCount flipped", lambda c, i: flip(c, blob["blob"] + 16, 0), "fallback"),
        ("blob guest hash flipped", lambda c, i: flip(c, blob["blob"] + 20, 5), "fallback"),
        ("blob integrity hash flipped", lambda c, i: flip(c, blob["blob"] + 40, 1), "fallback"),
        ("host code first instruction flipped", lambda c, i: flip(c, code_start, 0), "fallback"),
        ("host code middle flipped", lambda c, i: flip(c, code_start + (host_size // 2 & ~3), 7), "fallback"),
        ("host code last byte flipped", lambda c, i: flip(c, code_start + host_size - 1, 2), "fallback"),
        ("entry point guest offset flipped", lambda c, i: flip(c, entries_start, 2), "fallback"),
        ("entry point host offset flipped", lambda c, i: flip(c, entries_start + entry_count * 8, 4), "fallback"),
        ("HostSize set to 0xFFFFFFF0", lambda c, i: poke32(c, blob["blob"] + 4, 0xFFFFFFF0), "fallback"),
        ("EntryPointCount set to 0xFFFFFFFF", lambda c, i: poke32(c, blob["blob"] + 8, 0xFFFFFFFF), "fallback"),
        ("SmallRelocCount set to 0x10000000", lambda c, i: poke32(c, blob["blob"] + 12, 0x10000000), "fallback"),
        ("index payload size set huge", lambda c, i: poke32(i, blob["record"] + FOZ_KEY, 0x7FFFFFFF), "fallback"),
        ("index entry size set huge", lambda c, i: poke32(i, blob["body"] + 8, 0xFFFFFFFF), "fallback"),
        ("index entry offset moved by 4", lambda c, i: poke32(i, blob["body"] + 20, blob["blob"] + 4), "fallback"),
        ("index lookup key flipped", lambda c, i: flip(i, blob["body"] + 0, 0), "fallback"),
        ("index guest hash flipped", lambda c, i: flip(i, blob["body"] + MESA_ENTRY + 3, 0), "fallback"),
        ("index guest size flipped", lambda c, i: flip(i, blob["body"] + MESA_ENTRY + 24, 1), "fallback"),
        ("index extent count set huge", lambda c, i: poke32(i, blob["body"] + MESA_ENTRY + 28, 0x40000000), "fallback"),
        # Not covered by the integrity check, by design: the record key is only a name, and
        # the guest code copy is only read for validation diagnostics.
        ("index record key flipped (unused)", lambda c, i: flip(i, blob["record"] + 3, 0), "any"),
        ("guest code copy flipped (unused)", lambda c, i: flip(c, guest_copy_start, 0), "any"),
    ]
    if small_count:
        cases.append(("relocation type flipped", lambda c, i: flip(c, relocs_start + 4, 3), "fallback"))
        cases.append(("relocation offset flipped", lambda c, i: flip(c, relocs_start + 1, 7), "fallback"))
    for index, (name, damage, expect) in enumerate(cases):
        target = fresh_copy(pristine, work, "damage-%02d" % index)
        damage(os.path.join(target, cache_rel), os.path.join(target, index_rel))
        first = run(test_cmd, target)
        ok = first["rc"] == 0 and "compiled" in first
        if expect == "fallback":
            ok = ok and first["compiled"] > 0 and first["compiled"] + first["loaded"] >= total_blocks
        # Whatever state the damage left, the next run must also be right.
        second = run(test_cmd, target)
        ok = ok and second["rc"] == 0
        check(name, ok, "first: %s; second: %s" % (summary(first), summary(second)))

    # A damaged description empties the database and starts it again.
    target = fresh_copy(pristine, work, "heal")
    flip(os.path.join(target, cache_rel), metadata[0]["blob"] + 1)
    first, second, third = run(test_cmd, target), run(test_cmd, target), run(test_cmd, target)
    check("a damaged database is started again and fills up", first.get("rejected", 0) >= 1 and second.get("compiled") == 0 and
          second.get("loaded", 0) > 0 and third["rc"] == 0, "%s / %s" % (summary(first), summary(second)))

    # 3. Mismatched keys: a cache made under one configuration is never used under another.
    print("== mismatched configuration")
    for name, env in [
        ("TSO emulation on", {"FEX_TSOENABLED": "1"}),
        ("TSO on, vector TSO off", {"FEX_TSOENABLED": "1", "FEX_VECTORTSOENABLED": "0"}),
        ("TSO on, memcpy TSO on", {"FEX_TSOENABLED": "1", "FEX_MEMCPYSETTSOENABLED": "1"}),
        ("max instructions 1", {"FEX_MAXINST": "1"}),
        ("multiblock on", {"FEX_MULTIBLOCK": "1"}),
        ("full self-modifying-code checks", {"FEX_SMCCHECKS": "full"}),
        ("x87 reduced precision", {"FEX_X87REDUCEDPRECISION": "1"}),
    ]:
        # Within one run the core may load blocks it stored itself moments earlier, so
        # "not used" means: exactly what the same run does with an empty cache.
        target = fresh_copy(pristine, work, "config-" + re.sub(r"\W+", "-", name))
        info = run(test_cmd, target, env)
        empty = run(test_cmd, os.path.join(work, "empty-" + re.sub(r"\W+", "-", name)), env)
        same = all(info.get(k) == empty.get(k) for k in ("compiled", "loaded", "queued"))
        check("cache not used with %s" % name, info["rc"] == 0 and same and info.get("compiled", 0) > 0,
              "%s; with an empty cache: %s" % (summary(info), summary(empty)))

    # The same database copied under the name another configuration uses.
    target = os.path.join(work, "transplant")
    run(test_cmd, target, {"FEX_TSOENABLED": "1"})
    (other_cache, other_index), = db_files(target)
    shutil.copyfile(cache_file, other_cache)
    shutil.copyfile(index_file, other_index)
    info = run(test_cmd, target, {"FEX_TSOENABLED": "1"})
    check("a database copied under another configuration's name is rejected",
          info["rc"] == 0 and info.get("loaded") == 0 and info.get("rejected", 0) >= 1, summary(info))

    # A different format version or build recorded in the database's description.
    description_offset = metadata[0]["blob"]
    for name, offset, value in [("format version", 0, 48), ("build identity (Mach-O UUID)", 2 + 8 + 8 + 4 + 20 + 3, None)]:
        target = fresh_copy(pristine, work, "version-" + re.sub(r"\W+", "-", name))
        path = os.path.join(target, cache_rel)
        if value is None:
            flip(path, description_offset + offset, 0)
        else:
            with open(path, "r+b") as handle:
                handle.seek(description_offset + offset)
                handle.write(struct.pack("<H", value))
        info = run(test_cmd, target)
        check("a database with a different %s is rejected" % name,
              info["rc"] == 0 and info.get("loaded") == 0 and info.get("rejected", 0) >= 1, summary(info))

    # 4. Guest code: different bytes at the same address, and the same code at another address.
    print("== guest code identity and relocation")
    gdir = os.path.join(work, "guest")
    base = "0x8000000000"
    info = run([cache_test, "--threads", "1", "--base", base], gdir)
    check("cache test cold at %s" % base, info["rc"] == 0, summary(info))
    info = run([cache_test, "--threads", "1", "--base", base, "--variant", "sub"], gdir)
    check("changed guest bytes at the same address are compiled, with the right result",
          info["rc"] == 0 and info.get("compiled", 0) > 0, summary(info))
    info = run([cache_test, "--threads", "1", "--base", base, "--expect", "warm"], gdir)
    check("original bytes load again", info["rc"] == 0, summary(info))
    info = run([cache_test, "--threads", "1", "--base", "0x8100000000", "--expect", "warm"], gdir)
    check("the same code 4GB higher loads from the cache, relocated", info["rc"] == 0, summary(info))
    info = run([cache_test, "--threads", "1", "--expect", "warm"], gdir)
    check("the same code wherever the system maps it loads from the cache", info["rc"] == 0, summary(info))
    for mode, env in [("multiblock", {"FEX_MULTIBLOCK": "1"}), ("TSO", {"FEX_TSOENABLED": "1", "FEX_MULTIBLOCK": "1"})]:
        cold = run([cache_test, "--threads", "1", "--base", base], gdir, env)
        warm = run([cache_test, "--threads", "1", "--base", "0x8200000000", "--expect", "warm"], gdir, env)
        check("relocated load in %s mode" % mode, cold["rc"] == 0 and warm["rc"] == 0, "%s / %s" % (summary(cold), summary(warm)))

    # 5. Several threads loading the same cached blocks at the same moment.
    print("== threads")
    tdir = os.path.join(work, "threads")
    info = run([cache_test, "--threads", "8"], tdir)
    check("8 threads, cold", info["rc"] == 0, summary(info))
    bad = []
    for attempt in range(20):
        info = run([cache_test, "--threads", "8", "--expect", "warm"], tdir)
        if info["rc"] != 0:
            bad.append(summary(info) + " " + info["out"][-300:])
    check("8 threads, warm, 20 runs", not bad, "; ".join(bad[:2]))
    info = run([cache_test, "--threads", "8"], tdir, {"FEX_MULTIBLOCK": "1"})
    warm = run([cache_test, "--threads", "8", "--expect", "warm"], tdir, {"FEX_MULTIBLOCK": "1"})
    check("8 threads, multiblock: its own database, cold then warm", info["rc"] == 0 and warm["rc"] == 0,
          "%s / %s" % (summary(info), summary(warm)))

    # 6. Directory and file rules.
    print("== directory and file rules")
    target = fresh_copy(pristine, work, "shared-dir")
    os.chmod(target, 0o777)
    info = run(test_cmd, target)
    check("a cache directory writable by other users is not used",
          info["rc"] == 0 and info.get("loaded") == 0 and info.get("queued") == 0, summary(info))
    os.chmod(target, 0o755)
    target = fresh_copy(pristine, work, "symlink")
    path = os.path.join(target, cache_rel)
    os.rename(path, path + ".real")
    os.symlink(path + ".real", path)
    info = run(test_cmd, target)
    check("a symbolic link in place of a cache file is not used", info["rc"] == 0 and info.get("loaded") == 0, summary(info))
    target = fresh_copy(pristine, work, "permissions")
    bucket = os.path.dirname(os.path.join(target, cache_rel))
    os.chmod(bucket, 0o755)
    os.chmod(os.path.join(target, cache_rel), 0o666)
    info = run(test_cmd, target)
    modes = (os.stat(bucket).st_mode & 0o777, os.stat(os.path.join(target, cache_rel)).st_mode & 0o777)
    check("the bucket directory and files are made private to the user", info["rc"] == 0 and modes == (0o700, 0o600), "modes %o %o" % modes)
    target = fresh_copy(pristine, work, "prune")
    old_bucket = os.path.join(target, "0123456789abcdef")
    new_bucket = os.path.join(target, "fedcba9876543210")
    other = os.path.join(target, "not-a-bucket")
    for folder in (old_bucket, new_bucket, other):
        os.makedirs(folder)
        open(os.path.join(folder, "file"), "w").close()
    old = time.time() - 30 * 24 * 3600
    for path in (os.path.join(old_bucket, "file"), old_bucket, os.path.join(other, "file"), other):
        os.utime(path, (old, old))
    info = run(test_cmd, target)
    check("pruning removes only idle bucket directories",
          info["rc"] == 0 and not os.path.exists(old_bucket) and os.path.exists(new_bucket) and os.path.exists(other),
          "%s old=%s new=%s other=%s" % (summary(info), os.path.exists(old_bucket), os.path.exists(new_bucket), os.path.exists(other)))

    print("\n%d passed, %d failed" % (passes, len(failures)))
    for name in failures:
        print("  FAILED: %s" % name)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
