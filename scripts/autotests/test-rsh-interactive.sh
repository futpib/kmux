#!/usr/bin/env bash
# End-to-end test for kmux's --rsh option with an interactive wrapper.
#
# Drives the real kmux binary in an isolated HOME with an --rsh wrapper
# script that blocks on a named pipe until the test writes the "password"
# to it (mimicking ssh waiting on a password prompt). Once the wrapper
# reads a matching password it execs tmux; otherwise it exits non-zero
# and the bridge never sees a tmux session.
#
# The test asserts:
#   1. While the wrapper blocks on the FIFO, the tmux socket does NOT
#      exist — proves the wrapper actually ran (kmux didn't bypass it
#      and spawn tmux directly).
#   2. While the wrapper blocks, the kmux GUI window is NOT visible —
#      proves Application defers window->show() until tmux's first
#      reply so --rsh prompts (ssh password) don't lose terminal focus.
#   3. After writing the password, tmux -S $socket list-sessions
#      succeeds AND the kmux window appears — proves the wrapper
#      execed tmux, the bridge connected, and the show-deferral lifted.
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = rsh wrapper was invoked, blocked, authenticated, and kmux
#          successfully connected to the wrapped tmux.
# Exit 1 = test assertion failed (wrapper bypassed, window appeared
#          early, or never connected).
# Exit 2 = scaffolding failure (missing binary, no DISPLAY, etc.).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Capture kmux/Qt diagnostics in the kmux.log we already keep. Cheap, and
# turns the failure path's tail-of-log dump from empty into useful.
export QT_LOGGING_RULES="org.kde.konsole.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

FIFO="$HOMEDIR/rsh-fifo"
WRAPPER="$HOMEDIR/rsh-wrapper.sh"
SOCKET="$HOMEDIR/tmux.sock"
PASSWORD="hunter2"

mkfifo "$FIFO"

# The wrapper blocks on the FIFO until the test writes the password, then
# execs its args. Uses `read` (bash builtin) rather than /dev/tty so no
# pty is needed — the interactive scenario is simulated via a rendezvous
# on a named pipe, not a real terminal.
cat >"$WRAPPER" <<WRAPPER_EOF
#!/bin/bash
set -u
IFS= read -r pass <"$FIFO"
if [[ "\$pass" != "$PASSWORD" ]]; then
    echo 'rsh-wrapper: permission denied' >&2
    exit 1
fi
exec "\$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

echo "=== launching kmux with --rsh=$WRAPPER -S $SOCKET ==="
"$KMUX" --rsh "$WRAPPER" -S "$SOCKET" >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

# Give kmux time to start Qt, spawn the wrapper, and (if the show-
# deferral regressed) pop the window. 3 seconds is generous — the
# wrapper blocks forever on the FIFO so there's no race to win.
sleep 3

# The bridge shouldn't have failed: if `bridge->start` couldn't even
# exec the wrapper, kmux would have logged "Failed to start tmux" and
# exited.
if ! kill -0 "$KMUX_PID" 2>/dev/null; then
    echo "FAIL: kmux exited before wrapper unblocked (see $LOGDIR/kmux.log)" >&2
    exit 1
fi

# Assertion 1: tmux socket must NOT exist yet. If it did, the wrapper
# was bypassed and kmux spawned tmux directly — the whole point of
# --rsh is that every tmux invocation goes through the wrapper.
if [[ -e "$SOCKET" ]]; then
    echo "FAIL: tmux socket exists before wrapper was unblocked — wrapper was bypassed" >&2
    exit 1
fi
echo "OK: tmux socket absent while wrapper is blocked on FIFO"

# Assertion 2: kmux window must NOT be visible yet. If it is, the
# Application short-circuited the show-deferral and would steal focus
# from a still-prompting ssh in the real use case.
#
# Match by WM_CLASS, not window title: Qt creates an auxiliary "Qt
# Selection Owner for kmux" helper for clipboard plumbing as soon as
# QApplication starts, and a substring --name match would always hit
# it before any MainWindow exists. WM_CLASS=kmux is set only on the
# real top-level windows. Combine with --onlyvisible so we distinguish
# the deferred-hidden MainWindow (which exists but is unmapped) from
# the post-show one.
if xdotool search --onlyvisible --class kmux >/dev/null 2>&1; then
    echo "FAIL: kmux window appeared before tmux's first reply — show-deferral regressed" >&2
    exit 1
fi
echo "OK: kmux window hidden while wrapper is blocked on FIFO"

# Provide the "password" — wrapper reads it, validates, execs tmux.
echo "=== writing password to FIFO ==="
printf '%s\n' "$PASSWORD" >"$FIFO"

# Assertion 3: both the tmux session and the kmux window must appear.
# The session proves the bridge connected; the window proves the
# show-deferral released once the gateway emitted ready().
tmux_ok=0
window_ok=0
for _ in $(seq 1 60); do
    if [[ "$tmux_ok" -eq 0 ]] && tmux -S "$SOCKET" list-sessions >/dev/null 2>&1; then
        tmux_ok=1
    fi
    if [[ "$window_ok" -eq 0 ]] && xdotool search --onlyvisible --class kmux >/dev/null 2>&1; then
        window_ok=1
    fi
    if [[ "$tmux_ok" -eq 1 && "$window_ok" -eq 1 ]]; then
        echo "PASS: tmux session established and kmux window shown after --rsh authenticated"
        exit 0
    fi
    sleep 0.5
done

echo "FAIL: after unblocking wrapper, tmux_ok=$tmux_ok window_ok=$window_ok (see $LOGDIR/kmux.log)" >&2
echo "--- kmux still running? ---" >&2
if kill -0 "$KMUX_PID" 2>/dev/null; then
    echo "  yes, pid=$KMUX_PID" >&2
else
    echo "  no, exited" >&2
fi
echo "--- xdotool windows matching kmux ---" >&2
visible_set=" $(xdotool search --onlyvisible --class kmux 2>/dev/null | tr '\n' ' ')"
for w in $(xdotool search --name kmux 2>/dev/null) $(xdotool search --class kmux 2>/dev/null); do
    name=$(xdotool getwindowname "$w" 2>/dev/null || echo '?')
    geom=$(xdotool getwindowgeometry "$w" 2>/dev/null | tr '\n' ' ' || echo '?')
    [[ "$visible_set" == *" $w "* ]] && visible=1 || visible=0
    echo "  win $w visible=$visible name='$name' $geom" >&2
done
echo "--- last 80 lines of kmux.log ---" >&2
tail -80 "$LOGDIR/kmux.log" >&2 || true
exit 1
