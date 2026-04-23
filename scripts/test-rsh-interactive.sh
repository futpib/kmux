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
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
KMUX="$REPO_ROOT/build/bin/kmux"

if [[ ! -x "$KMUX" ]]; then
    echo "error: $KMUX missing — build with: cmake --build build --target kmux" >&2
    exit 2
fi

REQUIRED_TOOLS=(xdotool tmux)
if [[ "${USE_XVFB:-0}" == "1" ]]; then
    REQUIRED_TOOLS+=(Xvfb dbus-launch xdpyinfo)
fi
for tool in "${REQUIRED_TOOLS[@]}"; do
    command -v "$tool" >/dev/null || { echo "error: $tool not installed" >&2; exit 2; }
done
if [[ "${USE_XVFB:-0}" != "1" && -z "${DISPLAY:-}" ]]; then
    echo "error: no \$DISPLAY and USE_XVFB=0 — run on a live X session or export USE_XVFB=1" >&2
    exit 2
fi

HOMEDIR=$(mktemp -d)
LOGDIR="$HOMEDIR/logs"
mkdir -p "$LOGDIR"

KMUX_PID=""
XVFB_PID=""
cleanup() {
    if [[ -n "$KMUX_PID" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    pkill -P $$ 2>/dev/null || true
    if [[ -n "${DBUS_SESSION_BUS_PID:-}" ]]; then
        kill "$DBUS_SESSION_BUS_PID" 2>/dev/null || true
    fi
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
    if [[ "${KEEP_LOGS:-0}" == "1" ]]; then
        echo "logs kept in $LOGDIR"
    else
        rm -rf "$HOMEDIR"
    fi
}
trap cleanup EXIT

export HOME="$HOMEDIR"
export XDG_CONFIG_HOME="$HOMEDIR/.config"
export XDG_STATE_HOME="$HOMEDIR/.local/state"
export XDG_RUNTIME_DIR="$HOMEDIR/run"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

find_display() {
    for n in $(seq 90 120); do
        if [[ ! -e "/tmp/.X${n}-lock" && ! -e "/tmp/.X11-unix/X${n}" ]]; then
            echo ":$n"
            return 0
        fi
    done
    echo "error: no free X display" >&2
    return 1
}
if [[ "${USE_XVFB:-0}" == "1" ]]; then
    DISPLAY_NUM=$(find_display)
    Xvfb "$DISPLAY_NUM" -screen 0 1280x720x24 -nolisten tcp >"$LOGDIR/xvfb.log" 2>&1 &
    XVFB_PID=$!
    export DISPLAY="$DISPLAY_NUM"
    for _ in $(seq 1 50); do
        xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
        sleep 0.1
    done
    eval "$(dbus-launch --sh-syntax)"
    export DBUS_SESSION_BUS_ADDRESS
    export DBUS_SESSION_BUS_PID
fi

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
if xdotool search --name kmux >/dev/null 2>&1; then
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
    if [[ "$window_ok" -eq 0 ]] && xdotool search --name kmux >/dev/null 2>&1; then
        window_ok=1
    fi
    if [[ "$tmux_ok" -eq 1 && "$window_ok" -eq 1 ]]; then
        echo "PASS: tmux session established and kmux window shown after --rsh authenticated"
        exit 0
    fi
    sleep 0.5
done

echo "FAIL: after unblocking wrapper, tmux_ok=$tmux_ok window_ok=$window_ok (see $LOGDIR/kmux.log)" >&2
exit 1
