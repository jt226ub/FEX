# macOS: design for the on-disk code cache

Status: design only. Nothing here is implemented. On macOS, loading cached code stops
with an error (`CodeCache.cpp`, `FinalizeCodePages`).

## What the cache is for

Translating x86-64 code takes time and causes stutter the first time each piece of a game
runs. The cache saves translated code to disk so the next run can reuse it. This is the
"translate once, keep it" goal of the wider project.

## How upstream does it on Linux

- While a program runs, FEX records which guest code it translated (the code map).
- A cache file per guest binary section holds translated code plus relocations: the places
  in that code that depend on where things are in memory this run.
- To load, FEX maps the cache file, applies the relocations, then uses `mremap` to move
  the pages into place as executable code.

## Why that cannot be copied on macOS

1. macOS has no `mremap`.
2. Executable memory for generated code must come from a `MAP_JIT` mapping. A `MAP_JIT`
   mapping is always anonymous: it cannot be backed by a file and existing pages cannot
   be moved into it.
3. Mapping a file as executable directly would need the file to be code-signed, or the
   program to hold an entitlement that switches off the protection the port is built
   around. That is a workaround, and the project rule is not to use one when a native
   method exists.

## Proposed design

Load by copying, not by mapping.

1. Open and validate the cache file (see "Keys" below).
2. Reserve space in the thread's existing `MAP_JIT` code buffer, the same buffer freshly
   translated code goes into.
3. Inside a `ScopedJITWrite`, read the cached code straight into that space with `pread`,
   then apply the relocations in place.
4. End the write scope and flush the instruction cache for the range.
5. Register the blocks in the lookup cache exactly as the Linux path does.

Costs and benefits compared with Linux:

- Cost: the code is copied, so its pages are private to each process and are not shared
  or dropped by the system the way file-backed pages are. For one game process this does
  not matter.
- Benefit: cached code and new code live in one buffer, so block linking between them
  needs nothing special.
- Unchanged: the file format and the relocation logic. Only the last step differs.

Loading should be incremental: copy a section's code when the guest first maps that
section, not everything at start-up.

## Keys

A cache file is only valid for the exact combination that produced it. The key must
cover all of:

- the guest binary (content hash, not path or date),
- the translator build (a build identifier of this port),
- host CPU features the translator used,
- every configuration option that changes generated code (memory-ordering mode,
  multiblock, and so on).

A file with any mismatch is ignored and rebuilt. A corrupt or truncated file must be
detected before any of it is copied into executable memory.

## Where files live

`~/Library/Caches/<bundle or tool name>/code/`, which macOS treats as disposable. Files
are written to a temporary name and renamed into place so a crash cannot leave a
half-written cache.

## Security notes

- Cached code is executed, so a cache file is as sensitive as a program. Files are created
  readable and writable by the user only.
- The cache directory must not be shared between users.

## Open questions

1. How much of upstream's `CodeCache.cpp` can be kept unchanged above the final step.
   The file is about 1,000 lines and the answer needs a careful read.
2. Whether relocations can be applied while copying, to avoid touching each page twice.
3. Ahead-of-time translation of a whole binary before first run, using the same file
   format. Upstream has an offline code-map pipeline; whether its tooling builds on
   macOS is not yet known.

## Test plan

- Run the instruction tests twice against one cache directory and require identical
  results, with the second run reporting that it compiled nothing.
- Corrupt a cache file and require a clean fallback to translation.
- Change a configuration option and require the old file to be ignored.
