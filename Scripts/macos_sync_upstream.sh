#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Brings a new upstream FEX release into the macOS port, on a new branch, without ever
# sending anything upstream.
#
#   Scripts/macos_sync_upstream.sh [port-branch] [upstream-ref]
#
# Defaults: port branch macos/runtime-fixes, upstream ref main.
#
# What it does:
#   1. Fetches FEX-Emu/FEX (read only).
#   2. Creates sync/upstream-<short hash> from the port branch.
#   3. Merges the upstream ref into it.
#   4. On conflict, stops and lists the conflicted files.
#   5. On success, prints the commands to push the branch to the fork so the Mac job
#      builds it and runs every test.
#
# It never pushes by itself and never touches the upstream repository.
set -euo pipefail

PORT_BRANCH="${1:-macos/runtime-fixes}"
UPSTREAM_REF="${2:-main}"
UPSTREAM_URL="https://github.com/FEX-Emu/FEX.git"

if [ -n "$(git status --porcelain)" ]; then
  echo "The working tree has uncommitted changes. Commit or stash them first." >&2
  exit 1
fi

# A fetch-only remote: the push address is deliberately invalid.
if ! git remote get-url upstream-readonly >/dev/null 2>&1; then
  git remote add upstream-readonly "$UPSTREAM_URL"
fi
git remote set-url --push upstream-readonly "DISABLED-never-push-upstream"

git fetch --no-tags upstream-readonly "$UPSTREAM_REF"
UPSTREAM_SHA="$(git rev-parse --short=9 FETCH_HEAD)"
BASE_SHA="$(git merge-base "$PORT_BRANCH" FETCH_HEAD)"
NEW_COMMITS="$(git rev-list --count "$BASE_SHA"..FETCH_HEAD)"

echo "Port branch:      $PORT_BRANCH ($(git rev-parse --short=9 "$PORT_BRANCH"))"
echo "Upstream $UPSTREAM_REF:    $UPSTREAM_SHA"
echo "New upstream commits since the port's base: $NEW_COMMITS"
if [ "$NEW_COMMITS" -eq 0 ]; then
  echo "Nothing to do."
  exit 0
fi

echo
echo "Port files that upstream also changed (most likely to conflict):"
comm -12 \
  <(git diff --name-only "$BASE_SHA" "$PORT_BRANCH" | sort) \
  <(git diff --name-only "$BASE_SHA" FETCH_HEAD | sort) | sed 's/^/  /'

SYNC_BRANCH="sync/upstream-$UPSTREAM_SHA"
git checkout -b "$SYNC_BRANCH" "$PORT_BRANCH"

if git merge --no-ff --no-edit FETCH_HEAD; then
  echo
  echo "Merged cleanly into $SYNC_BRANCH."
  echo "Next: push it to the fork so the Mac job builds it and runs the tests:"
  echo "  git push origin $SYNC_BRANCH:macos/$SYNC_BRANCH"
  echo "Do not open a pull request against FEX-Emu/FEX."
else
  echo
  echo "Merge stopped on conflicts in:"
  git diff --name-only --diff-filter=U | sed 's/^/  /'
  echo "Resolve them on $SYNC_BRANCH, keeping the macOS changes, then continue the merge."
  exit 2
fi
