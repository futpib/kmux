#!/usr/bin/env bash
# Regression: after a control transport reconnects while its orphaned server-side
# client keeps the session at 80x24, kmux must detach that predecessor and
# advertise every existing window's size again.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.resize.debug=true;konsole.tmux.controller.debug=true;konsole.tmux.bridge.debug=true;konsole.tmux.reconnect.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
kmux_test_setup --wm --require tmux --require python3

SOCKET="$HOMEDIR/tmux.sock"
SESSION="reconnect-size"
WRAPPER="$HOMEDIR/rsh-wrapper.sh"
ORPHANING_RSH="$SCRIPT_DIR/fixtures/orphaning-rsh.py"
WRAPPER_COUNT="$HOMEDIR/rsh-wrapper.count"
RECONNECT_WAIT="$HOMEDIR/reconnect-waiting"
RECONNECT_GATE="$HOMEDIR/reconnect-gate"
TRANSPORT_PID_FILE="$HOMEDIR/rsh-wrapper.pid"
TRANSPORT_CHILDREN_FILE="$HOMEDIR/rsh-wrapper.children"
KMUX_PID=""
[[ -x "$ORPHANING_RSH" ]] || kmux_test_bail 2 "missing executable fixture: $ORPHANING_RSH"
kmux_test_register_tmux_socket "$SOCKET"

mkfifo "$RECONNECT_GATE"

cat >"$WRAPPER" <<WRAPPER_EOF
#!/usr/bin/env bash
set -uo pipefail
printf '%s\n' "\$\$" >"$TRANSPORT_PID_FILE"
count=0
if [[ -r "$WRAPPER_COUNT" ]]; then
    IFS= read -r count <"$WRAPPER_COUNT"
fi
count=\$((count + 1))
printf '%s\n' "\$count" >"$WRAPPER_COUNT"
if (( count > 1 )); then
    : >"$RECONNECT_WAIT"
    IFS= read -r _ <"$RECONNECT_GATE"
fi
exec "$ORPHANING_RSH" "$TRANSPORT_CHILDREN_FILE" "\$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 80 -y 24 'sleep 600'
tmux -S "$SOCKET" new-window -d -t "$SESSION" 'sleep 600'
tmux -S "$SOCKET" set-option -g automatic-rename off
tmux -S "$SOCKET" set-option -g window-size latest

cleanup_transport_children() {
    if [[ -r "$TRANSPORT_CHILDREN_FILE" ]]; then
        while IFS= read -r child_pid; do
            if [[ "$child_pid" =~ ^[0-9]+$ ]] && kill -0 "$child_pid" 2>/dev/null; then
                kill "$child_pid" 2>/dev/null || true
            fi
        done <"$TRANSPORT_CHILDREN_FILE"
    fi
}
kmux_test_add_cleanup cleanup_transport_children

window_sizes() {
    tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}=#{window_width}x#{window_height}' 2>/dev/null
}

all_windows_larger_than_default() {
    local size cols rows
    while IFS='=' read -r _ size; do
        cols=${size%x*}
        rows=${size#*x}
        if ! [[ "$cols" =~ ^[0-9]+$ && "$rows" =~ ^[0-9]+$ ]] || (( cols <= 80 || rows <= 24 )); then
            return 1
        fi
    done < <(window_sizes)
}

all_windows_at_default() {
    local size
    while IFS='=' read -r _ size; do
        [[ "$size" == "80x24" ]] || return 1
    done < <(window_sizes)
}

client_count() {
    tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_pid}' 2>/dev/null | wc -l
}

dump_state() {
    kmux_test_dump_tmux "$SOCKET" "$SESSION"
    echo "--- reconnect and resize traffic ---" >&2
    { grep -aE 'attach-session|reconnect|sendClientSize|refresh-client|size unchanged|size changed' \
        "$LOGDIR/kmux.log" 2>/dev/null | tail -160; } >&2 || true
}

echo "=== launching kmux through a reconnect-gated --rsh wrapper ==="
kmux_test_start "$LOGDIR/kmux.log" --rsh "$WRAPPER" -S "$SOCKET" -s "$SESSION" --qwindowgeometry 1100x700
KMUX_PID=$KMUX_TEST_PID
if ! kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 20; then
    exit 1
fi
WIN=$KMUX_TEST_WINDOW_ID

xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool windowsize --sync "$WIN" 900 600 2>/dev/null || true
xdotool windowsize --sync "$WIN" 1000 650 2>/dev/null || true

initial_client_ready() {
    all_windows_larger_than_default && (( $(client_count) == 1 ))
}
if ! kmux_test_wait_until 12 "initial client to size both windows" initial_client_ready; then
    dump_state
    exit 1
fi
echo "OK: initial kmux client sized both windows: $(window_sizes | paste -sd ' ' -)"

PRIMARY_PID=$(tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_pid}')
[[ "$PRIMARY_PID" =~ ^[0-9]+$ ]] || kmux_test_bail 2 "could not identify kmux's initial control client"
PRIMARY_NAME=$(tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_name}')
[[ -n "$PRIMARY_NAME" ]] || kmux_test_bail 2 "could not identify kmux's initial control client name"
TRANSPORT_PID=$(<"$TRANSPORT_PID_FILE")
[[ "$TRANSPORT_PID" =~ ^[0-9]+$ ]] || kmux_test_bail 2 "could not identify kmux's wrapped transport"

# Crash the wrapper which stands in for ssh, not the tmux process behind it.
# The proxy leaves that server-side control client alive just like the two
# orphaned SSH-created clients found in the live session. The wrapper gates the
# replacement attach-session so the old client can recreate 80x24 first.
kill -KILL "$TRANSPORT_PID"

reconnect_is_gated() {
    [[ -e "$RECONNECT_WAIT" ]] && (( $(client_count) == 1 )) && kill -0 "$PRIMARY_PID" 2>/dev/null
}
if ! kmux_test_wait_until 5 "kmux to enter its gated reconnect" reconnect_is_gated; then
    dump_state
    exit 1
fi

WINDOW_IDS=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}')
while IFS= read -r window_id; do
    tmux -S "$SOCKET" refresh-client -t "$PRIMARY_NAME" -C "$window_id:80x24"
done <<<"$WINDOW_IDS"

if ! kmux_test_wait_until 5 "surviving client to establish the 80x24 precondition" all_windows_at_default; then
    dump_state
    exit 1
fi
echo "OK: orphaned kmux control client reduced both windows to tmux's 80x24 default"

printf 'reconnect\n' >"$RECONNECT_GATE"

replacement_client_attached() {
    if [[ $(<"$WRAPPER_COUNT") == "2" ]] \
        && tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_pid}' | grep -qvxF "$PRIMARY_PID"; then
        return 0
    fi
    return 1
}
if ! kmux_test_wait_until 10 "replacement control client to attach" replacement_client_attached; then
    dump_state
    exit 1
fi
echo "OK: kmux attached a replacement control client"

# Exercise the normal user path after reconnect. Changing tabs schedules
# sendClientSize(), which must advertise sizes to this new tmux client even
# when the pixel dimensions equal values cached for the dead client.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool key --delay 150 ctrl+Tab

sizes_restored() {
    if all_windows_larger_than_default \
        && ! tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_name}' | grep -qxF "$PRIMARY_NAME"; then
        return 0
    fi
    return 1
}
if ! kmux_test_wait_until 10 "reconnect to restore every existing window" sizes_restored; then
    echo "FAIL (bug reproduced): kmux left its old control client attached and existing windows stayed at 80x24" >&2
    dump_state
    exit 1
fi

echo "PASS: reconnect restored every existing window: $(window_sizes | paste -sd ' ' -)"
