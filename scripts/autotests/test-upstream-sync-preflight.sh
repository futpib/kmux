#!/usr/bin/env bash
# Regression: classifying upstream paths must not put the upstream tip in the
# target checkout's object database, where actions-template-sync would mistake
# object availability for merged ancestry and silently skip a required sync.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CHECK_SCRIPT="$SCRIPT_DIR/../check-upstream-sync-required.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/kmux-upstream-preflight-test.XXXXXX")

cleanup() {
    rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

export TMPDIR="$TEST_ROOT/preflight-tmp"
mkdir -p "$TMPDIR"

UPSTREAM="$TEST_ROOT/upstream"
TARGET="$TEST_ROOT/target"
UNRELATED="$TEST_ROOT/unrelated"

git init --quiet --initial-branch=master "$UPSTREAM"
git -C "$UPSTREAM" config user.name test
git -C "$UPSTREAM" config user.email test@example.com
mkdir -p "$UPSTREAM/po/en" "$UPSTREAM/src"
echo base > "$UPSTREAM/po/en/kmux.po"
echo base > "$UPSTREAM/src/main.cpp"
git -C "$UPSTREAM" add .
git -C "$UPSTREAM" commit --quiet -m base

git clone --quiet "$UPSTREAM" "$TARGET"

assert_result() {
    local expected=$1
    local upstream_repository=$2
    local actual

    actual=$(
        "$CHECK_SCRIPT" \
            "$TARGET" HEAD \
            "$upstream_repository" master
    )
    if [[ $actual != "$expected" ]]; then
        echo "FAIL: expected sync_required=$expected, got $actual" >&2
        exit 1
    fi
}

assert_upstream_tip_absent_from_target() {
    local upstream_tip

    upstream_tip=$(git -C "$UPSTREAM" rev-parse HEAD)
    if git -C "$TARGET" cat-file -e "$upstream_tip" 2>/dev/null; then
        echo "FAIL: preflight leaked upstream tip $upstream_tip into target objects" >&2
        exit 1
    fi
}

assert_result false "$UPSTREAM"

echo localized >> "$UPSTREAM/po/en/kmux.po"
git -C "$UPSTREAM" add po/en/kmux.po
git -C "$UPSTREAM" commit --quiet -m localization
assert_result false "$UPSTREAM"
assert_upstream_tip_absent_from_target

echo source >> "$UPSTREAM/src/main.cpp"
git -C "$UPSTREAM" add src/main.cpp
git -C "$UPSTREAM" commit --quiet -m source
assert_result true "$UPSTREAM"
assert_upstream_tip_absent_from_target

git init --quiet --initial-branch=master "$UNRELATED"
git -C "$UNRELATED" config user.name test
git -C "$UNRELATED" config user.email test@example.com
echo unrelated > "$UNRELATED/unrelated.txt"
git -C "$UNRELATED" add unrelated.txt
git -C "$UNRELATED" commit --quiet -m unrelated
assert_result true "$UNRELATED"

echo "PASS: upstream classification is cumulative and object-isolated"
