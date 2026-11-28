#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Offline tests for Scripts/macos_sync_upstream.sh. Builds throwaway repositories in a
# temporary directory whose path contains spaces, and checks:
#   - the never-push hardening, for remotes in every address form;
#   - each refusal, by its exit status and message;
#   - the dry run changes no branch, ref or remote setting;
#   - clean merge, conflict, and a merge that does not start are told apart.
# The merge paths use the script's test hook MACOS_SYNC_TEST_UPSTREAM_URL to fetch
# "upstream" from a local repository; nothing touches the network.
#
#   Scripts/macos_sync_upstream_test.sh
set -euo pipefail

SCRIPT="$(cd "$(dirname "$0")" && pwd)/macos_sync_upstream.sh"
TOP="$(mktemp -d)"
trap 'rm -rf "$TOP"' EXIT
T="$TOP/sync test with spaces"
mkdir -p "$T"

# Isolate from the user's git settings (insteadOf rules, hooks, signing).
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid
unset MACOS_SYNC_TEST_UPSTREAM_URL || true

FAILS=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }

# expect <name> <exit status> <text the output must contain> -- command...
expect() {
  local name="$1" want="$2" text="$3" got out
  shift 4
  set +e
  out="$("$@" 2>&1)"
  got=$?
  set -e
  if [ "$got" -eq "$want" ] && printf '%s' "$out" | grep -qF -- "$text"; then
    pass "$name"
  else
    fail "$name (exit $got, wanted $want with \"$text\")"
    printf '%s\n' "$out" | sed 's/^/     | /'
  fi
}

# Independent of the script's own matcher.
UPSTREAM_RE='github\.com(:[0-9]+)?[:/]fex-emu/fex(\.git)?/*$'
pushable_upstream() {
  local r
  for r in $(git remote); do
    git remote get-url --push --all "$r" | grep -iE "$UPSTREAM_RE" | sed "s|^|$r: |" || true
  done
}

commit() { # file content message
  printf '%s\n' "$2" >"$1"
  git add -- "$1"
  git commit -q -m "$3"
}

# --- Repositories -----------------------------------------------------------------------

UP="$T/upstream repo"
git init -q -b main "$UP"
cd "$UP"
commit shared.txt "base" "base"
commit conflict.txt "base line" "conflict base"

PORT="$T/port repo"
git clone -q "$UP" "$PORT"
cd "$PORT"
git switch -q -c macos/v1
commit port.txt "port" "port change"

# Upstream moves on: a merge commit, a new file, and a new file in a new directory.
cd "$UP"
git switch -q -c topic
commit topic.txt "topic" "topic"
git switch -q main
git merge -q --no-ff -m "merge topic" topic
commit "added by upstream.txt" "new" "add file"
mkdir -p newdir
commit newdir/file.txt "nested" "add nested file"
UP_SHA="$(git rev-parse --short=9 HEAD)"
export UPSTREAM_TEST_URL="$UP"

# --- 1. Never-push hardening ------------------------------------------------------------

cd "$PORT"
git remote add r_https https://github.com/FEX-Emu/FEX.git
git remote add r_lower https://github.com/fex-emu/fex.git
git remote add r_ssh_slash ssh://git@github.com/FEX-Emu/FEX.git/
git remote add r_ssh_port ssh://git@github.com:22/FEX-Emu/FEX
git remote add r_scp git@github.com:FEX-Emu/FEX
git remote add r_fork_pushes_up https://github.com/someone/FEX.git
git config remote.r_fork_pushes_up.pushurl https://github.com/FEX-Emu/FEX.git
git remote add r_two_push https://github.com/someone/FEX.git
git config --add remote.r_two_push.pushurl https://github.com/someone/FEX.git
git config --add remote.r_two_push.pushurl git@github.com:fex-emu/FEX.git
git remote add r_rewrite https://example.invalid/mirror/FEX.git
git config url."https://github.com/FEX-Emu/".pushInsteadOf https://example.invalid/mirror/
git remote add r_fork https://github.com/someone/FEX.git

if [ -n "$(pushable_upstream)" ]; then pass "setup: remotes start pushable to upstream"; else fail "setup"; fi
expect "harden-only reports what it changed" 0 "set the push address of remote r_two_push" \
  -- /bin/bash "$SCRIPT" --harden-only
LEFT="$(pushable_upstream)"
if [ -z "$LEFT" ]; then pass "no remote can push upstream after hardening"; else fail "still pushable: $LEFT"; fi
if [ "$(git remote get-url --push r_fork)" = "https://github.com/someone/FEX.git" ]; then
  pass "an unrelated fork remote is left pushable"
else
  fail "the fork remote was changed"
fi
if [ "$(git config --get-all remote.r_two_push.pushurl)" = "DISABLED-never-push-upstream" ]; then
  pass "a remote with two push addresses ends with exactly the disabled one"
else
  fail "r_two_push push addresses: $(git config --get-all remote.r_two_push.pushurl | tr '\n' ' ')"
fi
expect "a second hardening pass changes nothing" 0 "Changed: nothing." -- /bin/bash "$SCRIPT" --harden-only

# A rewrite rule that turns the disabled address back into upstream is caught by the re-check.
git config url."https://github.com/FEX-Emu/FEX.git".insteadOf DISABLED-never-push-upstream
expect "refuses when a rewrite makes a remote pushable again" 3 "can still push to FEX-Emu/FEX" \
  -- /bin/bash "$SCRIPT" --harden-only
git config --unset url."https://github.com/FEX-Emu/FEX.git".insteadOf
git config --remove-section url."https://github.com/FEX-Emu/"
for r in $(git remote); do [ "$r" = origin ] || git remote remove "$r"; done

git remote add upstream-readonly https://github.com/someone-else/FEX.git
expect "refuses when upstream-readonly points elsewhere" 3 "does not point only at" \
  -- /bin/bash "$SCRIPT" --harden-only
git remote remove upstream-readonly

# --- 2. Refusals that must happen before any fetch --------------------------------------

# A test upstream that does not exist: reaching the fetch would fail differently.
export MACOS_SYNC_TEST_UPSTREAM_URL="$T/no such repo"
expect "--here with --branch" 1 "--here and --branch cannot be used together." -- /bin/bash "$SCRIPT" --here --branch x
expect "unknown option" 1 "Unknown option: --bogus" -- /bin/bash "$SCRIPT" --bogus
expect "--branch without a name" 1 "--branch needs a name." -- /bin/bash "$SCRIPT" --branch
expect "missing port branch" 1 "Port branch no/such does not exist." -- /bin/bash "$SCRIPT" no/such
git switch -q --detach
expect "--here on a detached HEAD, before fetching" 1 "detached HEAD" -- /bin/bash "$SCRIPT" --here
git switch -q macos/v1
git switch -q -c unrelated main
expect "--here on a branch without the port branch" 1 "does not contain macos/v1" -- /bin/bash "$SCRIPT" --here
git switch -q macos/v1
echo dirty >>port.txt
expect "uncommitted changes" 1 "uncommitted changes" -- /bin/bash "$SCRIPT" --here
git checkout -q -- port.txt

# This repository's own main carries the port and is not upstream: make the test port's
# main diverge from upstream (the port plus one more commit). The script must still take
# upstream only from the upstream address, whatever the local main holds.
git branch -f main macos/v1
git switch -q main
commit main-only.txt "main" "a commit only on the port's main"
git switch -q macos/v1

# --- 3. Dry run, merge, conflict (local test upstream) ----------------------------------

export MACOS_SYNC_TEST_UPSTREAM_URL="$UPSTREAM_TEST_URL"
WT="$T/work tree"
git worktree add -q -b pkg "$WT" macos/v1
cd "$WT"

BEFORE="$(git for-each-ref; git config --list --local; git rev-parse HEAD)"
expect "dry run reports and says what it changed" 0 "Dry run: no branch, remote or lasting ref" \
  -- /bin/bash "$SCRIPT" --dry-run --here
AFTER="$(git for-each-ref; git config --list --local; git rev-parse HEAD)"
if [ "$BEFORE" = "$AFTER" ]; then pass "dry run left refs, remote settings and HEAD unchanged"; else
  fail "dry run changed something"; diff <(echo "$BEFORE") <(echo "$AFTER") | sed 's/^/     | /' || true
fi
expect "commit counts name merge and non-merge commits" 0 "4 (3 without merge commits)" \
  -- /bin/bash "$SCRIPT" --dry-run --here
expect "a local main that carries the port is not taken for upstream" 0 "Upstream main:    $UP_SHA" \
  -- /bin/bash "$SCRIPT" --dry-run main

echo mine >"added by upstream.txt"
expect "refuses an untracked file where upstream adds one" 1 "untracked files are where upstream adds files" \
  -- /bin/bash "$SCRIPT" --here
rm "added by upstream.txt"
if [ -z "$(git rev-parse --quiet --verify MERGE_HEAD || true)" ]; then pass "no merge left in progress"; else fail "MERGE_HEAD left"; fi

echo mine >newdir
expect "a merge that does not start is not called a conflict" 1 "The merge did not start" \
  -- /bin/bash "$SCRIPT" --here
rm newdir
if [ "$(git rev-parse HEAD)" = "$(git rev-parse macos/v1)" ]; then pass "branch unchanged after a merge that did not start"; else fail "branch moved"; fi

expect "clean merge in place" 0 "Merged cleanly into pkg." -- /bin/bash "$SCRIPT" --here
if [ "$(git log -1 --format=%P | wc -w | tr -d ' ')" = 2 ]; then pass "the result is a two-parent merge commit"; else fail "not a merge commit"; fi
if [ "$(git config --get-all remote.upstream-readonly.pushurl)" = "DISABLED-never-push-upstream" ]; then
  pass "upstream-readonly was added fetch-only"
else
  fail "upstream-readonly push address"
fi
expect "nothing to do after the merge" 0 "Nothing to do." -- /bin/bash "$SCRIPT" --here
git remote add r_late https://github.com/FEX-Emu/FEX
expect "dry run with nothing to do still reports its safety change" 0 "set the push address of remote r_late" \
  -- /bin/bash "$SCRIPT" --dry-run --here
git remote remove r_late

cd "$PORT"
expect "default mode creates sync/upstream-<hash>" 0 "Merged cleanly into sync/upstream-$UP_SHA." -- /bin/bash "$SCRIPT"
expect "existing sync branch refused" 1 "already exists" -- /bin/bash "$SCRIPT"

git switch -q macos/v1
git switch -q -c conflicting
commit conflict.txt "port line" "port edits the same line"
cd "$UP"
commit conflict.txt "upstream line" "upstream edits the same line"
cd "$PORT"
expect "conflict reported with the file" 2 "  conflict.txt" -- /bin/bash "$SCRIPT" --here
if git rev-parse --quiet --verify MERGE_HEAD >/dev/null; then pass "conflicted merge left in progress to resolve"; else fail "no MERGE_HEAD"; fi
git merge --abort

echo
if [ "$FAILS" -eq 0 ]; then echo "All sync script tests passed."; else echo "$FAILS sync script test(s) failed."; exit 1; fi
