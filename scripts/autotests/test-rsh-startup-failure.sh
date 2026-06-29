#!/usr/bin/env bash
# End-to-end test for kmux's behaviour when an --rsh wrapper fails before
# tmux ever handshakes.
#
# kmux defers showing its window until tmux's first control-mode reply
# (so an interactive ssh password prompt doesn't lose terminal focus —
# see test-rsh-interactive.sh). The hazard is the failure path: if the
# rsh wrapper (or remote tmux) exits *before* that handshake — a bad ssh
# host, missing remote tmux, a wrapper that exits non-zero — then the
# "ready" signal never fires, no window is ever shown, and historically
# kmux would idle forever with no window and no error: a silent hang.
#
# This test drives the real binary with an --rsh wrapper that exits
# non-zero immediately and asserts that kmux:
#   1. exits on its own within a few seconds (does NOT hang), and
#   2. exits with a non-zero status (the failure is surfaced to the
#      launching shell, not swallowed).
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = kmux reported the startup failure and exited non-zero.
# Exit 1 = test assertion failed (kmux hung, or exited zero).
# Exit 2 = scaffolding failure (missing binary, no DISPLAY, etc.).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Capture kmux/Qt diagnostics in the kmux.log we keep, so a failure dump
# shows the "tmux session failed to start" line (or its absence).
export QT_LOGGING_RULES="org.kde.konsole.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

WRAPPER="$HOMEDIR/rsh-fail.sh"

# A wrapper that mimics ssh failing to connect: print an error to stderr
# and exit non-zero *without* exec'ing the tmux command appended to its
# argv. This is the generic "rsh dies before handshake" shape — `ssh
# bad@host`, a missing remote tmux, or a denied wrapper all reduce to it.
cat >"$WRAPPER" <<'WRAPPER_EOF'
#!/bin/bash
echo "rsh-fail: ssh: Could not resolve hostname test: Name or service not known" >&2
exit 255
WRAPPER_EOF
chmod +x "$WRAPPER"

echo "=== launching kmux with --rsh=$WRAPPER (exits 255 before handshake) ==="
"$KMUX" --rsh "$WRAPPER" >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

# kmux should notice the subprocess died before ready() and quit on its
# own. Poll for up to 10s; a binary with the bug never exits and we fall
# through to the failure branch.
exited=0
status=0
for _ in $(seq 1 50); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        # `|| status=$?` keeps a non-zero exit (the very thing we want to
        # assert) from tripping `set -e` before we can record it.
        wait "$KMUX_PID" || status=$?
        exited=1
        break
    fi
    sleep 0.2
done

if [[ "$exited" -ne 1 ]]; then
    echo "FAIL: kmux did not exit within 10s after the rsh wrapper failed — it hung" >&2
    echo "--- kmux.log ---" >&2
    cat "$LOGDIR/kmux.log" >&2 || true
    kill "$KMUX_PID" 2>/dev/null || true
    exit 1
fi

# Assertion 2: the exit must be non-zero. A zero exit would mean the
# failure was swallowed and a script chaining `kmux --rsh … && next`
# would wrongly proceed.
if [[ "$status" -eq 0 ]]; then
    echo "FAIL: kmux exited 0 after the rsh wrapper failed — failure was swallowed" >&2
    echo "--- kmux.log ---" >&2
    cat "$LOGDIR/kmux.log" >&2 || true
    exit 1
fi

echo "PASS: kmux exited non-zero (status=$status) after the --rsh wrapper failed before handshake"
exit 0
