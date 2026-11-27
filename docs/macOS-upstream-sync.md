# macOS port: bringing in upstream FEX

The port follows upstream FEX-Emu/FEX by merging it, never by rebasing or squashing, so
each merge commit has upstream as a parent and the next sync starts from the right merge
base. Nothing is ever sent upstream: no push, pull request, issue or comment.

## Upstream merged so far

| Date | Upstream `main` | Merge base before | New upstream commits | Branch |
|---|---|---|---|---|
| 27 November 2026 | `a92baf073` | `3ee253992` | 11 (7 without merges) | `macos/wp-h-upstream-sync` |

## The script

```
Scripts/macos_sync_upstream.sh [--here | --branch NAME] [--dry-run] [port-branch] [upstream-ref]
```

Defaults: port branch `macos/v1`, upstream ref `main`. It:

1. Sets an invalid push address (`DISABLED-never-push-upstream`) on every remote whose
   address is FEX-Emu/FEX, and keeps a fetch-only remote `upstream-readonly`.
2. Fetches the upstream ref into `refs/remotes/upstream-readonly/<ref>`. A named ref is
   used instead of `FETCH_HEAD`, which each worktree keeps separately.
3. Prints the new upstream commits, the files both sides changed, submodule pointers
   upstream moved, instruction tests upstream added or changed, and the upstream source
   files to review.
4. Merges with `--no-ff`. Without options it first creates `sync/upstream-<hash>` from
   the port branch in the current worktree. With `--here` it merges into the branch
   checked out in this worktree, which must already contain the port branch (a package
   branch made from `macos/v1`, for example). With `--dry-run` it stops after step 3.
5. On conflict it stops with status 2 and lists the conflicted files.

It works from any worktree and never checks out the port branch itself, so it runs while
another worktree has `macos/v1` checked out. It never pushes.

## Resolving conflicts

- Keep the macOS changes. Where upstream rewrote code the port also changed, take
  upstream's new code and apply the port's intent to it again. Do not keep the port's old
  copy of the function.
- Finish with `git commit` so the result stays a merge commit.
- Most port changes sit behind `__APPLE__` or `if (APPLE)`; keep those guards so Linux
  behaviour stays upstream's.

## What to re-check in every upstream change

Read every upstream change to `FEXCore`, `Source/Common`, `Source/Tools`, `CodeEmitter`,
`FEXHeaderUtils` and the top-level `CMakeLists.txt` for these, each of which has caused a
crash or hang on a physical Mac:

- New writes to code memory (emitting, patching, linking, the block tail's data) need
  `FEXCore::Allocator::ScopedJITWrite`, which must not be nested.
- New `mprotect`, `mremap` or other Linux-only calls need a macOS path. `mprotect` is
  refused on `MAP_JIT` pages.
- New uses of `x18`, which macOS reserves.
- New assumptions of 4K host pages (macOS uses 16K).
- New Linux headers (`linux/...`, `sys/...` that macOS lacks).
- New guest memory below 4GB: instruction tests with fixed addresses are relocated by
  `Scripts/macos_asm_tests.py` and `HarnessHelpers.h`, which must agree.
- Build rules: `CMakeLists.txt` must still stop after the core-only targets on macOS,
  use the bundled `fmt` and `range-v3`, and define `META_NO_STD_FORWARD_DECLARATIONS` for
  range-v3 (needed for the macOS 27 SDK).

## Submodules

If the script reports moved submodules, update the ones the macOS build uses:

```
git submodule update --init --depth 1 External/fmt External/range-v3 External/rpmalloc \
  External/unordered_dense External/xxhash Source/Common/cpp-optparse
```

(`git submodule status` shows which are initialised; the others are not needed here.)

## Verifying

Build with both toolchains, sign for the hardened runtime and run everything, as in
`docs/macOS-port-status.md`:

```
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DENABLE_LTO=OFF -DENABLE_CCACHE=OFF
ninja -C build FEXCore FEXCore_shared Common CommonTools FEXCoreSmokeTest FEXCoreThreadTest FEXCoreASMTestRunner
codesign --force --sign - --options runtime --entitlements Data/macOS/jit.entitlements <copy of each binary>
<smoke test> ; <thread test>
python3 Scripts/macos_asm_tests.py --runner <signed FEXCoreASMTestRunner> --out asm-out
```

Repeat with `DEVELOPER_DIR` set to Xcode. Every mode must report zero unexpected failures.
New upstream instruction tests that fail must be diagnosed: fix the port, or, only when
the cause is a documented macOS limit (no memory below 4GB, Linux system calls), add the
test to `unittests/ASM/Known_Failures_macOS` with the verified reason. Update the counts
in `docs/macOS-port-status.md` and add a row to the table above.

## The 27 November 2026 sync

Merged cleanly; no conflicts and no submodule moves. Each upstream commit and what it
needed on macOS:

| Upstream commit | Change | macOS review |
|---|---|---|
| `c87ff7d02` Vector: Amend MMX PUNPCKL memory op handling | IR generation: the MMX memory form loads 32 bits | IR only, no host-specific code. Nothing needed. |
| `604f0188f` OpcodeDispatcher: Lower SHUFPS with matching sources as PSHUFD | IR generation | IR only. Nothing needed. |
| `257703d10` InstCountCI: Update | Expected instruction counts, `DiskCache.h` format version 47 to 49 | Instruction-count tests are not run on macOS; the version bump only invalidates old caches. Nothing needed. |
| `130a6c0e7` Config: Reenable FEX global config under Steam config situations | `Source/Common/Config.cpp`, behind `FEX_STEAM_SUPPORT` | Steam support is not built on macOS, so behaviour there is unchanged. Merged beside the port's `limits.h` change without conflict. Nothing needed. |
| `353f54f44` Steam: Adds AppConfig support | Top-level `CMakeLists.txt` adds `Source/Steam/AppConfig` | Placed after the macOS core-only `return()`, so it is never reached on macOS. Nothing needed. |
| `37530900c` Steam/AppConfig: Add Assassin's Creed | JSON config files | Not built on macOS. Nothing needed. |
| `b5a4954f2` unittests/ASM: Adds new shufps test for single-source | New test `FEX_bugs/shufps_single_source` | Uses only relative addresses. Passes in all four modes with both toolchains. |

No upstream change writes code memory, calls `mprotect` or `mremap`, uses `x18`, assumes
4K pages or adds a Linux header. With the macOS 27 SDK and with Xcode 26.5, as built and
hardened: smoke and thread tests pass; instruction tests in every mode pass 2,255
(baseline 2,254 plus the new test), skip 8, 18 expected failures (crash 12, fail 5,
timeout 1), 0 unexpected.
