#!/usr/bin/env bash
# End-to-end test: when an --rsh wrapper fails before tmux handshakes,
# kmux must surface the wrapper's output so the user can see *why* it
# failed — both its stdout and its stderr (where ssh & friends print
# "Could not resolve hostname", "Permission denied", etc.).
#
# Drives the real binary with an --rsh wrapper that writes a distinct
# marker to each stream and exits non-zero before exec'ing tmux, then
# asserts both markers appear in kmux's own output. Complements
# test-rsh-startup-failure.sh, which only checks that kmux exits.
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = both the stdout and stderr of the failing wrapper were
#          reported by kmux.
# Exit 1 = test assertion failed (a marker was missing, or kmux hung).
# Exit 2 = scaffolding failure (missing binary, no DISPLAY, etc.).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

# kmux reports the failure via qCritical (stderr); keep debug logging off
# the critical path but ensure stderr reaches our captured log.
export QT_ASSUME_STDERR_HAS_CONSOLE=1

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

WRAPPER="$HOMEDIR/rsh-noisy-fail.sh"
OUT_MARKER="RSH-STDOUT-MARKER-7f3a91"
ERR_MARKER="RSH-STDERR-MARKER-9b2c4d"

# Write a marker to each stream, then exit non-zero WITHOUT exec'ing the
# tmux command appended to argv — the generic "rsh dies before handshake"
# shape, but noisy on both channels.
cat >"$WRAPPER" <<WRAPPER_EOF
#!/bin/bash
echo "$OUT_MARKER"
echo "$ERR_MARKER" >&2
exit 7
WRAPPER_EOF
chmod +x "$WRAPPER"

echo "=== launching kmux with --rsh=$WRAPPER (prints to both streams, exits 7) ==="
"$KMUX" --rsh "$WRAPPER" >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

# Wait for kmux to exit (the startup-failure path should make it quit).
exited=0
for _ in $(seq 1 50); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        wait "$KMUX_PID" || true
        exited=1
        break
    fi
    sleep 0.2
done

if [[ "$exited" -ne 1 ]]; then
    echo "FAIL: kmux did not exit within 10s after the rsh wrapper failed — it hung" >&2
    kill "$KMUX_PID" 2>/dev/null || true
    exit 1
fi

# Both markers must show up in kmux's output. grep -F: fixed strings.
missing=0
if ! grep -qF "$OUT_MARKER" "$LOGDIR/kmux.log"; then
    echo "FAIL: wrapper's stdout ('$OUT_MARKER') was not reported by kmux" >&2
    missing=1
fi
if ! grep -qF "$ERR_MARKER" "$LOGDIR/kmux.log"; then
    echo "FAIL: wrapper's stderr ('$ERR_MARKER') was not reported by kmux" >&2
    missing=1
fi

if [[ "$missing" -ne 0 ]]; then
    echo "--- kmux.log ---" >&2
    cat "$LOGDIR/kmux.log" >&2 || true
    exit 1
fi

echo "PASS: kmux reported both the stdout and stderr of the failing --rsh wrapper"
exit 0
