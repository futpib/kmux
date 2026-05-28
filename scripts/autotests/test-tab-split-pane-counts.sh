#!/usr/bin/env bash
# Bug repro: creating a 2nd tab in kmux, splitting it, switching back, then
# splitting the original tab does NOT produce 2 panes per tmux window. The
# second split lands on the wrong window (the still-active-in-tmux one),
# leaving the original window with 1 pane and the second window with 3.
#
# This test is expected to FAIL on current kmux; it exists to lock in the
# repro until the bug is fixed.
#
# Workflow (everything driven through kmux's UI; the test only inspects
# tmux state — never mutates it):
#   1. Pre-create a tmux session with 1 window/1 pane and attach kmux to it.
#   2. Create a 2nd tab in kmux (Ctrl+Shift+T) — kmux's tmux bridge runs
#      `tmux new-window`, so the session grows to 2 windows.
#   3. Split the (now-active) new tab (Ctrl+ParenLeft) — `tmux split-window`
#      adds a 2nd pane to that window. Now: window 0 = 1 pane, window 1 = 2.
#   4. Switch back to the original tab (Shift+Left, kmux's "Previous Tab"
#      shortcut). Expected: kmux's active session/tab is now window 0.
#   5. Split the original tab (Ctrl+ParenLeft). Expected: window 0 gains a
#      2nd pane, ending with 2 windows × 2 panes each.
#   6. Assert tmux reports exactly 2 windows, each with exactly 2 panes.
#
# Why it currently fails: ViewManager::splitView() targets
# TmuxController::activePaneId() (ViewManager.cpp:1054), which is updated
# only when ViewManager::controllerChanged sees a user-initiated focus
# reason (Mouse / Tab / Backtab / ShortcutFocusReason). A tab switch via
# QTabWidget::setCurrentIndex (the path Shift+Left, Ctrl+PageUp, and the
# prevSession D-Bus method all funnel into) lands the focus event with
# Qt::OtherFocusReason, so activePaneId stays pointing at the pane in the
# new tab. Step 5's split therefore goes to window 1 again.
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = bug fixed (each tab has 2 panes).
# Exit 1 = bug still present (assertion fails — expected outcome today).
# Exit 2 = scaffolding failure (missing binary, missing tool, etc.).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="org.kde.konsole.debug=true;konsole.tmux.bridge.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
# xdotool keystrokes need a focused window; bare Xvfb has no window manager,
# so have lib.sh spawn twm when USE_XVFB=1.
export KMUX_TEST_NEED_WM="${KMUX_TEST_NEED_WM:-1}"

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

SOCKET="$HOMEDIR/tmux.sock"
SESSION="splittest"

echo "=== pre-creating tmux session ==="
tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 200 -y 60

echo "=== launching kmux to attach ==="
"$KMUX" -S "$SOCKET" -s "$SESSION" --qwindowgeometry 1100x700 >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

cleanup_kmux() {
    if [[ -n "${KMUX_PID:-}" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    if [[ -e "$SOCKET" ]]; then
        tmux -S "$SOCKET" kill-server 2>/dev/null || true
    fi
}
trap cleanup_kmux EXIT

# Match by WM_CLASS, not name: Qt's "Qt Selection Owner for kmux" auxiliary
# helper window matches a substring --name search before any MainWindow exists.
WIN=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        echo "FAIL: kmux exited before window appeared (see $LOGDIR/kmux.log)" >&2
        exit 1
    fi
    WIN=$(xdotool search --onlyvisible --class kmux 2>/dev/null | tail -1 || true)
    [[ -n "$WIN" ]] && break
    sleep 0.2
done
[[ -n "$WIN" ]] || { echo "FAIL: kmux window never appeared (see $LOGDIR/kmux.log)" >&2; exit 1; }
echo "OK: kmux window appeared (winid=$WIN)"

# Activate the kmux window so subsequent xdotool keystrokes land on it.
# Under twm's focus-follows-mouse the pointer (parked at 0,0 by Xvfb)
# decides who gets keystrokes unless we force it.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
# Give the bridge time to finish its initial control-mode handshake.
sleep 1.5

INITIAL_WINDOWS=$(tmux -S "$SOCKET" list-windows -t "$SESSION" 2>/dev/null | wc -l)
INITIAL_PANES=$(tmux -S "$SOCKET" list-panes -s -t "$SESSION" 2>/dev/null | wc -l)
if (( INITIAL_WINDOWS != 1 || INITIAL_PANES != 1 )); then
    echo "FAIL: starting state windows=$INITIAL_WINDOWS panes=$INITIAL_PANES (expected 1/1)" >&2
    exit 1
fi
echo "OK: starting state — 1 window, 1 pane"

# Polls tmux until the line count of the given subcommand equals $target.
# Args: <target> <description for failure log> <tmux subcommand...>
wait_for_tmux_count() {
    local target="$1" what="$2"
    shift 2
    local got=
    for _ in $(seq 1 50); do
        got=$(tmux -S "$SOCKET" "$@" 2>/dev/null | wc -l)
        if (( got == target )); then
            return 0
        fi
        sleep 0.2
    done
    echo "FAIL: $what — got $got, expected $target" >&2
    return 1
}

DELAY_MS=150

echo "=== step 1: create a 2nd tab (Ctrl+Shift+T) ==="
xdotool key --delay "$DELAY_MS" ctrl+shift+t
wait_for_tmux_count 2 "windows after new-tab" \
    list-windows -t "$SESSION" || exit 1
echo "OK: tmux now has 2 windows"

echo "=== step 2: split the new tab (Ctrl+ParenLeft) ==="
xdotool key --delay "$DELAY_MS" ctrl+parenleft
wait_for_tmux_count 3 "total panes after first split" \
    list-panes -s -t "$SESSION" || exit 1
echo "OK: total panes = 3"

echo "=== step 3: switch back to original tab (Shift+Left) ==="
# "Previous Tab" is bound to Shift+Left and Ctrl+PageUp. Using Shift+Left
# rather than Ctrl+PageUp because Konsole's default keytab matches Ctrl+PgUp
# and sends "\E[5;*~" to the pty, swallowing the keystroke. Plain Shift+Left
# at a bash prompt matches no keytab rule (only +AppScreen / +Alt / +Ctrl
# variants exist), so it falls through to the previous-tab QAction.
xdotool key --delay "$DELAY_MS" shift+Left
sleep 0.5

echo "=== step 4: split the original tab (Ctrl+ParenLeft) ==="
xdotool key --delay "$DELAY_MS" ctrl+parenleft
wait_for_tmux_count 4 "total panes after second split" \
    list-panes -s -t "$SESSION" || exit 1
echo "OK: total panes = 4"

echo "=== verifying each tab has exactly 2 panes ==="
mapfile -t WINDOW_INFO < <(tmux -S "$SOCKET" list-windows -t "$SESSION" \
    -F '#{window_index} #{window_panes}' 2>/dev/null)

if (( ${#WINDOW_INFO[@]} != 2 )); then
    echo "FAIL: expected 2 windows, got ${#WINDOW_INFO[@]}" >&2
    printf '  %s\n' "${WINDOW_INFO[@]}" >&2
    exit 1
fi

fail=0
for line in "${WINDOW_INFO[@]}"; do
    idx=${line%% *}
    panes=${line#* }
    if (( panes != 2 )); then
        echo "  window $idx: $panes panes (expected 2)"
        fail=1
    else
        echo "  window $idx: $panes panes"
    fi
done

if (( fail )); then
    echo "" >&2
    echo "===== diagnostics =====" >&2
    echo "--- tmux list-windows ---" >&2
    tmux -S "$SOCKET" list-windows -t "$SESSION" \
        -F '#{window_index} #{window_panes} active=#{window_active}' >&2 || true
    echo "--- tmux list-panes -s ---" >&2
    tmux -S "$SOCKET" list-panes -s -t "$SESSION" \
        -F '#{window_index}.#{pane_index} #{pane_id} active=#{pane_active}' >&2 || true
    echo "--- bridge: new-window / split-window / select-window traffic ---" >&2
    grep -aE 'new-window|split-window|select-window|%window-add|%layout-change' \
        "$LOGDIR/kmux.log" >&2 || true
    echo "(full kmux output: $LOGDIR/kmux.log)" >&2
    exit 1
fi

echo "PASS: each tab has 2 panes from tmux's viewpoint"
exit 0
