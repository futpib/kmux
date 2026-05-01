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
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="org.kde.konsole.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
# Menu navigation via xdotool needs a focused window, which bare Xvfb
# doesn't provide on its own — have lib.sh spawn twm when USE_XVFB=1.
export KMUX_TEST_NEED_WM="${KMUX_TEST_NEED_WM:-1}"

kmux_test_setup

LAUNCH_PID=""
LAUNCH_WIN=""
# Spawns kmux in the background of the CURRENT shell (not a subshell) and
# waits for its window to appear. Returns via globals so the child is not
# reaped when a subshell exits.
#
# `--onlyvisible` is required: QApplication creates a "Qt Selection Owner
# for kmux" helper for clipboard plumbing as soon as it starts, and a
# plain --name match would return that immediately (before any MainWindow
# has been mapped). The keystrokes that follow would then race a window
# that isn't ready to receive them.
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
        LAUNCH_WIN=$(xdotool search --onlyvisible --name kmux 2>/dev/null | tail -1 || true)
        if [[ -n "$LAUNCH_WIN" ]]; then
            return 0
        fi
        sleep 0.2
    done
    echo "error: kmux window did not appear" >&2
    kill "$LAUNCH_PID" 2>/dev/null || true
    return 1
}

# Quit kmux and wait for it to exit. We don't rely on Ctrl+Q here: a kmux
# with running tmux pane sessions pops a "Confirm Close" dialog on quit,
# and under Xvfb+twm xdotool keystrokes don't reliably reach that dialog
# to dismiss it. The previous fan-out — `xdotool key --window` to every
# match of "kmux" — also targeted the invisible "Qt Selection Owner for
# kmux" auxiliary X window, where XSendEvent could block.
#
# SIGTERM short-circuits all of that: kmux installs no SIGTERM handler,
# so the kernel default terminates the process. The toolbar Hidden flag
# is already on disk by the time we get here (KConfigGroup::sync runs
# inside the eventFilter for QEvent::Hide), so nothing relevant is lost.
# SIGKILL is the last-ditch backstop in case the process is wedged in a
# kernel uninterruptible state.
kill_and_wait() {
    local pid="$1"
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    echo "warn: kmux $pid did not exit on SIGTERM after 5s, sending SIGKILL" >&2
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

echo "=== run 1: hide sessionToolbar ==="
launch_and_wait_for_window "$LOGDIR/run1.log"
PID1=$LAUNCH_PID
WIN1=$LAUNCH_WIN
echo "  run1 window mapped: pid=$PID1 win=$WIN1"
sleep 1
# Be explicit about focus: twm's focus-follows-mouse model means the
# pointer (parked at 0,0 by Xvfb) decides who gets keystrokes, not
# whichever window mapped last. Activate the kmux window directly so
# the menu mnemonics that follow land on it.
xdotool windowactivate --sync "$WIN1" 2>/dev/null || true

# Settings menu → Toolbars Shown → Session Toolbar (the only entry).
# Alt+S opens Settings; 'b' is the mnemonic for "Tool&bars Shown";
# Return picks the highlighted "Session Toolbar" entry (the only item
# in the submenu).
DELAY_MS=150
echo "  sending menu keystrokes..."
xdotool key --delay "$DELAY_MS" alt+s
sleep 0.5
xdotool key --delay "$DELAY_MS" b
sleep 0.5
xdotool key --delay "$DELAY_MS" Return
sleep 1
echo "  killing run1..."
kill_and_wait "$PID1"
echo "  run1 finished"

echo "=== run 2: check visibility ==="
launch_and_wait_for_window "$LOGDIR/run2.log"
PID2=$LAUNCH_PID
WIN2=$LAUNCH_WIN
echo "  run2 window mapped: pid=$PID2 win=$WIN2"
sleep 2
echo "  killing run2..."
kill_and_wait "$PID2"
echo "  run2 finished"

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
