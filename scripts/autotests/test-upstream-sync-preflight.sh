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
mkdir -p "$UPSTREAM/desktop" "$UPSTREAM/po/en" "$UPSTREAM/src"
echo '[Desktop Entry]' > "$UPSTREAM/desktop/kmuxpart.desktop"
echo '[Desktop Entry]' > "$UPSTREAM/desktop/org.kde.kmux.desktop"
echo '<releases>' > "$UPSTREAM/desktop/org.kde.kmux.appdata.xml"
echo base > "$UPSTREAM/po/en/kmux.po"
echo base > "$UPSTREAM/src/main.cpp"
git -C "$UPSTREAM" add .
git -C "$UPSTREAM" commit --quiet -m base
BASE=$(git -C "$UPSTREAM" rev-parse HEAD)

git clone --quiet "$UPSTREAM" "$TARGET"

assert_result() {
    local expected=$1
    local upstream_repository=$2
    local upstream_revision=${3:-master}
    local actual

    actual=$(
        "$CHECK_SCRIPT" \
            "$TARGET" HEAD \
            "$upstream_repository" "$upstream_revision"
    )
    if [[ $actual != "$expected" ]]; then
        echo "FAIL: expected sync_required=$expected, got $actual" >&2
        exit 1
    fi
}

assert_upstream_tip_absent_from_target() {
    local upstream_revision=${1:-master}
    local upstream_tip

    upstream_tip=$(git -C "$UPSTREAM" rev-parse "$upstream_revision")
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

echo 'Comment[nn]=Bruk kommandolinja' >> "$UPSTREAM/desktop/kmuxpart.desktop"
echo 'Name[he]=פתיחת חלון חדש' >> "$UPSTREAM/desktop/org.kde.kmux.desktop"
git -C "$UPSTREAM" add desktop
git -C "$UPSTREAM" commit --quiet -m desktop-localization
assert_result false "$UPSTREAM"
assert_upstream_tip_absent_from_target

echo '  <release version="26.08.1" date="2026-09-10"/>' >> "$UPSTREAM/desktop/org.kde.kmux.appdata.xml"
git -C "$UPSTREAM" add desktop/org.kde.kmux.appdata.xml
git -C "$UPSTREAM" commit --quiet -m appstream-release
assert_result false "$UPSTREAM"
assert_upstream_tip_absent_from_target

echo source >> "$UPSTREAM/src/main.cpp"
git -C "$UPSTREAM" add src/main.cpp
git -C "$UPSTREAM" commit --quiet -m source
assert_result true "$UPSTREAM"
assert_upstream_tip_absent_from_target

git -C "$UPSTREAM" switch --quiet --create functional-desktop "$BASE"
echo 'Exec=kmux --new-tab' >> "$UPSTREAM/desktop/org.kde.kmux.desktop"
git -C "$UPSTREAM" add desktop/org.kde.kmux.desktop
git -C "$UPSTREAM" commit --quiet -m functional-desktop
assert_result true "$UPSTREAM" functional-desktop
assert_upstream_tip_absent_from_target functional-desktop

git -C "$UPSTREAM" switch --quiet --create substantive-appdata "$BASE"
echo '  <description>Changed product behavior</description>' >> "$UPSTREAM/desktop/org.kde.kmux.appdata.xml"
git -C "$UPSTREAM" add desktop/org.kde.kmux.appdata.xml
git -C "$UPSTREAM" commit --quiet -m substantive-appdata
assert_result true "$UPSTREAM" substantive-appdata
assert_upstream_tip_absent_from_target substantive-appdata

git init --quiet --initial-branch=master "$UNRELATED"
git -C "$UNRELATED" config user.name test
git -C "$UNRELATED" config user.email test@example.com
echo unrelated > "$UNRELATED/unrelated.txt"
git -C "$UNRELATED" add unrelated.txt
git -C "$UNRELATED" commit --quiet -m unrelated
assert_result true "$UNRELATED"

echo "PASS: upstream classification is cumulative and object-isolated"
