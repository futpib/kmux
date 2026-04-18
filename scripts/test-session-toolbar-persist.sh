#!/usr/bin/env bash
# End-to-end test for sessionToolbar visibility persistence.
#
# Drives the real kmux binary in an isolated HOME, toggles "Session Toolbar"
# off via the Settings → Toolbars Shown menu, exits, launches kmux again,
# and asserts that sessionToolbar came back hidden.
#
# By default uses $DISPLAY (your live X session — you'll see two kmux windows
# flash by). Set USE_XVFB=1 to use a headless Xvfb instead; that needs a
# window manager on the display or the xdotool keystrokes may not land on
# menus, and Claude Code sandboxes have neither.
#
# Exit 0 = sessionToolbar stayed hidden (fix works).
# Exit 1 = sessionToolbar came back visible (bug still present).
# Exit 2 = test scaffolding failed (build missing, menu not reached, etc.).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
KMUX="$REPO_ROOT/build/bin/kmux"

if [[ ! -x "$KMUX" ]]; then
    echo "error: $KMUX missing — build with: cmake --build build --target kmux" >&2
    exit 2
fi
REQUIRED_TOOLS=(xdotool)
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

XVFB_PID=""
cleanup() {
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
export QT_LOGGING_RULES="org.kde.konsole.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# Pick a free display number, then start Xvfb on it.
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

LAUNCH_PID=""
LAUNCH_WIN=""
# Spawns kmux in the background of the CURRENT shell (not a subshell) and
# waits for its window to appear. Returns via globals so the child is not
# reaped when a subshell exits.
launch_and_wait_for_window() {
    local logfile="$1"
    "$KMUX" >"$logfile" 2>&1 &
    LAUNCH_PID=$!
    LAUNCH_WIN=""
    for _ in $(seq 1 100); do
        if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
            echo "error: kmux exited before window appeared (see $logfile)" >&2
            return 1
        fi
        LAUNCH_WIN=$(xdotool search --name kmux 2>/dev/null | tail -1 || true)
        if [[ -n "$LAUNCH_WIN" ]]; then
            return 0
        fi
        sleep 0.2
    done
    echo "error: kmux window did not appear" >&2
    kill "$LAUNCH_PID" 2>/dev/null || true
    return 1
}

kill_and_wait() {
    local pid="$1"
    xdotool search --name kmux 2>/dev/null | while read -r w; do
        xdotool key --window "$w" ctrl+q 2>/dev/null || true
    done
    for _ in $(seq 1 30); do
        if ! kill -0 "$pid" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

echo "=== run 1: hide sessionToolbar ==="
launch_and_wait_for_window "$LOGDIR/run1.log"
PID1=$LAUNCH_PID
WIN1=$LAUNCH_WIN
sleep 1

# Settings menu → Toolbars Shown → Session Toolbar (the only entry).
# Without a WM, focus is fragile — send events to the whole display and
# pace them so the menu has time to open between strokes. Alt+S opens
# Settings; 'b' is the mnemonic for "Tool&bars Shown"; Return picks the
# highlighted "Session Toolbar" entry (the only item in the submenu).
DELAY_MS=150
xdotool key --delay "$DELAY_MS" alt+s
sleep 0.5
xdotool key --delay "$DELAY_MS" b
sleep 0.5
xdotool key --delay "$DELAY_MS" Return
sleep 1

kill_and_wait "$PID1"

echo "=== run 2: check visibility ==="
launch_and_wait_for_window "$LOGDIR/run2.log"
PID2=$LAUNCH_PID
WIN2=$LAUNCH_WIN
sleep 2
kill_and_wait "$PID2"

# Sanity-check: confirm run 1 actually observed a sessionToolbar HIDE event.
if ! grep -q 'toolbar HIDE "sessionToolbar"' "$LOGDIR/run1.log"; then
    echo "FAIL: run 1 never hid sessionToolbar — xdotool menu navigation missed the toggle" >&2
    echo "  Check $LOGDIR/run1.log and verify Settings menu mnemonics." >&2
    exit 2
fi

# Did run 2 see sessionToolbar come back visible?
final_line=$(grep 'activeViewChanged: after  addClient' "$LOGDIR/run2.log" | tail -1 || true)
if [[ -z "$final_line" ]]; then
    echo "FAIL: could not find activeViewChanged line in run2 log" >&2
    exit 1
fi

echo "run2 activeViewChanged: $final_line"
if echo "$final_line" | grep -q 'sessionToolbar=hidden'; then
    echo "PASS: sessionToolbar stayed hidden across restart"
    exit 0
else
    echo "FAIL: sessionToolbar came back visible"
    exit 1
fi
