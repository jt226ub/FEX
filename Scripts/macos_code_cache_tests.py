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
STATE_LINE = re.compile(r"^Code cache state: (\d+) dropped (\d+) replaced (\d+) reset (\d+)$", re.M)
FLUSH_LINE = re.compile(r"^flush took ([0-9.]+) s$", re.M)
# fexmac_cache_state
OFF, ACTIVE, READ_ONLY, UNTRUSTED, UNAVAILABLE = range(5)
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
    match = STATE_LINE.search(result.stdout)
    if match:
        info.update(zip(("state", "dropped", "replaced", "reset"), map(int, match.groups())))
    match = FLUSH_LINE.search(result.stdout)
    if match:
        info["flush"] = float(match.group(1))
    return info


def summary(info):
    return "rc=%s compiled=%s loaded=%s rejected=%s queued=%s state=%s dropped=%s replaced=%s reset=%s" % (
        info["rc"], info.get("compiled"), info.get("loaded"), info.get("rejected"), info.get("queued"), info.get("state"),
        info.get("dropped"), info.get("replaced"), info.get("reset"))


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


def hold_locks(cache_dir):
    """A process that takes flock on every cache file under cache_dir and keeps it."""
    code = ("import fcntl,os,sys,time\nfds=[]\nfor d,_,fs in os.walk(sys.argv[1]):\n for f in fs:\n  if f.endswith('.foz'):\n"
            "   fd=os.open(os.path.join(d,f),os.O_RDONLY); fcntl.flock(fd,fcntl.LOCK_EX); fds.append(fd)\nprint(len(fds),flush=True)\n"
            "time.sleep(120)\n")
    holder = subprocess.Popen([sys.executable, "-c", code, cache_dir], stdout=subprocess.PIPE, text=True)
    holder.stdout.readline()
    return holder


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


XXH_HELPER_SOURCE = r"""
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
  unsigned long long seed = strtoull(argv[1], 0, 16);
  static unsigned char buf[1 << 24];
  size_t n = fread(buf, 1, sizeof buf, stdin);
  XXH128_hash_t h = XXH3_128bits_withSeed(buf, n, seed);
  fwrite(&h, 1, 16, stdout);
  return 0;
}
"""


def build_xxh_helper(root, work):
    """XXH3-128 with a seed, from the bundled xxhash, to re-seal crafted entries."""
    source = os.path.join(work, "xxh.c")
    binary = os.path.join(work, "xxh")
    with open(source, "w") as handle:
        handle.write(XXH_HELPER_SOURCE)
    subprocess.run(["cc", "-O1", "-I", os.path.join(root, "External", "xxhash"), "-o", binary, source], check=True)
    return binary


class CraftDB:
    """Reads entries of a database and writes re-sealed copies (after the reviewer's craft.py)."""

    def __init__(self, bucket, xxh):
        self.index = [os.path.join(bucket, f) for f in os.listdir(bucket) if f.endswith("_idx.foz")][0]
        self.cache = self.index[:-len("_idx.foz")] + ".foz"
        self.xxh = xxh

    def entries(self):
        data = open(self.index, "rb").read()
        result = []
        for record in parse_index(self.index):
            if record["metadata"]:
                continue
            body = record["body"]
            key, size, _, offset = struct.unpack_from("<QIQQ", data, body)
            _, guest_size, count = struct.unpack_from("<QII", data, body + MESA_ENTRY + 16)
            extents = list(struct.unpack_from("<%dI" % count, data, body + MESA_ENTRY + INDEX_EXTRA))
            result.append(dict(record=record["record"], body=body, key=key, size=size, offset=offset, extents=extents))
        return result

    def blob(self, entry):
        data = open(self.cache, "rb").read()[entry["offset"]:entry["offset"] + entry["size"]]
        guest_size, host_size, count, small, thunk = struct.unpack_from("<5I", data, 0)
        p = BLOB_HEADER
        code = bytearray(data[p:p + host_size])
        p += host_size
        rips = list(struct.unpack_from("<%dQ" % count, data, p))
        p += 8 * count
        hoffs = list(struct.unpack_from("<%dI" % count, data, p))
        p += 4 * count
        rest = data[p:p + small * 14 + thunk * 37]
        p += small * 14 + thunk * 37
        return dict(gs=guest_size, gh=data[20:36], code=code, rips=rips, hoffs=hoffs, small=small, thunk=thunk, rest=rest, guest=data[p:])

    def write(self, entry, blob, extents=None):
        extents = entry["extents"] if extents is None else extents
        count = len(blob["rips"])
        header = struct.pack("<5I", blob["gs"], len(blob["code"]), count, blob["small"], blob["thunk"]) + blob["gh"]
        body = bytes(blob["code"]) + struct.pack("<%dQ" % count, *[r & 0xFFFFFFFFFFFFFFFF for r in blob["rips"]]) + \
            struct.pack("<%dI" % count, *blob["hoffs"]) + bytes(blob["rest"])
        sealed = subprocess.run([self.xxh, "%x" % entry["key"]], input=header + b"\0" * 16 + body + struct.pack("<%dI" % len(extents), *extents),
                                capture_output=True, check=True).stdout
        new_blob = header + sealed + body + bytes(blob["guest"])
        with open(self.cache, "r+b") as handle:
            handle.seek(0, 2)
            handle.write(b"K" * FOZ_KEY + struct.pack("<4I", len(new_blob), 1, 0, len(new_blob)))
            offset = handle.tell()
            handle.write(new_blob)
        with open(self.index, "r+b") as handle:
            handle.seek(entry["body"] + 8)
            handle.write(struct.pack("<I", len(new_blob)))
            handle.seek(entry["body"] + 20)
            handle.write(struct.pack("<Q", offset))


def craft_tests(work, cache_test, base):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    xxh = build_xxh_helper(root, work)
    pristine = os.path.join(work, "craft-pristine")
    cmd = [cache_test, "--threads", "1", "--base", base]
    cold = run(cmd, pristine)
    warm = run(cmd, pristine)

    def craft(name, change, expect_reject):
        target = fresh_copy(pristine, work, "craft-" + re.sub(r"\W+", "-", name))
        bucket = [os.path.join(target, d) for d in os.listdir(target) if os.path.isdir(os.path.join(target, d))][0]
        db = CraftDB(bucket, xxh)
        for entry in db.entries():
            change(db, entry, db.blob(entry))
        info = run(cmd, target)
        if expect_reject == "fallback":
            # Not loaded: either a miss (the live guest bytes hash differently over the
            # changed extents) or a rejection; the block is compiled.
            ok = info["rc"] == 0 and info.get("compiled", 0) > 0 and "PASS" in info["out"]
        elif expect_reject:
            ok = info["rc"] == 0 and info.get("rejected", 0) >= 1 and "PASS" in info["out"]
        else:
            ok = info["rc"] == 0 and info.get("rejected") == 0 and info.get("loaded") == warm.get("loaded") and info.get("compiled") == 0
        check(name, ok, summary(info) + " " + info["out"][-200:])

    check("craft baseline: cold then warm", cold["rc"] == 0 and warm["rc"] == 0 and warm.get("compiled") == 0, summary(warm))

    def reseal(db, e, b):
        db.write(e, b)

    def extra_entry(offset):
        def change(db, e, b):
            b["rips"].append(offset)
            b["hoffs"].append(b["hoffs"][0])
            db.write(e, b)
        return change

    def host_offset(fn):
        def change(db, e, b):
            b["hoffs"][0] = fn(b)
            db.write(e, b)
        return change

    def tail_size(db, e, b):
        tail = struct.unpack_from("<I", b["code"], 0)[0]
        struct.pack_into("<Q", b["code"], tail, len(b["code"]) + 16)
        db.write(e, b)

    def extents_unsealed(db, e, b):
        # Change the index's extents without re-sealing: the hash covers them.
        with open(db.index, "r+b") as handle:
            handle.seek(e["body"] + MESA_ENTRY + INDEX_EXTRA + 4)
            handle.write(struct.pack("<I", max(1, e["extents"][1] - 1)))

    craft("re-sealed unchanged entries still load (the crafting is right)", reseal, False)
    craft("an extra guest entry point beyond the block (+0x100000) is rejected", extra_entry(0x100000), True)
    craft("an extra guest entry point before the block (-0x8000000000) is rejected", extra_entry(-0x8000000000), True)
    craft("a host entry point inside the block tail is rejected", host_offset(lambda b: len(b["code"]) - 16), True)
    craft("a host entry point on the block header is rejected", host_offset(lambda b: 0), True)
    craft("a block tail whose size disagrees is rejected", tail_size, True)
    craft("guest extents changed in the index (not re-sealed) are not used", extents_unsealed, "fallback")

    # A process that died while appending leaves a partial index record. The next process
    # to store must cut it off first, or the records it appends after it are lost.
    target = fresh_copy(pristine, work, "torn-then-append")
    first = run(cmd + ["--variant", "sub", "--tear-index"], target)
    second = run(cmd + ["--variant", "sub"], target)
    original = run(cmd, target)
    check("entries stored after a partly written record are kept",
          first["rc"] == 0 and first.get("queued", 0) > 0 and second.get("compiled") == 0 and original.get("compiled") == 0,
          "%s / %s / %s" % (summary(first), summary(second), summary(original)))

    # Index records with an odd extent count, or extents out of order: never parsed into use.
    for name, extents in [("odd extent count", [0, 4, 6]), ("extents out of order", [6, 3, 0, 2]), ("extent past the guest size", [0, 0x7FFFFFFF])]:
        target = fresh_copy(pristine, work, "index-" + re.sub(r"\W+", "-", name))
        bucket = [os.path.join(target, d) for d in os.listdir(target) if os.path.isdir(os.path.join(target, d))][0]
        db = CraftDB(bucket, xxh)
        data = open(db.index, "rb").read()
        for entry in db.entries():
            body = data[entry["body"]:entry["body"] + MESA_ENTRY + INDEX_EXTRA - 4] + struct.pack("<I", len(extents))
            payload = body + struct.pack("<%dI" % len(extents), *extents)
            # Same lookup key, appended: a second candidate for the same code.
            with open(db.index, "ab") as handle:
                handle.write(b"0" * (FOZ_KEY - 1) + b"\0" + struct.pack("<4I", len(payload), 1, 0, len(payload)) + payload)
        info = run(cmd, target)
        check("an index record with an %s is ignored" % name if name[0] in "aeiou" else "an index record with %s is ignored" % name,
              info["rc"] == 0 and "PASS" in info["out"] and info.get("compiled") == 0, summary(info))


FUZZ_VALUES = [0, 1, 2, 3, 4, 8, 12, 14, 16, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFF0,
               0xFFFFFFFC, 0xFFFFFFFF]


def fuzz(work, cache_test, base, iterations):
    """Random damage to a database (after the reviewer's fuzz2.py). Every run must pass."""
    import random
    random.seed(1234)
    pristine = os.path.join(work, "fuzz-pristine")
    cmd = [cache_test, "--threads", "1", "--blocks", "20", "--base", base]
    run(cmd, pristine)
    bucket_name = [d for d in os.listdir(pristine) if os.path.isdir(os.path.join(pristine, d))][0]
    files = sorted(f for f in os.listdir(os.path.join(pristine, bucket_name)) if f.endswith(".foz"))
    bad = []
    for iteration in range(iterations):
        target = fresh_copy(pristine, work, "fuzz-work")
        done = []
        for _ in range(random.randint(1, 4)):
            path = os.path.join(target, bucket_name, random.choice(files))
            data = bytearray(open(path, "rb").read())
            if not data:
                continue
            kind = random.random()
            if kind < 0.35:
                offset = random.randrange(len(data))
                data[offset] ^= 1 << random.randrange(8)
                done.append("flip %s %d" % (path[-8:], offset))
            elif kind < 0.75 and len(data) >= 4:
                offset = random.randrange(len(data) - 3)
                value = random.choice(FUZZ_VALUES) if random.random() < 0.7 else random.getrandbits(32)
                data[offset:offset + 4] = struct.pack("<I", value)
                done.append("poke %s %d %x" % (path[-8:], offset, value))
            elif kind < 0.85:
                offset = random.randrange(len(data))
                data = data[:offset]
                done.append("truncate %s %d" % (path[-8:], offset))
            elif kind < 0.95 and len(data) > 64:
                source, dest, count = random.randrange(len(data) - 32), random.randrange(len(data) - 32), random.randint(1, 32)
                data[dest:dest + count] = data[source:source + count]
                done.append("copy %s %d->%d %d" % (path[-8:], source, dest, count))
            else:
                offset = random.randrange(len(data))
                data[offset] = random.getrandbits(8)
                done.append("byte %s %d" % (path[-8:], offset))
            open(path, "wb").write(data)
        info = run(cmd, target)
        if info["rc"] != 0 or "PASS" not in info["out"]:
            bad.append("%d: %s %s" % (iteration, done, summary(info)))
            shutil.copytree(target, os.path.join(work, "fuzz-bad-%d" % iteration))
    check("%d randomly damaged caches all give the right answer" % iterations, not bad, "; ".join(bad[:3]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--cache-test", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--test", default="TwoByte/0F_B0_3.asm", help="instruction test used for the file-damage cases")
    parser.add_argument("--fuzz", type=int, default=300, help="number of randomly damaged caches to run")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    work = os.path.abspath(args.work)
    if os.path.exists(work):
        subprocess.run(["chmod", "-RN", work])  # ACLs from an earlier run
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
        # FEXCoreCacheTest states SMCCHECKS none unless told otherwise.
        ("full self-modifying-code checks", {"FEX_SMCCHECKS": "full"}),
        # Not a mismatch: mtrack translates the same code as none (its detection is page
        # protection), and a block loaded from the cache registers its code pages like a
        # translated one (CodeCache.cpp, MarkGuestExecutableRange), so the cache is shared.
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

    # 6. Directory and file rules: anything not private to the user is refused, never
    # narrowed. A refused read-write database in a private bucket is removed and started
    # again; a refused directory switches the cache off for the run.
    print("== directory and file rules")
    def bucket_of(target):
        return os.path.dirname(os.path.join(target, cache_rel))

    target = fresh_copy(pristine, work, "shared-dir")
    os.chmod(target, 0o777)
    info = run(test_cmd, target)
    check("a base directory writable by other users switches the cache off (state UNTRUSTED)",
          info["rc"] == 0 and info.get("loaded") == 0 and info.get("queued") == 0 and info.get("state") == UNTRUSTED and
          os.stat(target).st_mode & 0o777 == 0o777, summary(info))
    os.chmod(target, 0o755)

    target = fresh_copy(pristine, work, "bucket-open")
    os.chmod(bucket_of(target), 0o755)
    info = run(test_cmd, target)
    check("a bucket directory that is not private switches the cache off (UNTRUSTED) and is left as it was",
          info["rc"] == 0 and info.get("loaded") == 0 and info.get("queued") == 0 and info.get("state") == UNTRUSTED and
          os.stat(bucket_of(target)).st_mode & 0o777 == 0o755, summary(info))

    target = fresh_copy(pristine, work, "file-open")
    path = os.path.join(target, cache_rel)
    os.chmod(path, 0o666)
    before = os.stat(path).st_ino
    info = run(test_cmd, target)
    after = os.stat(path)
    check("a cache file writable by others is not used: removed, started again, counted (state ACTIVE, replaced 1)",
          info["rc"] == 0 and info.get("loaded") == 0 and after.st_ino != before and after.st_mode & 0o777 == 0o600 and
          info.get("state") == ACTIVE and info.get("replaced") == 1, summary(info))

    # Not trust: the cache cannot be used for another reason (state UNAVAILABLE).
    target = os.path.join(work, "read-only-dir")
    os.makedirs(target)
    os.chmod(target, 0o500)
    info = run(test_cmd, target)
    os.chmod(target, 0o700)
    check("a cache directory that cannot be written: state UNAVAILABLE, the run is right",
          info["rc"] == 0 and info.get("state") == UNAVAILABLE and info.get("queued") == 0, summary(info))
    target = fresh_copy(pristine, work, "busy-at-open")
    holder = hold_locks(target)
    started = time.monotonic()
    info = run(test_cmd, target)
    waited = time.monotonic() - started
    holder.kill()
    holder.wait()
    check("a database kept locked by another process at open: state UNAVAILABLE after one wait (%.1f s)" % waited,
          info["rc"] == 0 and info.get("state") == UNAVAILABLE and waited < 9, summary(info))

    target = fresh_copy(pristine, work, "file-acl")
    path = os.path.join(target, cache_rel)
    subprocess.run(["chmod", "+a", "everyone allow write", path], check=True)
    info = run(test_cmd, target)
    acl = subprocess.run(["ls", "-le", path], capture_output=True, text=True).stdout
    check("a cache file with an ACL entry granting write is not used", info["rc"] == 0 and info.get("loaded") == 0 and "allow write" not in acl,
          summary(info))

    target = fresh_copy(pristine, work, "bucket-acl")
    subprocess.run(["chmod", "+a", "everyone allow add_file,delete_child", bucket_of(target)], check=True)
    info = run(test_cmd, target)
    check("a bucket with an ACL entry granting add/delete switches the cache off",
          info["rc"] == 0 and info.get("loaded") == 0 and info.get("queued") == 0, summary(info))

    target = fresh_copy(pristine, work, "base-acl")
    subprocess.run(["chmod", "+a", "everyone allow add_subdirectory", target], check=True)
    info = run(test_cmd, target)
    check("a base directory with an ACL entry granting add switches the cache off",
          info["rc"] == 0 and info.get("loaded") == 0 and info.get("queued") == 0, summary(info))

    target = fresh_copy(pristine, work, "deny-acl")
    subprocess.run(["chmod", "+a", "everyone deny delete", bucket_of(target)], check=True)
    info = run(test_cmd, target)
    check("a deny-only ACL entry is accepted", info["rc"] == 0 and info.get("compiled") == 0 and info.get("loaded", 0) > 0, summary(info))
    subprocess.run(["chmod", "-N", bucket_of(target)], check=True)  # so the work directory can be removed

    target = fresh_copy(pristine, work, "file-symlink")
    path = os.path.join(target, cache_rel)
    real = os.path.join(work, "symlink-target.foz")
    shutil.move(path, real)
    os.symlink(real, path)
    real_before = open(real, "rb").read()
    info = run(test_cmd, target)
    check("a symbolic link in place of a cache file is not followed: removed, database started again, target untouched",
          info["rc"] == 0 and info.get("loaded") == 0 and not os.path.islink(path) and open(real, "rb").read() == real_before, summary(info))

    target = fresh_copy(pristine, work, "bucket-symlink")
    bucket = bucket_of(target)
    elsewhere = os.path.join(work, "bucket-elsewhere")
    shutil.move(bucket, elsewhere)
    os.chmod(elsewhere, 0o755)
    os.symlink(elsewhere, bucket)
    info = run(test_cmd, target)
    check("a symbolic link in place of the bucket directory switches the cache off and its target is not changed",
          info["rc"] == 0 and info.get("loaded") == 0 and info.get("queued") == 0 and os.stat(elsewhere).st_mode & 0o777 == 0o755, summary(info))

    target = fresh_copy(pristine, work, "hardlink")
    path = os.path.join(target, cache_rel)
    os.link(path, os.path.join(work, "second-name.foz"))
    info = run(test_cmd, target)
    check("a cache file with a second name is not used", info["rc"] == 0 and info.get("loaded") == 0, summary(info))

    target = fresh_copy(pristine, work, "prune")
    old_bucket = os.path.join(target, "0123456789abcdef")
    used_bucket = os.path.join(target, "00112233445566ff")
    new_bucket = os.path.join(target, "fedcba9876543210")
    other = os.path.join(target, "not-a-bucket")
    for folder in (old_bucket, used_bucket, new_bucket, other):
        os.makedirs(folder)
        open(os.path.join(folder, "file"), "w").close()
    open(os.path.join(used_bucket, "last-used"), "w").close()
    old = time.time() - 30 * 24 * 3600
    for path in (os.path.join(old_bucket, "file"), old_bucket, os.path.join(used_bucket, "file"), used_bucket, os.path.join(other, "file"), other):
        os.utime(path, (old, old))
    info = run(test_cmd, target)
    exists = [os.path.exists(p) for p in (old_bucket, used_bucket, new_bucket, other)]
    marker = os.path.join(bucket_of(target), "last-used")
    check("pruning removes only bucket directories nobody used for a week; opening a bucket marks it used",
          info["rc"] == 0 and exists == [False, True, True, True] and os.path.exists(marker) and time.time() - os.stat(marker).st_mtime < 600,
          "%s exists=%s" % (summary(info), exists))

    # 7. Size limit: stores stop at DiskCacheMaxFileSize; the files never grow past it.
    print("== size limit")
    target = os.path.join(work, "size-limit")
    limit = 2048
    info = run([cache_test, "--threads", "1", "--blocks", "300"], target, {"FEX_DISKCACHEMAXFILESIZE": str(limit)})
    sizes = [os.path.getsize(f) for pair in db_files(target) for f in pair]
    check("cache files stay within the size limit", info["rc"] == 0 and sizes and max(sizes) <= limit, "%s sizes=%s" % (summary(info), sizes))
    # Generations: with a small limit and many different blocks the database fills, a new
    # generation starts, the previous one is still read, and older ones are deleted.
    target = os.path.join(work, "generations")
    gen_cmd = [cache_test, "--threads", "1", "--blocks", "120", "--distinct", "--base", "0x8000000000"]
    # Sized for code without per-instruction self-modifying-code checks (stated, as
    # FEXCoreCacheTest's own default).
    gen_env = {"FEX_DISKCACHEMAXFILESIZE": "16384", "FEX_SMCCHECKS": "none"}
    first = run(gen_cmd, target, gen_env)
    names = sorted(f for folder, _, files in os.walk(target) for f in files if f.endswith("_idx.foz"))
    generations = [n for n in names if re.search(r"-\d+_idx\.foz$", n)]
    sizes = [os.path.getsize(f) for pair in db_files(target) for f in pair]
    second = run(gen_cmd, target, gen_env)
    check("a full database continues in a new generation; at most two are kept, each within the limit; the next run loads",
          first["rc"] == 0 and second["rc"] == 0 and generations and len(names) <= 2 and max(sizes) <= 16384 and
          second.get("loaded", 0) > 0 and second.get("rejected") == 0 and first.get("dropped") == 0,
          "%s / %s generations=%s sizes=%s" % (summary(first), summary(second), names, sizes))

    # Six processes at once, each filling and rotating the same database, 72 runs: no false
    # trust or damage event (a generation deleted by one process while another opens it is a
    # race to retry, not an untrusted or damaged file), and every answer right.
    target = os.path.join(work, "rotation-stress")
    import concurrent.futures

    def stress_run(index):
        cmd = [cache_test, "--threads", "1", "--blocks", "60", "--distinct"] + (["--variant", "sub"] if index % 2 else [])
        return run(cmd, target, {"FEX_DISKCACHEMAXFILESIZE": "12288"})
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        stress = list(pool.map(stress_run, range(72)))
    bad = [summary(i) for i in stress if i["rc"] != 0 or "PASS" not in i["out"]]
    replaced = sum(i.get("replaced", 0) for i in stress)
    reset = sum(i.get("reset", 0) for i in stress)
    rejected = sum(i.get("rejected", 0) for i in stress)
    names = sorted(f for folder, _, files in os.walk(target) for f in files if f.endswith("_idx.foz"))
    check("six processes rotating one database, 72 runs: all right, untrusted replaced %d, damaged reset %d, rejected %d" %
          (replaced, reset, rejected), not bad and replaced == 0 and reset == 0 and rejected == 0 and len(names) <= 3,
          "; ".join(bad[:3]) + " generations=%s" % names)

    # A planted generation name far ahead is ignored: start-up stays quick and the real
    # generations are neither skipped nor deleted.
    target = os.path.join(work, "planted-generation")
    run(gen_cmd, target)
    (pcache, pindex), = db_files(target)
    planted = pindex.replace("_idx.foz", "-2000000_idx.foz")
    open(planted, "wb").close()
    os.chmod(planted, 0o600)
    started = time.monotonic()
    info = run(gen_cmd, target)
    elapsed = time.monotonic() - started
    check("a planted generation -2000000 is ignored (%.2f s, the real one still loads)" % elapsed,
          info["rc"] == 0 and info.get("compiled") == 0 and os.path.exists(pindex) and elapsed < 2, summary(info))

    # A database another process filled past this process's (smaller) limit is used read-only,
    # never emptied.
    target = os.path.join(work, "over-limit")
    big = run(gen_cmd, target)
    before = sorted((f, os.path.getsize(f)) for pair in db_files(target) for f in pair)
    small = run(gen_cmd, target, {"FEX_DISKCACHEMAXFILESIZE": "4096"})
    after = sorted((f, os.path.getsize(f)) for pair in db_files(target) for f in pair)
    again = run(gen_cmd, target)
    check("a database larger than this process's limit is read, not written or emptied (state READ_ONLY)",
          big["rc"] == 0 and small["rc"] == 0 and small.get("state") == READ_ONLY and small.get("compiled") == 0 and
          before == after and again.get("compiled") == 0, "%s / %s / %s" % (summary(big), summary(small), summary(again)))

    # 8. Code generation state that is not in the configuration.
    print("== code generation state outside the configuration")
    sdir = os.path.join(work, "state")
    base = "0x8000000000"
    run([cache_test, "--threads", "1", "--base", base], sdir)
    info = run([cache_test, "--threads", "1", "--base", base, "--trampoline"], sdir)
    check("a custom IR handler (thunk trampoline) is not shadowed by a cached block", info["rc"] == 0, summary(info))
    for flag, env in [("--hardware-tso", {"FEX_TSOENABLED": "1"}), ("--force-tso", {})]:
        sdir = os.path.join(work, "state" + flag)
        run([cache_test, "--threads", "1", "--base", base], sdir, env)
        info = run([cache_test, "--threads", "1", "--base", base, flag], sdir, env)
        empty = run([cache_test, "--threads", "1", "--base", base, flag], os.path.join(work, "empty" + flag), env)
        same = all(info.get(k) == empty.get(k) for k in ("compiled", "loaded", "queued"))
        check("cached code is not used with %s" % flag, info["rc"] == 0 and same, "%s; empty cache: %s" % (summary(info), summary(empty)))

    # 9. Guest code changing between decoding and storing (the first time a page is
    # reported executable, which the core does between the two) must not be filed
    # under the new bytes.
    print("== guest code changed while compiling")
    rdir = os.path.join(work, "race")
    info = run([cache_test, "--threads", "1", "--base", base, "--race"], rdir)
    add = run([cache_test, "--threads", "1", "--base", base], rdir)
    sub = run([cache_test, "--threads", "1", "--base", base, "--variant", "sub"], rdir)
    check("no translation is stored under bytes it was not made from", info["rc"] == 0 and add["rc"] == 0 and sub["rc"] == 0,
          "%s / %s / %s" % (summary(info), summary(add), summary(sub)))

    # 10. fork while the cache is in use, cold and warm: the child runs guest code,
    # flushes (at once: it does not write) and exits; the parent carries on.
    print("== fork")
    fdir = os.path.join(work, "fork")
    for name in ("cold", "warm"):
        info = run([cache_test, "--threads", "4", "--fork"], fdir)
        ok = info["rc"] == 0 and "child: 0 wrong, flush skipped" in info["out"]
        check("fork with a %s cache" % name, ok, summary(info) + " " + info["out"][-300:])
    rawdir = os.path.join(work, "fork-raw")
    for name in ("cold", "warm"):
        info = run([cache_test, "--threads", "4", "--fork-raw"], rawdir)
        ok = info["rc"] == 0 and "child: 0 wrong, flush skipped, cache state as expected" in info["out"]
        check("fork without fexmac_before_fork, %s cache: the child turns read-only, its flush returns" % name, ok,
              summary(info) + " " + info["out"][-300:])

    # A process holding the database's locks (sleeping, or stopped with SIGSTOP) after this
    # one opened it: one wait, then every store is dropped and counted; the flush at exit
    # returns after about one deadline whatever the number of entries.
    print("== a stalled lock holder")
    ldir = os.path.join(work, "stall")
    run([cache_test, "--threads", "1", "--blocks", "3"], ldir)
    for how in ("hold", "stop"):
        info = run([cache_test, "--threads", "1", "--blocks", "60", "--distinct", "--stall-lock", how], ldir)
        check("lock holder that %s: %s entries dropped, flush %.1f s" % ("sleeps" if how == "hold" else "is stopped", info.get("dropped"),
                                                                         info.get("flush", -1)),
              info["rc"] == 0 and info.get("queued", 0) > 10 and info.get("dropped") == info.get("queued") and info.get("flush", 99) < 7,
              summary(info))

    # An entry that fails its checks is superseded: the next run loads cleanly.
    print("== a damaged entry is replaced")
    target = fresh_copy(pristine, work, "superseded")
    (dcache, dindex), = db_files(target)
    damaged = [r for r in parse_index(dindex) if not r["metadata"]][0]
    flip(dcache, damaged["blob"] + BLOB_HEADER + 8, 3)
    first = run(test_cmd, target)
    second = run(test_cmd, target)
    third = run(test_cmd, target)
    check("damaged entry: rejected once, then replaced (next runs load everything, rejected 0)",
          first.get("rejected", 0) >= 1 and second.get("compiled") == 0 and second.get("rejected") == 0 and third.get("rejected") == 0,
          "%s / %s / %s" % (summary(first), summary(second), summary(third)))

    # 11. Entries re-sealed with a correct integrity hash but bad contents (what a process
    # running as the user could write), and index records outside the hash.
    print("== re-sealed entries and index records")
    craft_tests(work, cache_test, base)

    # 12. Random damage.
    print("== fuzz")
    fuzz(work, cache_test, base, args.fuzz)
    print("\n%d passed, %d failed" % (passes, len(failures)))
    for name in failures:
        print("  FAILED: %s" % name)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
