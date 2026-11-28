# macOS port: bringing in upstream FEX

The port follows upstream FEX-Emu/FEX by merging it, never by rebasing or squashing, so
each merge commit has upstream as a parent and the next sync starts from the right merge
base. Nothing is ever sent upstream: no push, pull request, issue or comment.

## Branches

- **`main`** in this fork carries the port. It is no longer a mirror of upstream; it
  moves only when the owner merges `macos/v1` into it.
- **`macos/v1`** is the integration branch for further work, and the port branch the
  sync script uses by default. A sync is merged on a branch made from `macos/v1`, checked
  there, merged into `macos/v1`, and reaches `main` with the owner's next merge.
- **Upstream** is tracked only through the fetch-only remotes `upstream` and
  `upstream-readonly` (their push addresses are `DISABLED-never-push-upstream`). The
  script fetches upstream's `main` from the fixed address
  `https://github.com/FEX-Emu/FEX.git` into `refs/remotes/upstream-readonly/main`; it
  never reads this fork's `main` or `origin/main` as upstream.
  `Scripts/macos_sync_upstream_test.sh` checks this with a local `main` that carries the
  port and differs from upstream.

## Upstream merged so far

| Date | Upstream `main` | Merge base before | New upstream commits | Branch |
|---|---|---|---|---|
| 27 November 2026 | `a92baf073` | `3ee253992` | 11 (7 without merge commits) | `macos/wp-h-upstream-sync` |

## The script

```
Scripts/macos_sync_upstream.sh [--here | --branch NAME] [--dry-run | --harden-only] [port-branch] [upstream-ref]
```

Defaults: port branch `macos/v1`, upstream ref `main`. It:

1. **Makes upstream unpushable.** Any remote with a fetch or push address that names
   FEX-Emu/FEX has all its push addresses removed and replaced by one invalid address
   (`DISABLED-never-push-upstream`). The match ignores letter case, `.git` and trailing
   slashes, and covers https, ssh (with or without a port) and scp-style addresses. It
   checks both the configured addresses and the addresses after `insteadOf` and
   `pushInsteadOf` rewriting, so a fork remote whose push address leads upstream is
   caught too. Every remote's push addresses are then resolved again, and the script
   stops with status 3 if any can still reach FEX-Emu/FEX. An existing
   `upstream-readonly` remote must point only at FEX-Emu/FEX, or the script stops with
   status 3. `--harden-only` does this step alone and reports each change.
2. **Checks what needs no network**: a clean tree (untracked files are allowed), that
   the port branch exists, and for `--here` a checked-out branch that contains the port
   branch.
3. **Fetches** the upstream ref from the fixed address `https://github.com/FEX-Emu/FEX.git`,
   not through a remote, into `refs/remotes/upstream-readonly/<ref>`. It refuses if a
   `url.*.insteadOf` rule would send that fetch elsewhere. A named ref is used instead of
   `FETCH_HEAD`, which each worktree keeps separately. A fetch-only `upstream-readonly`
   remote is added if missing.
4. **Reports** the new upstream commits (all of them, and how many are not merge
   commits), the files both sides changed, submodule pointers upstream moved,
   instruction tests upstream added, changed or renamed, and the upstream source files
   to review. It refuses if an untracked file sits where upstream adds a file.
5. **Merges** with `--no-ff`. Without options it first creates `sync/upstream-<hash>`
   from the port branch in the current worktree. With `--here` it merges into the branch
   checked out in this worktree.

`--dry-run` does steps 1 to 4 but fetches into a temporary ref that is deleted on exit.
It creates no branch, remote or lasting ref and merges nothing. The only change it can
make is step 1's safety change, which it lists. The fetched objects stay in the object
store.

Exit status: 0 merged or nothing to do; 1 refused before merging, or the merge did not
start (for example because of an untracked file git would overwrite), with git's reason
printed and nothing merged; 2 the merge stopped on conflicts, with the conflicted files
listed and the merge left in progress to resolve; 3 a remote could still push upstream,
or `upstream-readonly` or the fetch address points elsewhere.

It works from any worktree and never checks out the port branch itself, so it runs while
another worktree has `macos/v1` checked out. It never pushes. Passing `main` as the port
branch works too, but is not the procedure: syncs go through `macos/v1`.

`Scripts/macos_sync_upstream_test.sh` tests it offline, in the Mac job too. It builds
throwaway repositories under a temporary path with spaces and runs every case under
macOS's bash 3.2: remotes in each address form, every refusal, the dry run, a clean
merge, a conflict, and a merge that does not start. For the merge cases the environment
variable `MACOS_SYNC_TEST_UPSTREAM_URL` stands in a local repository for upstream; it is
a test hook only and the script says so when it is set.

## Resolving conflicts

Expected port changes in upstream's own files, besides the source changes:

- **Upstream's workflows** (`ccpp.yml`, `glibc_fault.yml`, `hostrunner.yml`,
  `instcountci.yml`, `mingw_build.yml`, `steamrt4.yml`, `vixl_simulator.yml`,
  `wine_dll_artifacts.yml` under `.github/workflows/`) each have one added line per job,
  `if: github.repository == 'FEX-Emu/FEX'`, after the job's `runs-on:`, in the style of
  upstream's own `pr-code-format.yml`. With the port on this fork's `main` they would
  otherwise start on every push to `main`, wait 24 hours for upstream's self-hosted
  runners and fail, and a self-hosted label must never be matchable from this public
  fork. When upstream changes one of these files, expect a conflict or a merged file
  without the guard: keep upstream's change and keep (or put back) the guard on every
  job, including any job upstream adds. After the sync, check that every job of every
  workflow that is not the port's (`macos_core.yml`, `linux_core_arm64.yml`) has the
  guard, for example with
  `python3 -c "import yaml,glob; [print(f, j) for f in glob.glob('.github/workflows/*.yml') if not f.endswith(('macos_core.yml', 'linux_core_arm64.yml')) for j, v in yaml.safe_load(open(f))['jobs'].items() if v.get('if') != \"github.repository == 'FEX-Emu/FEX'\"]"`,
  which prints nothing when all are guarded.
- **`README.md`** has a notice at the top saying what this fork is; keep it above
  upstream's text.

When the merge itself conflicts:


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
