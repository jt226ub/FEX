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
  writer thread that appends it to the database. On macOS the writer appends in batches
  (`WriteBatchMacOS`): stores collect until the queue holds no more of them (or 4096
  entries or 4MB), then the batch is written with one lock taken and one write per file.
  Per-entry appends (two `flock`s, five `fstat`s and seven `pwrite`s each) made a test
  with 65,538 one-instruction blocks take 1.1 s alone and over 60 s when many processes
  wrote at once on a busy machine, since every entry handed the lock to the next process
  and the exit waited for the whole queue; batched, the same test takes 0.3 s alone and
  0.6 s with six copies writing one database next to ten CPU-bound processes (139 s
  before). While a flush waits for the writer (at exit), the writer runs at
  `QOS_CLASS_USER_INITIATED` instead of its usual utility class. Compiling threads never
  wait for a file lock: they only queue the entry.

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
4. Copy it into the code buffer inside one `ScopedJITWrite`, end the write scope, then
   flush the instruction cache for the range (`Arm64JITCore::LoadCachedCode`, the same
   order and call as the compile path).
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
  bits, and, added by this port, the port identity: a port cache revision (3), the git
  hash the core was configured from, the **Mach-O UUID of the image the core's code is
  in** (the linker derives it from the code, so any rebuilt core gets a new one, even
  with uncommitted changes), the CPU brand string and the CPU MIDRs. If the image has no
  `LC_UUID` the cache is switched off rather than keyed without one.

  Which UUID: the identity that matters is that of the code generator. When the core is
  a dylib, the image is the core alone. When it is linked statically into a host, the
  image is the host, so rebuilding the host (even without touching the core) starts a
  new bucket and two different hosts never share one. That costs cold starts, never
  correctness; a narrower identity (a hash of just the core's objects, made at build
  time) would need a post-link build step and could be wrong if it missed an input, so
  the image UUID is kept. Two hosts' buckets live side by side and each is pruned only
  when unused (below).
- **File (process bucket):** hash of the 64-bit mode, SVE flags and every configuration
  option marked as affecting code generation: multiblock, maximum instructions per block,
  TSO emulation and each of its sub-switches, self-modifying-code check mode, x87 reduced
  precision, and so on. Upstream skipped enumerated options; this port adds them
  (`JITFeatures` is one that changes code).
- **Entry:** a hash of the guest bytes of the block with patchable fields left out, plus
  flags (writable code, trap flag set, Mono hacks, and, added by this port, the effective
  TSO emulation state: `SetHardwareTSOSupport` changes it after the configuration was
  hashed), then the full guest hash over the exact byte extents the block was translated
  from.
- **Never cached:** blocks overlapping a range given to `AddForceTSOInformation` (their
  code depends on that per-instruction list), and blocks at or with an entry point at an
  address with a custom IR handler (`AddCustomIREntrypoint`, `AddThunkTrampolineIRHandler`):
  a cached block made from the bytes would otherwise shadow the handler. Neither is
  served from the cache nor stored to it.

Each database's first record is its description: the full bytes the two bucket hashes
are made from. This port checks it when the database is opened and refuses (and, for the
read-write database, empties and restarts) a database whose description is not exactly
this process's. The directory and file names already differ for any mismatch, so this
only fires for a damaged file or one copied under another name.

Guest code identity, not names: the anonymous key is computed from the bytes, so the same
code at the same address with different bytes misses, and stale code is never used after
guest code changes. In a running process, self-modifying code is still handled by the
usual invalidation; a block recompiled after a change is looked up by its new bytes.

Upstream hashed and copied the live guest bytes at store time, not the bytes the decoder
read, so a write between decoding and storing filed the old translation under the new
bytes for good. The decoder now copies every byte it consumes as it reads it
(`Decoder::RecordByte`, in `ReadByte` and `ReadData`), and the store hashes and saves that
copy: the guest hash is of exactly the bytes the translation was made from. A byte the
decoder reads twice with different values (the guest wrote it during decoding) marks the
copy unusable and nothing is stored. (`FEXCoreCacheTest --race` changes the code between
decoding and storing; with the live re-read its later runs gave wrong answers.)

A cache entry that fails its checks is marked rejected in the process (never tried
again, and not a duplicate when the block is compiled and stored afresh), and in later
processes a newer record for the same guest code replaces an older one, so a damaged
entry is superseded once and the next run loads cleanly.

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

- Index records: framing; blob offset and size inside the cache file; guest size non-zero
  and inside the blob; an even, non-zero number of extent words with the payload size
  matching it exactly; extents inside the guest size, in order and not overlapping. A
  record failing any of these is ignored. The record key is only a name (except its last
  byte, which marks the description); the footprint is only a sort key.
- Entry sizes computed in 64 bits, so a damaged count cannot wrap to a matching size.
- **Integrity hash:** XXH3-128 over the entry header (hash field zeroed), host code, entry
  points, relocations and the index record's guest extents, seeded with the entry's
  lookup key. So the fields of the index that decide what is executed (key, extents)
  are covered as well as the blob; the guest size and guest hash are compared with the
  blob's own. Detects torn writes, bit flips and entries moved to another key.
  (A stack-allocated hash state used with a seed must be cleared with `XXH3_INITSTATE`;
  without it, worker threads hashed with a stale secret now and then, which showed up as
  random rejections.)
- Shape: host code size 16-byte aligned and large enough for the block header and tail;
  the header's tail offset aligned and inside the code; the tail's recorded size equal to
  the code size.
- Entry points: at least one, one of them the block entry; every guest entry point inside
  the extents the block was translated from (so it lies in the guest pages the block is
  registered under and is removed by self-modifying-code invalidation); every host entry
  point after the block header and before the tail, instruction-aligned.
- Relocations: known type, write fully inside the code, symbol index known, register
  index below 32, patchable sites inside the guest bytes that were hashed, thunk
  relocations only if the embedder has a thunk handler.
- Guest bytes are read only inside the range the embedder reports as mapped
  (`QueryGuestExecutableRange`); the test runner now reports its real mappings.

Any failure counts as a rejection (`TranslationCounters::CacheEntriesRejected`) and the
block is compiled instead. `Scripts/macos_code_cache_tests.py` damages files in 37 ways
(zero length, truncation at several points, bit flips in headers, code, entry points,
relocations and the index, length fields set huge), runs 300 randomly damaged caches,
writes re-sealed entries with a correct hash but bad entry points or tails, and appends
index records with odd, unordered or oversized extents; it requires the right result
every time.

What the checks cannot do: tell a well-formed entry with valid-looking entry points
inside the code from one whose entry points were moved to the middle of an instruction
sequence. Only someone who can write the file can produce that (see the trust model).

### Trust model, stated plainly

A cache file is executable code. Whoever can write it can make this process run anything.
The rules are about who can write it, and are enforced on the open descriptors, never by
path:

- **Base directory** (`DiskCachePath` or the default): opened with `O_NOFOLLOW` (a
  symbolic link is refused), must be a directory owned by this user, with no group or
  other write bit and no ACL entry that allows anyone to write, append, add or delete
  entries, change attributes, security or owner. Created 0700 if missing. Otherwise the
  cache is off for the run, with a log line.
- **Bucket directory:** created with `mkdirat` 0700 and opened with `openat(O_NOFOLLOW)`
  relative to the base directory's descriptor. It must be owned by this user, have no
  group or other bits at all, and no such ACL entry. Otherwise the cache is off for the
  run. It is never narrowed with `chmod`.
- **Cache files:** opened with `openat(O_NOFOLLOW)` relative to the bucket's descriptor
  (new files are created 0600). Each must be a regular file with one link, owned by this
  user, with no group or other bits and no such ACL entry. A refused read-write database
  is removed (`unlinkat` in the verified bucket, which only this user can change) and
  started again, once; a refused read-only database is not used. Files are never
  narrowed with `fchmod`.
- Deny ACL entries are accepted (macOS puts `everyone deny delete` on some folders).
- Directories above the base directory are not checked.
- The integrity hash is **not a security boundary**. It is an unkeyed checksum: any
  process running as the same user can rewrite a cache file and its hash. The cache
  protects against accidents and against other users, not against code already running
  with the user's rights (which could equally patch the program itself).

## Writing files: atomicity

The first design asked for write-to-temporary, `fsync`, rename. That fits a cache written
once as a whole; upstream's database is append-only and shared, so it is kept and made
safe instead:

- Every append takes an exclusive `flock` on both files; the blob is written before its
  index record, so a reader never sees an index record for a blob that is not there.
- Opening a read-write database takes both locks, reads the index once, cuts a partly
  written last index record (a process that died mid-append), and checks the
  description.
- Every append first does the same for whatever other processes appended since this
  process last looked (under the locks, reading only those bytes). Without it, a
  process that died mid-append (the crashing instruction tests do, with their writer
  thread busy) left a partial record that later appends went behind, and the next open
  cut them all off: warm runs then compiled a few blocks at random.
  (`FEXCoreCacheTest --tear-index` writes such a record after opening.)
- A database is emptied by replacing its two files (`unlinkat` and create), never by
  truncating them, so a process still appending to the old files never sees records it
  wrote move.
- **Size limit and generations:** `DiskCacheMaxFileSize` applies to each file; fexmac sets
  it to 256MB unless the embedder passes the option. Upstream enforced it only with a file
  mapper (macOS embedders install none) and then stopped storing for good; here each
  append checks both files' real sizes under the locks, and when the current file is full
  the cache continues in the next generation (`RWCacheDB_<hash>-<n>`), still reads the
  previous generation and deletes the ones before it (their names only: a process that
  still has one open keeps reading it). Processes find the newest generation by listing
  the bucket. A database larger than this process's limit (filled by a process with a
  larger one) is read but not written by this process, never emptied, so processes with
  different limits do not destroy each other's data. Generations and other processes: a cache
  file whose name was removed (link count 0) or now names another file, while it was
  being opened or locked, is a race with a process deleting an old generation; the open
  looks at the directory again instead of calling the file untrusted or damaged, a file
  is removed only if its name still refers to the file that was judged, and a new
  database's description is written before its locks are let go. APFS can show a link
  count of 2 for a while on a name being deleted, so a link count other than 1 is a
  second name only if the name still leads to the file. A database whose two files fail
  their checks is looked at again before it is called damaged (an old cache file paired
  with a new index while a generation was being replaced looks the same). Six processes
  rotating one database 72 times report no untrusted or damaged file. A generation number
  is believed only if it is the lowest found or its predecessor exists (at most 2^24); a
  planted name far ahead is ignored. Which generations may exist: with N the newest
  believed generation, processes write N and read N - 1; older ones are stale (left by a
  process killed during a rotation, or one that recreated a name from an old listing).
  Every open and every rotation removes the stale generations it finds, at most 16 per
  call and each only while holding both of its locks without waiting (so never during
  another process's write); an open that would create a generation already stale looks
  again and removes it if empty; a process whose generation was removed notices before its
  next write (the names no longer lead to its files) and moves to N. So once no process is
  opening or rotating, any open leaves exactly N - 1 and N, whatever the gaps. (The
  orphans the physical Mac job found came from listing the bucket twice in one process:
  the listing used a duplicate of the bucket's descriptor, which shares its read position,
  so every listing after the first saw nothing and a process restarted from generation 0.
  Each listing now rewinds first.) The next generation is opened outside the index lock and then swapped in; if
  it is over this process's limit, saving stops for the run (read-only) instead of
  rotating again. Why 256MB: every process reads the
  whole index of the generations it uses at start (about 40% of the cache file for the
  instruction tests' small blocks; less for real programs' larger ones), so the limit
  bounds start-up time and memory; the instruction tests use 59MB in all.
- **A stalled lock holder:** a store waits for another process's lock at most once per
  process (5 s, measured against the clock, not counted in sleeps). After a timeout every
  later store of the run is dropped and counted (`CacheEntriesDropped`), so a flush at exit
  returns after about one wait however many entries were queued. Tested with a process
  that holds the locks sleeping, and one stopped with SIGSTOP while holding them: 59
  entries dropped, flush 5.0 s.
- A torn or damaged blob that an index record does point to fails the integrity check.
- `Context::FlushDiskCache()` waits for the writer thread to finish every queued store and
  then calls `fcntl(F_FULLFSYNC)` (the macOS way through the drive cache) on both files,
  skipping the sync only if no store was queued since the last one (stores count
  themselves after they are queued, and the count is read before the flush marker is,
  under a mutex). Entries queued but not flushed when a process exits are lost, never
  half-used.
- While choosing a candidate entry, the lookup copies it under the index lock; the
  writer thread fills in the file offset of a just-stored entry under the same lock.

### Reading the index at start-up

Upstream reads the whole index and builds the full in-memory index (one allocation per
entry) when a process starts. With the instruction tests' 85,000-entry jit_1 database
(10.7MB of index) that took about 9ms per process: 1.7ms reading the file for the
torn-record check and 7.3ms reading it again and parsing it. Each test uses a few dozen
entries.

Chosen: read the index once (under the locks) and build only a flat open-addressed table
of (lookup key, record offset), 16 bytes a slot with no allocation per entry; a record is
parsed (and checked) the first time its key is looked up, the records of one key in file
order, as upstream adds them. This keeps upstream's file
format and its index code above the lookup unchanged, and it costs the same whatever the
size of the database. Not chosen: mapping the index (another process may truncate it,
which would turn a read into SIGBUS), an on-disk sorted or hashed index (a format of our
own, and a rewrite to keep sorted under concurrent appends), or a per-database cap with
rotation (it limits the cost by throwing away entries a long-running program would want).

### fork

`pthread_atfork` handlers (registered once; they look after the context's cache) take the
disk cache's index and in-memory-cache locks across every fork, so the child never
inherits one held by the writer thread. `Context::LockBeforeFork` / `UnlockAfterFork`
(fexmac's `fexmac_before_fork` / `fexmac_after_fork`) take them too, after the code
invalidation lock, and the handlers then leave them. In the child, which has no writer
thread and shares the parent's open files (and so their `flock` locks), writing is
switched off: the child keeps loading from the cache but stores nothing, `FlushDiskCache`
returns at once, and a fresh work queue replaces the abandoned one. A process id check at
flush and store catches a fork the handlers did not see. `FEXCoreCacheTest --fork` and
`--fork-raw` (without the fexmac calls) fork with a warm and a cold cache; the child runs
guest code and exits.

### What the cache reports

`fexmac_cache_state`: `OFF` (not requested); `ACTIVE`; `READ_ONLY` (a fork child, or the
database is over this process's limit); `UNTRUSTED` (the directory or bucket is not
private to the user, has an ACL entry granting write, or is a link; the cache is off);
`UNAVAILABLE` (the database stayed locked at open, the directory cannot be created or
written, an I/O error, or no build identity; the cache is off). Counters besides: entries
dropped, untrusted cache files removed and started again (the cache is still `ACTIVE`
after that), damaged databases emptied.


- Several processes and threads can use one database at once: this is what the
  instruction-test run does (six processes per mode, four modes, one directory). Lock
  waits are 5 s on macOS (upstream: 0.1 s to open, 1 s to store): on a busy machine a
  lock holder can be descheduled. A timeout at open leaves the cache `UNAVAILABLE`; at a
  store it drops that and every later store of the run (above).
- Workaround, with no native alternative: when several processes create the same new
  file at once, APFS can fail a loser's `openat(O_CREAT)` with ENOENT although the
  directory exists (a ten-line C program shows it about one round in five with six
  processes; with or without `O_NOFOLLOW`). Such a create is retried for up to 0.1 s.
- Files are read with `pread`; 16K pages play no part in the format.

Stale buckets: upstream removes every other bucket directory when a process starts. With
processes of different builds or host-feature settings running together (the instruction
tests set host features per test) they deleted each other's caches mid-run. On macOS a
bucket directory is only removed if it is named like a bucket (16 hex digits, so a cache
path that holds other things is safe) and it has not been used for seven days. Use, not
writes: a process that opens a bucket touches its `last-used` file (one `futimens` per
start), so a cache in daily read-only use is kept. Pruning lists, checks and removes
through the verified base directory's descriptor (`fdopendir`, `fstatat`, `openat`,
`unlinkat`), never by path.

## Where files live

`FEX_DISKCACHEPATH`, or by default `~/Library/Caches/fex-emu/DiskCache/` on macOS (the
system's disposable per-user cache location; `FEX_APP_CACHE_LOCATION` still overrides).
Inside: `<machine bucket>/RWCacheDB_<process bucket>.foz` and `..._idx.foz`.

## The embedder's side

Through `fexmac` (`docs/macOS-embedding-interface.md`, "Cached translations and guest
code ranges"): `fexmac_config.code_cache_enabled` and `code_cache_directory` switch the
cache on (the core's `DISKCACHE`/`DISKCACHEPATH` options stay refused);
`fexmac_memory_map` registers the guest's code so that the cache hashes and patches
only mapped bytes; `fexmac_context_flush_cache` before exiting;
`fexmac_context_get_counters` for the counters and the cache state (a refused directory
shows as `FEXMAC_CACHE_UNTRUSTED`, not as an error; see "What the cache reports"); `fexmac_before_fork` /
`fexmac_after_fork` around `fork()`. Under it, the core's `Context::FlushDiskCache`,
`GetTranslationCounters`, `LockBeforeFork`/`UnlockAfterFork` and the syscall handler's
`QueryGuestExecutableRange`. The core's writer thread uses the core's default pthread
implementation on macOS.

## Measured

See `docs/macOS-port-status.md` for the numbers (time for the instruction tests cold and
warm, bytes on disk).

## Open questions

1. Ahead-of-time translation of a whole binary using the same format (upstream's offline
   code-map pipeline is not built on macOS).
2. Sharing read-only databases between users (`DiskCacheRODBNames`) needs a different trust
   model and is not supported here: read-only databases must belong to the user.
