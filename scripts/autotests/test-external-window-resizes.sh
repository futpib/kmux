#!/usr/bin/env bash
# Poll predicates are passed by name and outcome helpers accept optional messages.
# shellcheck disable=SC2119,SC2329
# Regression: a tmux window created by another process must be sized to an
# existing kmux frame instead of remaining at tmux's 80x24 default when a
# second, window-restricted kmux control client is attached.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.resize.debug=true;konsole.tmux.controller.debug=true;konsole.tmux.bridge.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
kmux_test_setup --wm --require tmux

SOCKET="$HOMEDIR/tmux.sock"
SESSION="external-size"
kmux_test_register_tmux_socket "$SOCKET"

tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 80 -y 24
tmux -S "$SOCKET" set-option -g automatic-rename off 2>/dev/null || true

kmux_test_start "$LOGDIR/kmux.log" -S "$SOCKET" -s "$SESSION" --qwindowgeometry 1100x700
KMUX_PID=$KMUX_TEST_PID

window_size() {
    tmux -S "$SOCKET" display-message -p -t "$1" '#{window_width}x#{window_height}' 2>/dev/null || echo '?'
}

dump_state() {
    kmux_test_dump_tmux "$SOCKET" "$SESSION"
    echo "--- resize traffic ---" >&2
    { grep -aE 'refresh-client|%window-add|%layout-change|sendClientSize|setWindowSize|ApplyingLayout' \
        "$LOGDIR/kmux.log" 2>/dev/null | tail -100; } >&2 || true
}

count_visible_kmux_windows() {
    xdotool search --onlyvisible --class kmux 2>/dev/null | wc -l
}

if ! kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 20; then
    kmux_test_fail
fi
WIN=$KMUX_TEST_WINDOW_ID

ORIGINAL_WID=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}' | head -1)
ORIGINAL_SIZE=""
initial_window_sized() {
    ORIGINAL_SIZE=$(window_size "$ORIGINAL_WID")
    original_cols=${ORIGINAL_SIZE%x*}
    original_rows=${ORIGINAL_SIZE#*x}
    [[ "$original_cols" =~ ^[0-9]+$ && "$original_rows" =~ ^[0-9]+$ ]] \
        && (( original_cols > 80 && original_rows > 24 ))
}

# A visible top-level X window does not guarantee that its terminal view has
# finished the control-mode handshake and installed its resize handling. CI
# can expose the window just before that point, causing a one-shot xdotool
# resize to be lost and leaving this test's unrelated setup at tmux's 80x24
# default. Wait for the initial view to settle, then retry distinct frame
# sizes until kmux has advertised a real cell size. The assertion below still
# tests only windows created after this precondition is established.
sleep 1
for _ in 1 2 3; do
    xdotool windowactivate --sync "$WIN" 2>/dev/null || true
    xdotool windowsize --sync "$WIN" 900 600 2>/dev/null || true
    sleep 0.2
    xdotool windowsize --sync "$WIN" 1000 650 2>/dev/null || true
    sleep 1
    initial_window_sized && break
done

if ! kmux_test_wait_until 10 "initial tmux window to exceed 80x24" initial_window_sized; then
    dump_state
    echo "FAIL: initial window did not negotiate beyond 80x24 (got $ORIGINAL_SIZE)" >&2
    kmux_test_fail
fi
echo "OK: initial window negotiated to $ORIGINAL_SIZE"

# Kmux's New Window action adds a second top-level window backed by its own
# control-mode client. That client is restricted to the window it owns, while
# the original client hides that window. This is the multi-client state in
# which externally added slopd windows have remained at 80x24.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool key --delay 150 ctrl+shift+n

second_kmux_window_ready() {
    tmux_windows=$(tmux -S "$SOCKET" list-windows -t "$SESSION" 2>/dev/null | wc -l)
    tmux_clients=$(tmux -S "$SOCKET" list-clients 2>/dev/null | wc -l)
    gui_windows=$(count_visible_kmux_windows)
    (( tmux_windows == 2 && tmux_clients == 2 && gui_windows == 2 ))
}
if ! kmux_test_wait_until 20 "second kmux window and control client" second_kmux_window_ready; then
    dump_state
    kmux_test_fail
fi
echo "OK: second kmux window and restricted control client attached"

# Keep both GUI geometries unchanged from here onward. A separate process
# creates a detached window exactly like slopd or a shell-side tmux command.
KNOWN_WIDS=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}')
tmux -S "$SOCKET" new-window -d -t "$SESSION"
NEW_WID=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}' | grep -vxF -f <(printf '%s\n' "$KNOWN_WIDS") | head -1)
[[ -n "$NEW_WID" ]] || kmux_test_infra "external tmux window was not created"

NEW_SIZE=""
external_window_sized() {
    NEW_SIZE=$(window_size "$NEW_WID")
    new_cols=${NEW_SIZE%x*}
    new_rows=${NEW_SIZE#*x}
    [[ "$new_cols" =~ ^[0-9]+$ && "$new_rows" =~ ^[0-9]+$ ]] \
        && (( new_cols > 80 && new_rows > 24 ))
}
if ! kmux_test_wait_until 10 "external tmux window to exceed 80x24" external_window_sized; then
    echo "FAIL: externally created window remained constrained at $NEW_SIZE; expected kmux to advertise the existing frame size" >&2
    dump_state
    kmux_test_fail
fi

kmux_test_pass "externally created window grew from tmux's 80x24 default to $NEW_SIZE"
