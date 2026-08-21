#!/usr/bin/env bash
# Regression: after a control transport reconnects while an older client keeps
# the session at 80x24, kmux must advertise every existing window's size again.

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
WRAPPER_COUNT="$HOMEDIR/rsh-wrapper.count"
RECONNECT_WAIT="$HOMEDIR/reconnect-waiting"
RECONNECT_GATE="$HOMEDIR/reconnect-gate"
STALE_INPUT="$HOMEDIR/stale-client-input"
KMUX_PID=""
STALE_PID=""
STALE_FD_OPEN=0

mkfifo "$RECONNECT_GATE" "$STALE_INPUT"

cat >"$WRAPPER" <<WRAPPER_EOF
#!/usr/bin/env bash
set -uo pipefail
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
exec "\$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 80 -y 24 'sleep 600'
tmux -S "$SOCKET" new-window -d -t "$SESSION" 'sleep 600'
tmux -S "$SOCKET" set-option -g automatic-rename off
tmux -S "$SOCKET" set-option -g window-size latest

cleanup_tmux() {
    local rc=$1
    if (( STALE_FD_OPEN == 1 )); then
        exec 9>&-
        STALE_FD_OPEN=0
    fi
    if [[ -n "$STALE_PID" ]] && kill -0 "$STALE_PID" 2>/dev/null; then
        kill "$STALE_PID" 2>/dev/null || true
        wait "$STALE_PID" 2>/dev/null || true
    fi
    if [[ -n "$KMUX_PID" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    if [[ -e "$SOCKET" ]]; then
        tmux -S "$SOCKET" kill-server 2>/dev/null || true
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
    echo "--- stale client traffic ---" >&2
    tail -80 "$LOGDIR/stale-client.log" >&2 2>/dev/null || true
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

# Keep a second control client connected just like the orphaned SSH-created
# clients seen in the live session. Its input stays open through fd 9 and its
# output is drained to a file so control-mode flow control cannot hide the
# sizing behavior under test.
exec 9<>"$STALE_INPUT"
STALE_FD_OPEN=1
(
    exec 9>&-
    exec tmux -S "$SOCKET" -u -C attach-session -t "$SESSION" <"$STALE_INPUT" >"$LOGDIR/stale-client.log" 2>&1
) &
STALE_PID=$!

stale_ready=0
for _ in $(seq 1 50); do
    if (( $(client_count) == 2 )); then
        stale_ready=1
        break
    fi
    sleep 0.1
done
if (( stale_ready != 1 )); then
    dump_state
    kmux_test_bail 2 "stale control client did not attach"
fi
echo "OK: second control client attached (pid=$STALE_PID)"

# Crash kmux's transport without sending tmux's deliberate %exit notification.
# The --rsh wrapper gates the replacement attach-session so the surviving
# client can recreate the observed 80x24 state deterministically.
kill -KILL "$PRIMARY_PID"

reconnect_waiting=0
for _ in $(seq 1 50); do
    if [[ -e "$RECONNECT_WAIT" ]] && (( $(client_count) == 1 )); then
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
    printf 'select-window -t %s\n' "$window_id" >&9
    printf 'refresh-client -C %s:80x24\n' "$window_id" >&9
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
echo "OK: surviving control client reduced both windows to tmux's 80x24 default"

printf 'reconnect\n' >"$RECONNECT_GATE"

reconnected=0
for _ in $(seq 1 100); do
    if (( $(client_count) == 2 )) && [[ $(<"$WRAPPER_COUNT") == "2" ]]; then
        reconnected=1
        break
    fi
    sleep 0.1
done
if (( reconnected != 1 )); then
    dump_state
    kmux_test_bail 2 "kmux did not attach a replacement control client"
fi
echo "OK: kmux reconnected while the older control client remained attached"

# Exercise the normal user path after reconnect. Changing tabs schedules
# sendClientSize(), which must advertise sizes to this new tmux client even
# when the pixel dimensions equal values cached for the dead client.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool key --delay 150 ctrl+Tab

for _ in $(seq 1 50); do
    if all_windows_larger_than_default; then
        echo "PASS: reconnect restored every existing window: $(window_sizes | paste -sd ' ' -)"
        exit 0
    fi
    sleep 0.2
done

echo "FAIL (bug reproduced): existing windows stayed at tmux's 80x24 default after kmux reconnected" >&2
dump_state
exit 1
