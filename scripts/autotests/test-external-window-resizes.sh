#!/usr/bin/env bash
# Regression: a tmux window created by another process must be sized to an
# existing kmux frame instead of remaining at tmux's 80x24 default when a
# second, window-restricted kmux control client is attached.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.resize.debug=true;konsole.tmux.controller.debug=true;konsole.tmux.bridge.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
export KMUX_TEST_NEED_WM="${KMUX_TEST_NEED_WM:-1}"
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

SOCKET="$HOMEDIR/tmux.sock"
SESSION="external-size"

tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 80 -y 24
tmux -S "$SOCKET" set-option -g automatic-rename off 2>/dev/null || true

"$KMUX" -S "$SOCKET" -s "$SESSION" --qwindowgeometry 1100x700 >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

cleanup_tmux() {
    local rc=$1
    if [[ -n "${KMUX_PID:-}" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    if [[ -e "$SOCKET" ]]; then
        tmux -S "$SOCKET" kill-server 2>/dev/null || true
    fi
    return "$rc"
}
trap 'cleanup_tmux "$?"; kmux_test__cleanup' EXIT

window_size() {
    tmux -S "$SOCKET" display-message -p -t "$1" '#{window_width}x#{window_height}' 2>/dev/null || echo '?'
}

dump_state() {
    echo "--- tmux state ---" >&2
    tmux -S "$SOCKET" list-clients -F 'client=#{client_name} flags=#{client_flags}' >&2 2>&1 || true
    tmux -S "$SOCKET" list-windows -t "$SESSION" \
        -F 'window=#{window_id}:#{window_index} size=#{window_width}x#{window_height} active=#{window_active}' >&2 2>&1 || true
    echo "--- resize traffic ---" >&2
    { grep -aE 'refresh-client|%window-add|%layout-change|sendClientSize|setWindowSize|ApplyingLayout' \
        "$LOGDIR/kmux.log" 2>/dev/null | tail -100; } >&2 || true
}

count_visible_kmux_windows() {
    xdotool search --onlyvisible --class kmux 2>/dev/null | wc -l
}

WIN=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        tail -50 "$LOGDIR/kmux.log" >&2 2>/dev/null || true
        kmux_test_bail 2 "kmux exited before its window appeared"
    fi
    WIN=$(xdotool search --onlyvisible --class kmux 2>/dev/null | tail -1 || true)
    [[ -n "$WIN" ]] && break
    sleep 0.2
done
[[ -n "$WIN" ]] || kmux_test_bail 2 "kmux window never appeared"

# Start from a settled, frame-sized session. The regression concerns windows
# added after this point, so an explicit initial resize removes the separate
# startup-size race from the setup.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool windowsize --sync "$WIN" 900 600 2>/dev/null || true
xdotool windowsize --sync "$WIN" 1000 650 2>/dev/null || true

ORIGINAL_WID=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}' | head -1)
ORIGINAL_SIZE=""
for _ in $(seq 1 50); do
    ORIGINAL_SIZE=$(window_size "$ORIGINAL_WID")
    original_cols=${ORIGINAL_SIZE%x*}
    original_rows=${ORIGINAL_SIZE#*x}
    if [[ "$original_cols" =~ ^[0-9]+$ && "$original_rows" =~ ^[0-9]+$ ]] \
        && (( original_cols > 80 && original_rows > 24 )); then
        break
    fi
    sleep 0.2
done
if ! [[ "$original_cols" =~ ^[0-9]+$ && "$original_rows" =~ ^[0-9]+$ ]] \
    || (( original_cols <= 80 || original_rows <= 24 )); then
    dump_state
    kmux_test_bail 2 "initial window did not negotiate beyond 80x24 (got $ORIGINAL_SIZE)"
fi
echo "OK: initial window negotiated to $ORIGINAL_SIZE"

# Kmux's New Window action adds a second top-level window backed by its own
# control-mode client. That client is restricted to the window it owns, while
# the original client hides that window. This is the multi-client state in
# which externally added slopd windows have remained at 80x24.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool key --delay 150 ctrl+shift+n

second_ready=0
for _ in $(seq 1 100); do
    tmux_windows=$(tmux -S "$SOCKET" list-windows -t "$SESSION" 2>/dev/null | wc -l)
    tmux_clients=$(tmux -S "$SOCKET" list-clients 2>/dev/null | wc -l)
    gui_windows=$(count_visible_kmux_windows)
    if (( tmux_windows == 2 && tmux_clients == 2 && gui_windows == 2 )); then
        second_ready=1
        break
    fi
    sleep 0.2
done
if (( second_ready != 1 )); then
    dump_state
    kmux_test_bail 2 "kmux New Window did not establish two tmux windows, GUI windows, and control clients"
fi
echo "OK: second kmux window and restricted control client attached"

# Keep both GUI geometries unchanged from here onward. A separate process
# creates a detached window exactly like slopd or a shell-side tmux command.
KNOWN_WIDS=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}')
tmux -S "$SOCKET" new-window -d -t "$SESSION"
NEW_WID=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}' | grep -vxF -f <(printf '%s\n' "$KNOWN_WIDS") | head -1)
[[ -n "$NEW_WID" ]] || kmux_test_bail 2 "external tmux window was not created"

NEW_SIZE=""
for _ in $(seq 1 50); do
    NEW_SIZE=$(window_size "$NEW_WID")
    new_cols=${NEW_SIZE%x*}
    new_rows=${NEW_SIZE#*x}
    if [[ "$new_cols" =~ ^[0-9]+$ && "$new_rows" =~ ^[0-9]+$ ]] \
        && (( new_cols > 80 && new_rows > 24 )); then
        break
    fi
    sleep 0.2
done

if ! [[ "$new_cols" =~ ^[0-9]+$ && "$new_rows" =~ ^[0-9]+$ ]] \
    || (( new_cols <= 80 || new_rows <= 24 )); then
    echo "FAIL: externally created window remained constrained at $NEW_SIZE; expected kmux to advertise the existing frame size" >&2
    dump_state
    exit 1
fi

echo "PASS: externally created window grew from tmux's 80x24 default to $NEW_SIZE"
exit 0
