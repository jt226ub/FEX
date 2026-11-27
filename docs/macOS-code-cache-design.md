# macOS: the on-disk code cache

Status: implemented (work package F). Cached translations are loaded by copying them into
the live `MAP_JIT` code buffer. This document describes what is built, why it differs
from the first design, and the rules a cache file must pass before any of it is used.

## What the cache is for

Translating x86-64 code takes time and causes stutter the first time each piece of a
program runs. The cache saves translated code to disk so the next run can reuse it.

## Correction to the first design

The first version of this document planned to adapt upstream's whole-section cache
(`CodeCache.cpp`, files of type `FXCC`, behind `EnableCodeCachingWIP`): a file per guest
binary section, mapped and then published with `mremap`. That plan was written before
the upstream sync at `a92baf073`, and it missed that upstream now has a second, separate
cache that already works the way this port needs:

- **`DiskCache` (`FEXCore/Source/Interface/Core/DiskCache.cpp`, format version 49).** A
  per-block cache. When the core is about to compile a block it looks the block up by a
  hash of its guest bytes; on a hit it copies the cached host code into the code buffer
  (`Arm64JITCore::LoadCachedCode`, inside a `ScopedJITWrite`), applies the relocations,
  flushes the instruction cache and registers the block in the lookup cache, exactly like
  a freshly compiled block. On a miss it compiles and hands a copy of the result to a
  writer thread that appends it to the database.

That is "load by copy into the live code buffer" with no file mapping and no `mremap`, so
this port uses `DiskCache` and leaves the `FXCC` path alone. On macOS the `FXCC` path still
stops with an error (`CodeCache::FinalizeCodePages`); none of this port's tools enables
it, and it is not needed.

What the first design got right and still holds: code can only live in `MAP_JIT` memory,
`MAP_JIT` memory cannot be file-backed or remapped, so the cache is copied, not mapped.

## How a block goes through the cache

Store (cold run), in `ContextImpl::CompileBlock` after compiling:

1. The compiled code is copied out of the code buffer before the block is published, so
   no other thread can have linked it yet: exit stubs are still in their unlinked state.
2. Every place in the code that depends on this run is recorded as a relocation (the
   JIT emits them whenever the cache is being written, `RequiresRelocatableConstants`):
   - the exit-linker pointer (`RELOC_NAMED_SYMBOL_LITERAL`), the only host address the
     block code bakes in; dispatcher entry points and thread state are reached through
     the state register, at offsets fixed by the build;
   - guest addresses (`RELOC_GUEST_RIP_LITERAL`, `RELOC_GUEST_RIP_MOVE`), stored relative
     to the block's entry, including the guest address in the block tail;
   - immediates and displacements the frontend marked patchable
     (`RELOC_GUEST_PATCHABLE_*`): they are re-read from live guest memory at load time.
3. The entry stores the host code, the entry points (guest offset, host offset), the
   relocations and a copy of the guest bytes, and is keyed as described below.

Load (warm run), in `DiskCache::Lookup` and `CompileBlock`:

1. Find candidate entries by lookup key, then compare a hash of the live guest bytes with
   the entry's guest hash.
2. Read the entry and check it completely (sizes, integrity hash, every offset and
   operand) before using any of it; see "Rejection".
3. Apply the relocations to the entry in ordinary memory (not executable memory).
4. Copy it into the code buffer inside one `ScopedJITWrite`, flush the instruction cache
   while still writable (enough on an M4, per `JITWriteProbe`; `ClearICache` is the same
   call the compile path uses), switch back.
5. Register the entry points and the guest code pages in the lookup cache.

Block links between cached and freshly compiled code need nothing special: both live in
the same buffer and are linked lazily by the exit linker, as usual. Block tails, which
hold a spin-lock word, are part of the copied code; nothing writes them outside a write
scope. `mprotect` is never used on the code buffer.

Guest code may sit at a different address from run to run (the instruction tests are
moved up by 1TB; `FEXCoreCacheTest` maps its code wherever the system puts it). Nothing
absolute about the guest is baked in: guest addresses are relative to the block entry and
the key does not contain the address. The cache tests load the same code at three
addresses.

## Keys

A cache entry can only be found under the exact combination that produced it.

- **Directory (machine bucket):** hash of the format version, upstream's host-feature
  bits, and, added by this port, the port identity: a port cache revision, the git hash
  the core was configured from, the **Mach-O UUID of the image the core is linked into**
  (the linker derives it from the code, so any rebuilt core gets a new one, even with
  uncommitted changes), the CPU brand string and the CPU MIDRs.
- **File (process bucket):** hash of the 64-bit mode, SVE flags and every configuration
  option marked as affecting code generation: multiblock, maximum instructions per block,
  TSO emulation and each of its sub-switches, self-modifying-code check mode, x87 reduced
  precision, and so on. Upstream skipped enumerated options; this port adds them
  (`JITFeatures` is one that changes code).
- **Entry:** a hash of the guest bytes of the block with patchable fields left out, plus
  flags (writable code, trap flag set, Mono hacks), then the full guest hash over the
  exact byte extents the block was translated from.

Each database's first record is its description: the full bytes the two bucket hashes
are made from. This port checks it when the database is opened and refuses (and, for the
read-write database, empties and restarts) a database whose description is not exactly
this process's. The directory and file names already differ for any mismatch, so this
only fires for a damaged file or one copied under another name.

Guest code identity, not names: the anonymous key is computed from the bytes, so the same
code at the same address with different bytes misses, and stale code is never used after
guest code changes. In a running process, self-modifying code is still handled by the
usual invalidation; a block recompiled after a change is looked up by its new bytes.

### Patchable fields: a correction found by the tests

Patchable immediates and displacements let one entry serve blocks that differ only in
those fields (for example the same instruction sequence with different constants). The
relocation rebuilds a RIP-relative target as end-of-field plus the live displacement,
which assumes the displacement is the last field of the instruction. For 3DNow! the
opcode byte follows it, and loading such a cached block read data one byte off: 22 of 27
`Test_3DNow` tests failed on the warm run in `jit_1`. This port only marks a RIP-relative
displacement (or branch offset) patchable when it ends the instruction; other
instructions are matched byte for byte. The same bug is in upstream's code.

A second, harmless difference: a patched 32-bit immediate is loaded sign-extended, while
the original compile loads it zero-extended. Every consumer seen uses only the low 32
bits, so results are the same, but `--cache-validate` (byte comparison against a fresh
compile) reports those blocks.

## Rejection: the cache file is untrusted input

Everything read from a cache file is checked before it is used, and nothing is copied into
executable memory until the whole entry has passed:

- Index records: framing and sizes within both files (upstream), extents within the guest
  size (upstream).
- Entry sizes computed in 64 bits, so a damaged count cannot wrap to a matching size.
- **Integrity hash:** XXH3-128 over the entry header (hash field zeroed), host code, entry
  points and relocations, seeded with the entry's lookup key. Detects torn writes, bit
  flips and entries moved to another key.
- Shape: host code size non-zero and 16-byte aligned; at least one entry point, one of
  them the block entry; host offsets inside the code and instruction-aligned.
- Relocations: known type, write fully inside the code, symbol index known, register
  index below 32, patchable sites inside the guest bytes that were hashed, thunk
  relocations only if the embedder has a thunk handler.
- Guest bytes are read only inside the range the embedder reports as mapped
  (`QueryGuestExecutableRange`); the test runner now reports its real mappings.

Any failure counts as a rejection (`TranslationCounters::CacheEntriesRejected`) and the
block is compiled instead. `Scripts/macos_code_cache_tests.py` damages files in 37 ways
(zero length, truncation at several points, bit flips in headers, code, entry points,
relocations and the index, length fields set huge) and requires the right result every
time.

### Trust model, stated plainly

A cache file is executable code. Whoever can write it can make this process run anything.
The rules are therefore about who can write it:

- The bucket directory is made private (0700) and cache files 0600. A file that is not a
  regular file (for example a symbolic link), is not owned by this user, or (read-only
  databases) is writable by others, is not used. The base directory must be owned by
  this user and not writable by other users, or the cache is switched off.
- The integrity hash is **not a security boundary**. It is an unkeyed checksum: any
  process running as the same user can rewrite a cache file and its hash. The cache
  protects against accidents and against other users, not against code already running
  with the user's rights (which could equally patch the program itself).
- Directories above the base directory are not checked.

## Writing files: atomicity

The first design asked for write-to-temporary, `fsync`, rename. That fits a cache written
once as a whole; upstream's database is append-only and shared, so it is kept and made
safe instead:

- Every append takes an exclusive `flock` on both files; the blob is written before its
  index record, so a reader never sees an index record for a blob that is not there.
- Opening a read-write database takes both locks, cuts a partly written last index
  record (a process that died mid-append), reads the index and checks the description.
- A torn or damaged blob that an index record does point to fails the integrity check.
- `Context::FlushDiskCache()` waits for the writer thread to finish every queued store and
  then calls `fcntl(F_FULLFSYNC)` (the macOS way through the drive cache) on both files.
  Entries queued but not flushed when a process exits are lost, never half-used.
- Several processes and threads can use one database at once: this is what the
  instruction-test run does (six processes per mode, four modes, one directory).
- Files are read with `pread`; 16K pages play no part in the format.

Stale buckets: upstream removes every other bucket directory when a process starts. With
processes of different builds or host-feature settings running together (the instruction
tests set host features per test) they deleted each other's caches mid-run. On macOS a
bucket directory is only removed if it is named like a bucket (16 hex digits, so a cache
path that holds other things is safe) and nothing in it was written for seven days.

## Where files live

`FEX_DISKCACHEPATH`, or by default `~/Library/Caches/fex-emu/DiskCache/` on macOS (the
system's disposable per-user cache location; `FEX_APP_CACHE_LOCATION` still overrides).
Inside: `<machine bucket>/RWCacheDB_<process bucket>.foz` and `..._idx.foz`.

## The embedder's side

The core does the work; an embedder only switches it on and flushes:

- Configuration: `DiskCache=1` (`FEX_DISKCACHE=1`) and `DiskCachePath`
  (`FEX_DISKCACHEPATH=<dir>`), set before the context is created. `DiskCacheValidation`
  compiles as well and compares, for diagnosis.
- `Context::FlushDiskCache()` before the process exits.
- `Context::GetTranslationCounters()`: blocks compiled, blocks loaded, entries rejected,
  entries queued.
- `QueryGuestExecutableRange` must report the real mapped range of guest code.
- The core's writer thread needs a thread implementation; on macOS the core now has a
  plain pthread default (signals blocked, `QOS_CLASS_UTILITY` for low priority), so an
  embedder need not install one.

For the C interface (package C) this means: a cache directory and an on/off switch in
`fexmac_config`, `fexmac_context_flush_cache()`, and a counters query.

## Measured

See `docs/macOS-port-status.md` for the numbers (time for the instruction tests cold and
warm, bytes on disk).

## Open questions

1. Ahead-of-time translation of a whole binary using the same format (upstream's offline
   code-map pipeline is not built on macOS).
2. Sharing read-only databases between users (`DiskCacheRODBNames`) needs a different trust
   model and is not supported here: read-only databases must belong to the user.
