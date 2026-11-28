#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Brings new upstream FEX commits into the macOS port as a real merge commit, without
# ever sending anything upstream. The full procedure (conflicts, what to re-check, how to
# verify) is in docs/macOS-upstream-sync.md.
#
#   Scripts/macos_sync_upstream.sh [options] [port-branch] [upstream-ref]
#
#   port-branch   the port's integration branch (default macos/v1). The fork's main also
#                 carries the port, but syncs land on macos/v1 first and reach main only
#                 by the owner's merge.
#   upstream-ref  the upstream branch or tag to merge (default main: upstream's main,
#                 fetched from FEX-Emu/FEX; this repository's own main is never used as
#                 upstream)
#
#   --here         merge into the branch checked out in this worktree instead of creating
#                  a new branch. The checked-out branch must already contain the port
#                  branch (for example a package branch made from it).
#   --branch NAME  create NAME from the port branch and merge there
#                  (default sync/upstream-<short upstream hash>)
#   --dry-run      make remotes unable to push upstream (step 1, every change reported),
#                  fetch upstream into a temporary ref that is deleted on exit, and report
#                  what would be merged. No branch, remote or lasting ref is created and
#                  nothing is merged. The fetched objects stay in the object store.
#   --harden-only  do step 1 only, then exit
#
# What it does:
#   1. Never-push hardening. Every remote that has any fetch or push address naming
#      FEX-Emu/FEX (in any letter case, https, ssh or scp form, with or without .git or a
#      trailing slash, after insteadOf/pushInsteadOf rewriting) has all its push
#      addresses removed and replaced by one invalid address. Then every remote's
#      effective push addresses are checked again, and the script refuses to go on if
#      any could still reach FEX-Emu/FEX. An existing upstream-readonly remote must point
#      at FEX-Emu/FEX, or the script refuses.
#   2. Fetches the upstream ref (read only) from the fixed upstream address into
#      refs/remotes/upstream-readonly/<ref>, and keeps a fetch-only remote named
#      upstream-readonly.
#   3. Reports the new upstream commits, the files both sides changed, submodule pointers
#      upstream moved, instruction tests upstream added, changed or renamed, and the
#      upstream source files to review. Refuses if an untracked file is where upstream
#      adds one.
#   4. Merges with --no-ff, so the merge commit records upstream as a parent and the next
#      sync finds the right merge base. Never a squash or a rebase.
#
# Exit status: 0 done (or nothing to do), 1 refused before merging or the merge did not
# start, 2 the merge stopped on conflicts, 3 a remote could still push upstream or
# upstream-readonly points elsewhere.
#
# It works from any worktree of the repository: it only touches the current worktree,
# and it never checks out a branch that another worktree has checked out.
#
# It never pushes, never opens a pull request and never touches the upstream repository.
set -euo pipefail

UPSTREAM_URL="https://github.com/FEX-Emu/FEX.git"
DISABLED_PUSH_URL="DISABLED-never-push-upstream"
# Test hook for Scripts/macos_sync_upstream_test.sh: fetch "upstream" from a local
# repository so the merge paths can be tested offline. The never-push checks still look
# for FEX-Emu/FEX.
if [ -n "${MACOS_SYNC_TEST_UPSTREAM_URL:-}" ]; then
  UPSTREAM_URL="$MACOS_SYNC_TEST_UPSTREAM_URL"
  echo "TEST MODE: upstream is $UPSTREAM_URL" >&2
fi

MODE="new-branch"
SYNC_BRANCH=""
DRY_RUN=0
HARDEN_ONLY=0
PORT_BRANCH=""
UPSTREAM_REF=""
NPOS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --here) MODE="here" ;;
    --branch)
      if [ $# -lt 2 ] || [ -z "$2" ]; then echo "--branch needs a name." >&2; exit 1; fi
      shift; SYNC_BRANCH="$1" ;;
    --dry-run) DRY_RUN=1 ;;
    --harden-only) HARDEN_ONLY=1 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "Unknown option: $1" >&2; exit 1 ;;
    *)
      NPOS=$((NPOS + 1))
      case "$NPOS" in
        1) PORT_BRANCH="$1" ;;
        2) UPSTREAM_REF="$1" ;;
        *) echo "Too many arguments: $1" >&2; exit 1 ;;
      esac ;;
  esac
  shift
done
PORT_BRANCH="${PORT_BRANCH:-macos/v1}"
UPSTREAM_REF="${UPSTREAM_REF:-main}"

if [ "$MODE" = "here" ] && [ -n "$SYNC_BRANCH" ]; then
  echo "--here and --branch cannot be used together." >&2
  exit 1
fi

cd "$(git rev-parse --show-toplevel)"

# --- Step 1: never-push hardening -------------------------------------------------------

# True if the address names the upstream repository on GitHub. Errs towards matching.
is_upstream_url() {
  local u
  u="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
  while :; do
    case "$u" in
      */) u="${u%/}" ;;
      *.git) u="${u%.git}" ;;
      *) break ;;
    esac
  done
  case "$u" in
    *github.com/fex-emu/fex | *github.com:fex-emu/fex | *github.com:[0-9]*/fex-emu/fex) return 0 ;;
  esac
  return 1
}

# Every address a remote has: configured and effective, fetch and push.
remote_urls() {
  git config --get-all "remote.$1.url" || true
  git config --get-all "remote.$1.pushurl" || true
  git remote get-url --all "$1" 2>/dev/null || true
  git remote get-url --push --all "$1" 2>/dev/null || true
}

HARDEN_ACTIONS=""
harden_remotes() {
  local remote url hit
  for remote in $(git remote); do
    hit=0
    while IFS= read -r url; do
      if [ -n "$url" ] && is_upstream_url "$url"; then hit=1; fi
    done < <(remote_urls "$remote")
    if [ "$hit" -eq 1 ]; then
      if [ "$(git config --get-all "remote.$remote.pushurl" || true)" != "$DISABLED_PUSH_URL" ]; then
        git config --unset-all "remote.$remote.pushurl" || true
        git config --add "remote.$remote.pushurl" "$DISABLED_PUSH_URL"
        echo "Disabled pushes on remote $remote (it names FEX-Emu/FEX)."
        HARDEN_ACTIONS="${HARDEN_ACTIONS}  set the push address of remote $remote to $DISABLED_PUSH_URL
"
      fi
    fi
  done

  # upstream-readonly must be what its name says.
  if git remote get-url upstream-readonly >/dev/null 2>&1; then
    local ok=1 count=0
    while IFS= read -r url; do
      count=$((count + 1))
      if [ -n "${MACOS_SYNC_TEST_UPSTREAM_URL:-}" ]; then
        [ "$url" = "$UPSTREAM_URL" ] || ok=0
      else
        is_upstream_url "$url" || ok=0
      fi
    done < <(git remote get-url --all upstream-readonly)
    if [ "$ok" -ne 1 ] || [ "$count" -ne 1 ]; then
      echo "Refusing: remote upstream-readonly does not point only at $UPSTREAM_URL:" >&2
      git remote get-url --all upstream-readonly | sed 's/^/  /' >&2
      echo "Fix or remove it (git remote remove upstream-readonly) and run again." >&2
      exit 3
    fi
    if [ "$(git config --get-all remote.upstream-readonly.pushurl || true)" != "$DISABLED_PUSH_URL" ]; then
      git config --unset-all remote.upstream-readonly.pushurl || true
      git config --add remote.upstream-readonly.pushurl "$DISABLED_PUSH_URL"
      echo "Disabled pushes on remote upstream-readonly."
      HARDEN_ACTIONS="${HARDEN_ACTIONS}  set the push address of remote upstream-readonly to $DISABLED_PUSH_URL
"
    fi
  fi

  # Check again, as git would resolve the push addresses now.
  local bad=""
  for remote in $(git remote); do
    while IFS= read -r url; do
      if [ -n "$url" ] && is_upstream_url "$url"; then bad="${bad}  $remote -> $url
"; fi
    done < <(git remote get-url --push --all "$remote" 2>/dev/null || true)
  done
  if [ -n "$bad" ]; then
    echo "Refusing: these remotes can still push to FEX-Emu/FEX:" >&2
    printf '%s' "$bad" >&2
    echo "Check url.*.insteadOf and url.*.pushInsteadOf settings (git config --get-regexp '^url\\.')." >&2
    exit 3
  fi
}

harden_remotes
if [ "$HARDEN_ONLY" -eq 1 ]; then
  if [ -n "$HARDEN_ACTIONS" ]; then printf 'Changed:\n%s' "$HARDEN_ACTIONS"; else echo "Changed: nothing."; fi
  echo "No remote can push to FEX-Emu/FEX."
  exit 0
fi

# --- Checks that need no network ---------------------------------------------------------

if [ "$DRY_RUN" -eq 0 ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "The working tree has uncommitted changes. Commit or stash them first." >&2
  exit 1
fi

if ! git rev-parse --verify --quiet "$PORT_BRANCH^{commit}" >/dev/null; then
  echo "Port branch $PORT_BRANCH does not exist." >&2
  exit 1
fi

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
  if [ -n "$SYNC_BRANCH" ] && git rev-parse --verify --quiet "refs/heads/$SYNC_BRANCH" >/dev/null; then
    echo "Branch $SYNC_BRANCH already exists. Pass --branch with another name, or delete it." >&2
    exit 1
  fi
  TARGET_TIP="$PORT_BRANCH"
fi

# --- Step 2: fetch -----------------------------------------------------------------------

# insteadOf could silently send the fetch somewhere else.
if [ "$(git ls-remote --get-url "$UPSTREAM_URL")" != "$UPSTREAM_URL" ]; then
  echo "Refusing: git rewrites $UPSTREAM_URL to $(git ls-remote --get-url "$UPSTREAM_URL")." >&2
  exit 3
fi

if [ "$DRY_RUN" -eq 1 ]; then
  UPSTREAM_TRACKING="refs/macos-sync-dry-run/upstream"
  trap 'git update-ref -d "$UPSTREAM_TRACKING" 2>/dev/null || true' EXIT
else
  UPSTREAM_TRACKING="refs/remotes/upstream-readonly/$UPSTREAM_REF"
  if ! git remote get-url upstream-readonly >/dev/null 2>&1; then
    git remote add --no-tags upstream-readonly "$UPSTREAM_URL"
    git config --unset-all remote.upstream-readonly.pushurl || true
    git config --add remote.upstream-readonly.pushurl "$DISABLED_PUSH_URL"
    echo "Added fetch-only remote upstream-readonly."
  fi
fi
# Fetch from the fixed address, not through a remote, so what is merged is upstream.
git fetch --no-tags "$UPSTREAM_URL" "+$UPSTREAM_REF:$UPSTREAM_TRACKING"
UPSTREAM_FULL="$(git rev-parse "$UPSTREAM_TRACKING^{commit}")"
UPSTREAM_SHA="$(git rev-parse --short=9 "$UPSTREAM_FULL")"

if [ "$MODE" = "new-branch" ]; then
  TARGET="${SYNC_BRANCH:-sync/upstream-$UPSTREAM_SHA}"
fi

# --- Step 3: report ----------------------------------------------------------------------

BASE_SHA="$(git merge-base "$TARGET_TIP" "$UPSTREAM_FULL")"
NEW_ALL="$(git rev-list --count "$BASE_SHA..$UPSTREAM_FULL")"
NEW_NONMERGE="$(git rev-list --count --no-merges "$BASE_SHA..$UPSTREAM_FULL")"
GIT="git -c core.quotePath=false"

echo "Port branch:      $PORT_BRANCH ($(git rev-parse --short=9 "$PORT_BRANCH"))"
echo "Merge into:       $TARGET ($MODE)"
echo "Upstream $UPSTREAM_REF:    $UPSTREAM_SHA"
echo "Merge base:       $(git rev-parse --short=9 "$BASE_SHA")"
echo "New upstream commits since the merge base: $NEW_ALL ($NEW_NONMERGE without merge commits)"
dry_run_summary() {
  echo "Dry run: no branch, remote or lasting ref was created and nothing was merged."
  if [ -n "$HARDEN_ACTIONS" ]; then
    printf 'Changed for safety:\n%s' "$HARDEN_ACTIONS"
  else
    echo "Remote settings: unchanged."
  fi
  echo "The fetched upstream objects remain in the object store; the temporary ref is removed."
}
if [ "$NEW_ALL" -eq 0 ]; then
  echo "Nothing to do."
  if [ "$DRY_RUN" -eq 1 ]; then dry_run_summary; fi
  exit 0
fi

echo
echo "Upstream commits without merge commits ($NEW_NONMERGE):"
git log --no-merges --format='  %h %s' "$BASE_SHA..$UPSTREAM_FULL"

echo
echo "Files that the port and upstream both changed (most likely to conflict):"
comm -12 \
  <($GIT diff --name-only "$BASE_SHA" "$TARGET_TIP" | sort) \
  <($GIT diff --name-only "$BASE_SHA" "$UPSTREAM_FULL" | sort) | sed 's/^/  /'

echo
echo "Submodule pointers upstream moved:"
SUBMODULES="$($GIT diff --raw --no-renames "$BASE_SHA" "$UPSTREAM_FULL" \
  | awk -F'\t' '{ split($1, m, " "); if (m[1] == ":160000" || m[2] == "160000") print "  " $2 }')"
echo "${SUBMODULES:-  (none)}"

echo
echo "Instruction tests upstream added, changed or renamed (run them):"
TESTS="$($GIT diff --name-only --diff-filter=AMR "$BASE_SHA" "$UPSTREAM_FULL" -- unittests/ASM | sed 's/^/  /')"
echo "${TESTS:-  (none)}"

echo
echo "Upstream source changes to check for the macOS traps (code-memory writes need"
echo "ScopedJITWrite, mprotect/mremap/Linux-only calls, x18, 4K pages, Linux headers):"
$GIT diff --name-only "$BASE_SHA" "$UPSTREAM_FULL" -- FEXCore Source CodeEmitter FEXHeaderUtils CMakeLists.txt \
  | sed 's/^/  /'

# Untracked files where upstream adds a file would stop git merge before it starts.
COLLISIONS="$(comm -12 \
  <($GIT ls-files --others --exclude-standard | sort) \
  <($GIT diff --no-renames --name-only --diff-filter=A "$TARGET_TIP" "$UPSTREAM_FULL" | sort))"

if [ "$DRY_RUN" -eq 1 ]; then
  echo
  if [ -n "$COLLISIONS" ]; then
    echo "A real run would refuse: untracked files are where upstream adds files:"
    printf '%s\n' "$COLLISIONS" | sed 's/^/  /'
  fi
  dry_run_summary
  exit 0
fi

if [ -n "$COLLISIONS" ]; then
  echo >&2
  echo "Refusing: these untracked files are where upstream adds files. Move them away first:" >&2
  printf '%s\n' "$COLLISIONS" | sed 's/^/  /' >&2
  exit 1
fi

# --- Step 4: merge -----------------------------------------------------------------------

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

Upstream FEX-Emu/FEX $UPSTREAM_REF $UPSTREAM_FULL: $NEW_ALL new commits
($NEW_NONMERGE without merge commits) since $(git rev-parse --short=9 "$BASE_SHA").
Made by Scripts/macos_sync_upstream.sh."

if git merge --no-ff -m "$MSG" "$UPSTREAM_FULL"; then
  echo
  echo "Merged cleanly into $TARGET."
elif git rev-parse --quiet --verify MERGE_HEAD >/dev/null; then
  echo
  CONFLICTS="$($GIT diff --name-only --diff-filter=U)"
  if [ -n "$CONFLICTS" ]; then
    echo "Merge stopped on conflicts in:"
    printf '%s\n' "$CONFLICTS" | sed 's/^/  /'
  else
    echo "The merge stopped without conflicted files; git's message above says why."
  fi
  echo "Resolve them on $TARGET keeping the macOS changes (see docs/macOS-upstream-sync.md),"
  echo "then 'git commit' to finish the merge. Do not squash or rebase it."
  exit 2
else
  echo >&2
  echo "The merge did not start; git's message above says why. Nothing was merged." >&2
  if [ "$MODE" = "new-branch" ]; then
    echo "$TARGET was created from $PORT_BRANCH and is checked out here; after fixing the" >&2
    echo "cause, switch back and delete it (git branch -D $TARGET), then run again." >&2
  fi
  exit 1
fi

echo
echo "Next, as described in docs/macOS-upstream-sync.md:"
echo "  git submodule update --init --depth 1 <each moved submodule the build uses>"
echo "  build, then run the smoke, thread and instruction tests as built and hardened"
echo "Nothing has been pushed. Never push to or open a pull request against FEX-Emu/FEX."
