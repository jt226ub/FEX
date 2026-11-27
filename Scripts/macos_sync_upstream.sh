#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Brings new upstream FEX commits into the macOS port as a real merge commit, without
# ever sending anything upstream. The full procedure (conflicts, what to re-check, how to
# verify) is in docs/macOS-upstream-sync.md.
#
#   Scripts/macos_sync_upstream.sh [options] [port-branch] [upstream-ref]
#
#   port-branch   the port's integration branch (default macos/v1)
#   upstream-ref  the upstream branch or tag to merge (default main)
#
#   --here        merge into the branch checked out in this worktree instead of creating
#                 a new branch. The checked-out branch must already contain the port
#                 branch (for example a package branch made from it).
#   --branch NAME create NAME from the port branch and merge there
#                 (default sync/upstream-<short upstream hash>)
#   --dry-run     fetch and report what would be merged; change nothing
#
# What it does:
#   1. Makes sure every remote that points at FEX-Emu/FEX cannot be pushed to, and keeps
#      a fetch-only remote, upstream-readonly, for the fetch.
#   2. Fetches the upstream ref (read only) into refs/remotes/upstream-readonly/<ref>.
#   3. Reports the new upstream commits, the files both sides changed, any submodule
#      pointers that moved and any new or changed instruction tests.
#   4. Merges the upstream ref with --no-ff, so the merge commit records upstream as a
#      parent and the next sync finds the right merge base. Never a squash or a rebase.
#   5. On conflict, stops and lists the conflicted files (exit status 2).
#
# It works from any worktree of the repository: it only touches the current worktree,
# and it never checks out a branch that another worktree has checked out.
#
# It never pushes, never opens a pull request and never touches the upstream repository.
set -euo pipefail

UPSTREAM_URL="https://github.com/FEX-Emu/FEX.git"
DISABLED_PUSH_URL="DISABLED-never-push-upstream"

MODE="new-branch"
SYNC_BRANCH=""
DRY_RUN=0
POSITIONAL=()
while [ $# -gt 0 ]; do
  case "$1" in
    --here) MODE="here" ;;
    --branch) shift; SYNC_BRANCH="${1:?--branch needs a name}" ;;
    --dry-run) DRY_RUN=1 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "Unknown option: $1" >&2; exit 1 ;;
    *) POSITIONAL+=("$1") ;;
  esac
  shift
done
PORT_BRANCH="${POSITIONAL[0]:-macos/v1}"
UPSTREAM_REF="${POSITIONAL[1]:-main}"

if [ "$MODE" = "here" ] && [ -n "$SYNC_BRANCH" ]; then
  echo "--here and --branch cannot be used together." >&2
  exit 1
fi

cd "$(git rev-parse --show-toplevel)"

if [ "$DRY_RUN" -eq 0 ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "The working tree has uncommitted changes. Commit or stash them first." >&2
  exit 1
fi

if ! git rev-parse --verify --quiet "$PORT_BRANCH^{commit}" >/dev/null; then
  echo "Port branch $PORT_BRANCH does not exist." >&2
  exit 1
fi

# Never-push guarantee: every remote whose address is the upstream repository gets an
# invalid push address, so even a mistaken "git push <that remote>" cannot reach it.
for REMOTE in $(git remote); do
  URL="$(git remote get-url "$REMOTE")"
  case "$URL" in
    *[/:]FEX-Emu/FEX|*[/:]FEX-Emu/FEX.git|*[/:]FEX-Emu/FEX/)
      if [ "$(git remote get-url --push "$REMOTE")" != "$DISABLED_PUSH_URL" ]; then
        echo "Disabling pushes to remote $REMOTE ($URL)."
        git remote set-url --push "$REMOTE" "$DISABLED_PUSH_URL"
      fi
      ;;
  esac
done
if ! git remote get-url upstream-readonly >/dev/null 2>&1; then
  git remote add --no-tags upstream-readonly "$UPSTREAM_URL"
fi
git remote set-url --push upstream-readonly "$DISABLED_PUSH_URL"

# Fetch into a named ref rather than FETCH_HEAD, which each worktree keeps separately.
UPSTREAM_TRACKING="refs/remotes/upstream-readonly/$UPSTREAM_REF"
git fetch --no-tags upstream-readonly "+$UPSTREAM_REF:$UPSTREAM_TRACKING"
UPSTREAM_FULL="$(git rev-parse "$UPSTREAM_TRACKING")"
UPSTREAM_SHA="$(git rev-parse --short=9 "$UPSTREAM_FULL")"

if [ "$MODE" = "here" ]; then
  TARGET="$(git symbolic-ref --quiet --short HEAD || true)"
  if [ -z "$TARGET" ]; then
    echo "--here needs a checked-out branch; this worktree has a detached HEAD." >&2
    exit 1
  fi
  if ! git merge-base --is-ancestor "$PORT_BRANCH" HEAD; then
    echo "$TARGET does not contain $PORT_BRANCH. Update it from $PORT_BRANCH first," >&2
    echo "or run without --here to start a new branch from $PORT_BRANCH." >&2
    exit 1
  fi
  TARGET_TIP="HEAD"
else
  TARGET="${SYNC_BRANCH:-sync/upstream-$UPSTREAM_SHA}"
  TARGET_TIP="$PORT_BRANCH"
fi

BASE_SHA="$(git merge-base "$TARGET_TIP" "$UPSTREAM_FULL")"
NEW_COMMITS="$(git rev-list --count "$BASE_SHA..$UPSTREAM_FULL")"

echo "Port branch:      $PORT_BRANCH ($(git rev-parse --short=9 "$PORT_BRANCH"))"
echo "Merge into:       $TARGET ($MODE)"
echo "Upstream $UPSTREAM_REF:    $UPSTREAM_SHA"
echo "Merge base:       $(git rev-parse --short=9 "$BASE_SHA")"
echo "New upstream commits since the merge base: $NEW_COMMITS"
if [ "$NEW_COMMITS" -eq 0 ]; then
  echo "Nothing to do."
  exit 0
fi

echo
echo "Upstream commits (excluding merges):"
git log --no-merges --format='  %h %s' "$BASE_SHA..$UPSTREAM_FULL"

echo
echo "Files that the port and upstream both changed (most likely to conflict):"
comm -12 \
  <(git diff --name-only "$BASE_SHA" "$TARGET_TIP" | sort) \
  <(git diff --name-only "$BASE_SHA" "$UPSTREAM_FULL" | sort) | sed 's/^/  /'

echo
echo "Submodule pointers upstream moved:"
SUBMODULES="$(git diff --raw "$BASE_SHA" "$UPSTREAM_FULL" | awk '$1 == ":160000" || $2 == "160000" {print "  " $NF}')"
echo "${SUBMODULES:-  (none)}"

echo
echo "Instruction tests upstream added or changed (run them):"
TESTS="$(git diff --name-only --diff-filter=AM "$BASE_SHA" "$UPSTREAM_FULL" -- unittests/ASM | sed 's/^/  /')"
echo "${TESTS:-  (none)}"

echo
echo "Upstream source changes to check for the macOS traps (code-memory writes need"
echo "ScopedJITWrite, mprotect/mremap/Linux-only calls, x18, 4K pages, Linux headers):"
git diff --name-only "$BASE_SHA" "$UPSTREAM_FULL" -- FEXCore Source CodeEmitter FEXHeaderUtils CMakeLists.txt \
  | sed 's/^/  /'

if [ "$DRY_RUN" -eq 1 ]; then
  echo
  echo "Dry run: nothing changed."
  exit 0
fi

if [ "$MODE" = "new-branch" ]; then
  if git rev-parse --verify --quiet "refs/heads/$TARGET" >/dev/null; then
    echo "Branch $TARGET already exists. Pass --branch with another name, or delete it." >&2
    exit 1
  fi
  # switch -c checks out only in this worktree; the port branch itself is never checked
  # out, so this works while another worktree has it.
  git switch -c "$TARGET" "$PORT_BRANCH"
fi

MSG="Merge upstream FEX $UPSTREAM_REF at $UPSTREAM_SHA into the macOS port

Upstream FEX-Emu/FEX $UPSTREAM_REF $UPSTREAM_FULL, $NEW_COMMITS new commits since
$(git rev-parse --short=9 "$BASE_SHA"). Made by Scripts/macos_sync_upstream.sh."

if git merge --no-ff -m "$MSG" "$UPSTREAM_FULL"; then
  echo
  echo "Merged cleanly into $TARGET."
else
  echo
  echo "Merge stopped on conflicts in:"
  git diff --name-only --diff-filter=U | sed 's/^/  /'
  echo "Resolve them on $TARGET keeping the macOS changes (see docs/macOS-upstream-sync.md),"
  echo "then 'git commit' to finish the merge. Do not squash or rebase it."
  exit 2
fi

echo
echo "Next, as described in docs/macOS-upstream-sync.md:"
echo "  git submodule update --init --depth 1 <each moved submodule the build uses>"
echo "  build, then run the smoke, thread and instruction tests as built and hardened"
echo "Nothing has been pushed. Never push to or open a pull request against FEX-Emu/FEX."
