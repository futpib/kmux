#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
RECONCILE_SCRIPT="$SCRIPT_DIR/../reconcile-upstream-sync-issues.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/kmux-upstream-issue-test.XXXXXX")

cleanup() {
    rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

mkdir -p "$TEST_ROOT/bin"
MOCK_GH_CALLS="$TEST_ROOT/gh-calls"
MOCK_GH_RESPONSE="$TEST_ROOT/issues.json"
export MOCK_GH_CALLS MOCK_GH_RESPONSE

cat > "$TEST_ROOT/bin/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s' "$1" >> "$MOCK_GH_CALLS"
for argument in "${@:2}"; do
    printf ' %s' "$argument" >> "$MOCK_GH_CALLS"
done
printf '\n' >> "$MOCK_GH_CALLS"

if [[ $1 == api ]]; then
    shift
    jq_filter=
    while [[ $# -gt 0 ]]; do
        if [[ $1 == --jq ]]; then
            jq_filter=$2
            break
        fi
        shift
    done
    jq -r "$jq_filter" "$MOCK_GH_RESPONSE"
fi
EOF
chmod +x "$TEST_ROOT/bin/gh"

assert_call() {
    local expected=$1
    if ! grep -Fq -- "$expected" "$MOCK_GH_CALLS"; then
        echo "FAIL: expected gh call containing: $expected" >&2
        cat "$MOCK_GH_CALLS" >&2
        exit 1
    fi
}

assert_no_call() {
    local unexpected=$1
    if grep -Fq -- "$unexpected" "$MOCK_GH_CALLS"; then
        echo "FAIL: unexpected gh call containing: $unexpected" >&2
        cat "$MOCK_GH_CALLS" >&2
        exit 1
    fi
}

run_reconcile() {
    : > "$MOCK_GH_CALLS"
    PATH="$TEST_ROOT/bin:$PATH" \
        GITHUB_REPOSITORY=futpib/kmux \
        "$RECONCILE_SCRIPT" "$@"
}

printf '[]\n' > "$MOCK_GH_RESPONSE"
run_reconcile failed https://github.example/runs/1
assert_call "issue create --repo futpib/kmux --title Upstream sync from KDE/konsole failed"
assert_call "--body The scheduled upstream sync is failing. See the latest failed [run](https://github.example/runs/1)."

cat > "$MOCK_GH_RESPONSE" <<'EOF'
[
  {
    "number": 114,
    "created_at": "2026-09-09T10:52:50Z",
    "title": "Upstream sync from KDE/konsole failed",
    "user": {"login": "github-actions[bot]"}
  },
  {
    "number": 112,
    "created_at": "2026-09-07T11:46:14Z",
    "title": "Upstream sync from KDE/konsole failed",
    "user": {"login": "github-actions[bot]"}
  },
  {
    "number": 113,
    "created_at": "2026-09-08T10:44:56Z",
    "title": "Upstream sync from KDE/konsole failed",
    "user": {"login": "github-actions[bot]"}
  },
  {
    "number": 115,
    "created_at": "2026-09-09T11:00:00Z",
    "title": "Upstream sync from KDE/konsole failed",
    "user": {"login": "someone-else"}
  },
  {
    "number": 116,
    "created_at": "2026-09-09T12:00:00Z",
    "title": "Upstream sync from KDE/konsole failed",
    "user": {"login": "github-actions[bot]"},
    "pull_request": {"url": "https://github.example/pulls/116"}
  }
]
EOF

run_reconcile failed https://github.example/runs/2
assert_call "issue edit 112 --repo futpib/kmux"
assert_call "issue close 113 --repo futpib/kmux --reason not planned"
assert_call "issue close 114 --repo futpib/kmux --reason not planned"
assert_no_call "issue close 112"
assert_no_call "issue close 115"
assert_no_call "issue close 116"
assert_no_call "issue create"

run_reconcile resolved https://github.example/runs/3
assert_call "issue close 112 --repo futpib/kmux --reason completed"
assert_call "issue close 113 --repo futpib/kmux --reason completed"
assert_call "issue close 114 --repo futpib/kmux --reason completed"
assert_no_call "issue close 115"
assert_no_call "issue close 116"
assert_no_call "issue edit"
assert_no_call "issue create"

echo "PASS: repeated upstream-sync failures reuse one issue and resolved failures close cleanly"
