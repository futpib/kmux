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
export KMUX_TEST_NEED_WM="${KMUX_TEST_NEED_WM:-1}"
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

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

cleanup_tmux() {
    local rc=$1
    if [[ -n "$KMUX_PID" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    if [[ -e "$SOCKET" ]]; then
        tmux -S "$SOCKET" kill-server 2>/dev/null || true
    fi
    if [[ -r "$TRANSPORT_CHILDREN_FILE" ]]; then
        while IFS= read -r child_pid; do
            if [[ "$child_pid" =~ ^[0-9]+$ ]] && kill -0 "$child_pid" 2>/dev/null; then
                kill "$child_pid" 2>/dev/null || true
            fi
        done <"$TRANSPORT_CHILDREN_FILE"
    fi
    return "$rc"
}
trap 'cleanup_tmux "$?"; kmux_test__cleanup' EXIT

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
    echo "--- tmux clients ---" >&2
    tmux -S "$SOCKET" list-clients -t "$SESSION" \
        -F 'client=#{client_name} pid=#{client_pid} flags=#{client_flags} width=#{client_width}' >&2 2>&1 || true
    echo "--- tmux windows ---" >&2
    window_sizes >&2 2>&1 || true
    echo "--- reconnect and resize traffic ---" >&2
    { grep -aE 'attach-session|reconnect|sendClientSize|refresh-client|size unchanged|size changed' \
        "$LOGDIR/kmux.log" 2>/dev/null | tail -160; } >&2 || true
}

echo "=== launching kmux through a reconnect-gated --rsh wrapper ==="
"$KMUX" --rsh "$WRAPPER" -S "$SOCKET" -s "$SESSION" --qwindowgeometry 1100x700 >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

WIN=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        tail -80 "$LOGDIR/kmux.log" >&2 2>/dev/null || true
        kmux_test_bail 2 "kmux exited before its window appeared"
    fi
    WIN=$(xdotool search --onlyvisible --class kmux 2>/dev/null | tail -1 || true)
    [[ -n "$WIN" ]] && break
    sleep 0.2
done
[[ -n "$WIN" ]] || kmux_test_bail 2 "kmux window never appeared"

xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool windowsize --sync "$WIN" 900 600 2>/dev/null || true
xdotool windowsize --sync "$WIN" 1000 650 2>/dev/null || true

initial_ready=0
for _ in $(seq 1 60); do
    if all_windows_larger_than_default && (( $(client_count) == 1 )); then
        initial_ready=1
        break
    fi
    sleep 0.2
done
if (( initial_ready != 1 )); then
    dump_state
    kmux_test_bail 2 "initial kmux client did not size both windows beyond 80x24"
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

reconnect_waiting=0
for _ in $(seq 1 50); do
    if [[ -e "$RECONNECT_WAIT" ]] && (( $(client_count) == 1 )) && kill -0 "$PRIMARY_PID" 2>/dev/null; then
        reconnect_waiting=1
        break
    fi
    sleep 0.1
done
if (( reconnect_waiting != 1 )); then
    dump_state
    kmux_test_bail 2 "kmux did not enter its gated reconnect after transport death"
fi

WINDOW_IDS=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}')
while IFS= read -r window_id; do
    tmux -S "$SOCKET" refresh-client -t "$PRIMARY_NAME" -C "$window_id:80x24"
done <<<"$WINDOW_IDS"

default_ready=0
for _ in $(seq 1 50); do
    if all_windows_at_default; then
        default_ready=1
        break
    fi
    sleep 0.1
done
if (( default_ready != 1 )); then
    dump_state
    kmux_test_bail 2 "surviving control client did not establish the 80x24 precondition"
fi
echo "OK: orphaned kmux control client reduced both windows to tmux's 80x24 default"

printf 'reconnect\n' >"$RECONNECT_GATE"

reconnected=0
for _ in $(seq 1 100); do
    if [[ $(<"$WRAPPER_COUNT") == "2" ]] \
        && tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_pid}' | grep -qvxF "$PRIMARY_PID"; then
        reconnected=1
        break
    fi
    sleep 0.1
done
if (( reconnected != 1 )); then
    dump_state
    kmux_test_bail 2 "kmux did not attach a replacement control client"
fi
echo "OK: kmux attached a replacement control client"

# Exercise the normal user path after reconnect. Changing tabs schedules
# sendClientSize(), which must advertise sizes to this new tmux client even
# when the pixel dimensions equal values cached for the dead client.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool key --delay 150 ctrl+Tab

for _ in $(seq 1 50); do
    if all_windows_larger_than_default \
        && ! tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_name}' | grep -qxF "$PRIMARY_NAME"; then
        echo "PASS: reconnect restored every existing window: $(window_sizes | paste -sd ' ' -)"
        exit 0
    fi
    sleep 0.2
done

echo "FAIL (bug reproduced): kmux left its old control client attached and existing windows stayed at 80x24" >&2
dump_state
exit 1
