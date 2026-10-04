#!/usr/bin/env bash
# Poll predicates are passed by name and outcome helpers accept optional messages.
# shellcheck disable=SC2119,SC2329
# A background kmux must keep advertising its own capacity while another
# client constrains the layout, so every tab recovers when that client leaves.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.resize.debug=true;konsole.tmux.layout.debug=true;konsole.tmux.controller.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
kmux_test_setup --wm --require tmux --require xmessage

SOCKET="$HOMEDIR/tmux.sock"
SESSION="smaller-client"
kmux_test_register_tmux_socket "$SOCKET"
tmux -S "$SOCKET" -f /dev/null new-session -d -s "$SESSION" -x 120 -y 24 'sleep 600'
tmux -S "$SOCKET" set-option -g window-size latest
tmux -S "$SOCKET" new-window -d -t "$SESSION" 'sleep 600'
tmux -S "$SOCKET" split-window -h -t "$SESSION:1" 'sleep 600'
tmux -S "$SOCKET" split-window -v -t "$SESSION:1" 'sleep 600'

dump_state() {
    kmux_test_dump_tmux "$SOCKET" "$SESSION"
    kmux_test_dump_log "$LOGDIR/kmux.log" 100
}

window_sizes() {
    tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_width} #{window_height}'
}

windows_large() {
    local cols rows
    while read -r cols rows; do
        (( cols > 75 && rows > 30 )) || return 1
    done < <(window_sizes)
}

windows_small() {
    local cols rows
    while read -r cols rows; do
        (( cols == 75 && rows == 30 )) || return 1
    done < <(window_sizes)
}

kmux_test_start "$LOGDIR/kmux.log" -S "$SOCKET" -s "$SESSION" --qwindowgeometry 1100x700
KMUX_PID=$KMUX_TEST_PID
kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 20 || kmux_test_fail
WIN=$KMUX_TEST_WINDOW_ID
desktop_sample=""
desktop_stable_samples=0
client_sizes_advertised() {
    grep -q 'sendClientSize: windowId= 0 size changed' "$LOGDIR/kmux.log" \
        && grep -q 'sendClientSize: windowId= 1 size changed' "$LOGDIR/kmux.log"
}
desktop_ready() {
    windows_large || return 1
    local sample
    sample=$(window_sizes)
    if [[ "$sample" == "$desktop_sample" ]]; then
        desktop_stable_samples=$((desktop_stable_samples + 1))
    else
        desktop_stable_samples=0
        desktop_sample=$sample
    fi
    (( desktop_stable_samples >= 5 ))
}
kmux_test_wait_until 10 "kmux to advertise both tabs' sizes" client_sizes_advertised || { dump_state; kmux_test_fail; }
xdotool windowactivate --sync "$WIN" 2>/dev/null || xdotool windowfocus --sync "$WIN"
for index in 1 0; do
    tmux -S "$SOCKET" select-window -t "$SESSION:$index"
    desktop_stable_samples=0
    desktop_sample=""
    kmux_test_wait_until 10 "window $index to finish its initial resize" desktop_ready || { dump_state; kmux_test_fail; }
done
window_sizes >"$HOMEDIR/desktop-sizes"
echo "Initial desktop sizes: $(paste -sd ';' "$HOMEDIR/desktop-sizes")"

# Keep the desktop unfocused while the phone changes tmux's layout.
kmux_test_start_process "$LOGDIR/focus.log" xmessage -title kmux-resize-focus -buttons OK:0 'Resize test focus holder'
focus_window_ready() {
    FOCUS_WIN=$(xdotool search --onlyvisible --name '^kmux-resize-focus$' | head -n 1)
    [[ -n "$FOCUS_WIN" ]]
}
kmux_test_wait_until 5 "focus holder window to appear" focus_window_ready || kmux_test_fail
xdotool windowfocus --sync "$FOCUS_WIN"

mkfifo "$HOMEDIR/phone-input"
exec {phone_input}<>"$HOMEDIR/phone-input"
# The child shell must redirect stdin itself; an asynchronous shell command
# otherwise gets /dev/null even when its enclosing function has a redirect.
# shellcheck disable=SC2016
kmux_test_start_process "$LOGDIR/phone.log" bash -c 'exec tmux -S "$1" -C attach-session -t "$2" <"$3"' \
    _ "$SOCKET" "$SESSION" "$HOMEDIR/phone-input"
PHONE_PID=$KMUX_TEST_PID
PHONE_CLIENT="client-$PHONE_PID"
phone_attached() {
    tmux -S "$SOCKET" list-clients -F '#{client_name}' | grep -qxF "$PHONE_CLIENT"
}
kmux_test_wait_until 5 "phone control client to attach" phone_attached || { dump_state; kmux_test_dump_log "$LOGDIR/phone.log"; kmux_test_fail; }
while read -r window_id; do
    tmux -S "$SOCKET" refresh-client -t "$PHONE_CLIENT" -C "$window_id:75x30"
done < <(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}')
kmux_test_wait_until 10 "phone to constrain both tabs" windows_small || { dump_state; kmux_test_fail; }

constrained_layout_received() {
    [[ $(grep -c 'constrainSplitterToLayout: layout= 75 x 30' "$LOGDIR/kmux.log") -ge 2 ]]
}
kmux_test_wait_until 5 "kmux to apply the smaller layout" constrained_layout_received || kmux_test_fail

# A focus cycle schedules a normal size advertisement without changing the
# desktop geometry. Finish unfocused so layout handling retains the constraint.
xdotool windowfocus --sync "$WIN"
xdotool windowfocus --sync "$FOCUS_WIN"
sleep 0.5
windows_small || { dump_state; kmux_test_fail "phone no longer constrains the session"; }

tmux -S "$SOCKET" detach-client -t "$PHONE_CLIENT"
exec {phone_input}>&-
# tmux defers applying a hidden window's pending resize until it is selected.
# Visit both while keeping the GUI unfocused and its geometry unchanged.
window_large() {
    local cols rows
    read -r cols rows < <(tmux -S "$SOCKET" display-message -p -t "$SESSION:$1" '#{window_width} #{window_height}')
    (( cols > 75 && rows > 30 ))
}
for index in 1 0; do
    tmux -S "$SOCKET" select-window -t "$SESSION:$index"
    kmux_test_wait_until 10 "window $index to recover after the phone disconnects" window_large "$index" || { dump_state; kmux_test_fail; }
done
desktop_sizes_restored() {
    local cols rows old_cols old_rows
    while read -r cols rows old_cols old_rows; do
        (( cols >= old_cols - 1 && cols <= old_cols + 1 \
            && rows >= old_rows - 1 && rows <= old_rows + 1 )) || return 1
    done < <(paste <(window_sizes) "$HOMEDIR/desktop-sizes")
}
kmux_test_wait_until 10 "both tabs to regain their desktop capacity" desktop_sizes_restored || { dump_state; kmux_test_fail; }
[[ $(xdotool getwindowfocus) == "$FOCUS_WIN" ]] || kmux_test_fail "desktop unexpectedly took focus"
[[ $(tmux -S "$SOCKET" list-clients -F '#{client_name}' | wc -l) == 1 ]] || kmux_test_fail "unexpected surviving client count"
kmux_test_pass "unfocused desktop recovered both tabs: $(window_sizes | paste -sd ';' -)"
