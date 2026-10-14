#!/usr/bin/env bash
# Verify that the notification used by the tree switcher changes the visible
# kmux tab. Read the active session over D-Bus instead of relying on X titles,
# which need not update when the window manager has not delivered focus.
# shellcheck disable=SC2119,SC2329

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"
kmux_test_setup --wm --require tmux --require qdbus6

SOCKET="$HOMEDIR/tmux.sock"
SESSION="tree-switch"
kmux_test_register_tmux_socket "$SOCKET"

tmux -S "$SOCKET" -f /dev/null new-session -d -s "$SESSION" -n alpha -x 120 -y 30 'exec sleep 3600'
tmux -S "$SOCKET" new-window -d -t "$SESSION":1 -n beta 'exec sleep 3600'
tmux -S "$SOCKET" new-window -d -t "$SESSION":2 -n gamma 'exec sleep 3600'

kmux_test_start "$LOGDIR/kmux.log" --separate -S "$SOCKET" -s "$SESSION" --builtin-profile --qwindowgeometry 1100x700
KMUX_PID=$KMUX_TEST_PID
kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 20 || kmux_test_fail
WIN=$KMUX_TEST_WINDOW_ID
kmux_test_wait_dbus_service "$KMUX_PID" || kmux_test_infra
SERVICE=$KMUX_TEST_DBUS_SERVICE
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool windowfocus --sync "$WIN"

SESSION_IDS=()
tabs_ready() {
    mapfile -t SESSION_IDS < <(qdbus6 "$SERVICE" /Windows/1 org.kde.konsole.Window.sessionList 2>/dev/null)
    [[ ${#SESSION_IDS[@]} == 3 ]]
}
active_tab_is() {
    [[ $(qdbus6 "$SERVICE" /Windows/1 org.kde.konsole.Window.currentSession) == "${SESSION_IDS[$1]}" ]]
}
initial_tab_ready() {
    active_tab_is 0 && [[ $(tmux -S "$SOCKET" display-message -p -t "$SESSION" '#{window_index}') == 0 ]]
}

kmux_test_wait_until 10 "all three tmux windows to appear as kmux tabs" tabs_ready || kmux_test_infra
# Establish the visible starting tab, not just tmux's current window. Focus
# during attachment can select another tab before the test begins.
qdbus6 "$SERVICE" /Windows/1 org.kde.konsole.Window.setCurrentSession "${SESSION_IDS[0]}"
kmux_test_wait_until 10 "the first tab to be active in kmux and tmux" initial_tab_ready || kmux_test_infra

# The tree switcher sends select-window through its control client. Issuing
# the same command externally produces the same notification for kmux.
tmux -S "$SOCKET" select-window -t "$SESSION":1
if ! kmux_test_wait_until 10 "kmux to display the second tab after select-window" active_tab_is 1; then
    kmux_test_dump_tmux "$SOCKET" "$SESSION"
    kmux_test_dump_log "$LOGDIR/kmux.log"
    kmux_test_fail
fi
kmux_test_pass "server-side window selection switched the visible kmux tab"
