#!/usr/bin/env bash
# Exercise the nested path: kmux -> local tmux -> ssh -> remote tmux. The
# remote prefix+x binding and prefix+c window creation check that C-b C-b
# passes through local tmux and becomes the remote tmux prefix.
# Poll predicates are invoked indirectly by kmux_test_wait_until.
# shellcheck disable=SC2329

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.gateway.debug=true"
if [[ ${KMUX_TEST_WAYLAND:-0} == 1 ]]; then
    COMPOSITOR_RUNTIME_DIR=$XDG_RUNTIME_DIR
    export DISPLAY=:99
    kmux_test_setup --require tmux --require ssh --require sshd --require ssh-keygen --require python3 --require wtype
    export XDG_RUNTIME_DIR=$COMPOSITOR_RUNTIME_DIR
    export QT_QPA_PLATFORM=wayland
    unset DISPLAY
else
    kmux_test_setup --wm --require tmux --require ssh --require sshd --require ssh-keygen --require python3
fi
KMUX=${KMUX_TEST_BINARY:-$KMUX}

LOCAL_SOCKET="$HOMEDIR/local-tmux.sock"
REMOTE_SOCKET="$HOMEDIR/remote-tmux.sock"
LOCAL_SESSION=outer
REMOTE_SESSION=inner
KEY="$HOMEDIR/ssh-key"
HOST_KEY="$HOMEDIR/host-key"
SSHD_CONFIG="$HOMEDIR/sshd_config"
SSHD=$(command -v sshd)
CONNECT="$HOMEDIR/connect.sh"
kmux_test_register_tmux_socket "$LOCAL_SOCKET"
kmux_test_register_tmux_socket "$REMOTE_SOCKET"

ssh-keygen -q -t ed25519 -N '' -f "$KEY"
ssh-keygen -q -t ed25519 -N '' -f "$HOST_KEY"
cp "$KEY.pub" "$HOMEDIR/authorized_keys"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
cat >"$SSHD_CONFIG" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $HOST_KEY
AuthorizedKeysFile $HOMEDIR/authorized_keys
PidFile $HOMEDIR/sshd.pid
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
UsePAM no
AllowUsers $(id -un)
PermitRootLogin yes
EOF
"$SSHD" -t -f "$SSHD_CONFIG" || kmux_test_infra 'temporary sshd configuration is invalid'
"$SSHD" -D -f "$SSHD_CONFIG" -E "$LOGDIR/sshd.log" &
kmux_test_register_pid "$!"

ssh_client() {
    ssh -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -i "$KEY" -p "$PORT" "$(id -un)@127.0.0.1" "$@"
}
ssh_ready() { ssh_client true 2>/dev/null; }
kmux_test_wait_until 10 'local SSH server' ssh_ready || {
    kmux_test_dump_log "$LOGDIR/sshd.log" 50
    kmux_test_infra 'could not authenticate to local SSH server'
}

tmux -S "$REMOTE_SOCKET" -f /dev/null new-session -d -s "$REMOTE_SESSION" -x 100 -y 30
tmux -S "$REMOTE_SOCKET" bind-key -T prefix x set-option -g @prefix-probe seen

cat >"$CONNECT" <<EOF
#!/usr/bin/env bash
exec ssh -tt -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes \\
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \\
    -o LogLevel=ERROR -i '$KEY' -p '$PORT' '$(id -un)@127.0.0.1' \\
    'exec tmux -S "$REMOTE_SOCKET" attach-session -t "$REMOTE_SESSION"'
EOF
chmod +x "$CONNECT"

tmux -S "$LOCAL_SOCKET" -f /dev/null new-session -d -s "$LOCAL_SESSION" -x 100 -y 30 "exec '$CONNECT'"
kmux_test_start "$LOGDIR/kmux.log" -S "$LOCAL_SOCKET" -s "$LOCAL_SESSION"
KMUX_PID=$KMUX_TEST_PID
if [[ ${KMUX_TEST_WAYLAND:-0} != 1 ]]; then
    kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 20 || kmux_test_fail
    WIN=$KMUX_TEST_WINDOW_ID
fi

remote_client_ready() {
    [[ -n $(tmux -S "$REMOTE_SOCKET" list-clients -t "$REMOTE_SESSION" -F '#{client_pid}' 2>/dev/null) ]]
}
kmux_test_wait_until 15 'remote tmux attached through SSH' remote_client_ready || {
    kmux_test_dump_tmux "$LOCAL_SOCKET" "$LOCAL_SESSION"
    kmux_test_dump_log "$LOGDIR/sshd.log" 50
    kmux_test_fail 'remote tmux did not attach'
}

send_key() {
    if [[ ${KMUX_TEST_WAYLAND:-0} == 1 ]]; then
        case "$1" in
            ctrl+b) wtype -M ctrl -k b -m ctrl ;;
            Escape) wtype -k Escape ;;
            *) wtype "$1" ;;
        esac
    else
        xdotool key --clearmodifiers "$1"
    fi
}
send_held_double_prefix() {
    if [[ ${KMUX_TEST_WAYLAND:-0} == 1 ]]; then
        wtype -M ctrl -k b -k b -m ctrl
    else
        xdotool keydown ctrl
        xdotool key b
        xdotool key b
        xdotool keyup ctrl
    fi
}
if [[ ${KMUX_TEST_WAYLAND:-0} != 1 ]]; then
    xdotool windowfocus --sync "$WIN"
fi
sleep 0.5
send_key x
if [[ -n $(tmux -S "$REMOTE_SOCKET" show-option -gqv @prefix-probe) ]]; then
    kmux_test_fail 'remote prefix binding fired without a prefix'
fi

remote_prefix_seen() {
    [[ $(tmux -S "$REMOTE_SOCKET" show-option -gqv @prefix-probe) == seen ]]
}

# Include consecutive key events, which exercise the palette's focus handoff.
for delay in 0.15 0 0 0 0; do
    tmux -S "$REMOTE_SOCKET" set-option -gu @prefix-probe
    send_key Escape
    send_key ctrl+b
    [[ "$delay" == 0 ]] || sleep "$delay"
    send_key ctrl+b
    sleep 0.15
    send_key x
    if ! kmux_test_wait_until 5 'remote tmux prefix binding' remote_prefix_seen; then
        kmux_test_dump_tmux "$LOCAL_SOCKET" "$LOCAL_SESSION"
        kmux_test_dump_tmux "$REMOTE_SOCKET" "$REMOTE_SESSION"
        tmux -S "$REMOTE_SOCKET" list-clients -t "$REMOTE_SESSION" -F 'key-table=#{client_key_table} prefix=#{client_prefix}' >&2
        tmux -S "$REMOTE_SOCKET" capture-pane -pt "$REMOTE_SESSION" >&2
        kmux_test_dump_log "$LOGDIR/kmux.log" 70
        kmux_test_fail "C-b C-b did not reach remote tmux with ${delay}s between presses"
    fi
done

tmux -S "$REMOTE_SOCKET" set-option -gu @prefix-probe
send_key Escape
send_held_double_prefix
sleep 0.15
send_key x
if ! kmux_test_wait_until 5 'remote prefix with Ctrl held between B presses' remote_prefix_seen; then
    kmux_test_dump_log "$LOGDIR/kmux.log" 70
    kmux_test_fail 'holding Ctrl across both B presses left the remote prefix unsent'
fi

# Exercise tmux's default prefix+c binding with no pause between the keys.
for expected_window_count in {2..11}; do
    send_key ctrl+b
    send_key ctrl+b
    send_key c
    remote_window_created() {
        [[ $(tmux -S "$REMOTE_SOCKET" list-windows -t "$REMOTE_SESSION" -F '#{window_index}' | wc -l) == "$expected_window_count" ]]
    }
    if ! kmux_test_wait_until 5 'remote tmux prefix+c window' remote_window_created; then
        kmux_test_dump_tmux "$REMOTE_SOCKET" "$REMOTE_SESSION"
        tmux -S "$REMOTE_SOCKET" list-clients -t "$REMOTE_SESSION" -F 'key-table=#{client_key_table} prefix=#{client_prefix}' >&2
        tmux -S "$REMOTE_SOCKET" capture-pane -pt "$REMOTE_SESSION" >&2
        kmux_test_dump_log "$LOGDIR/kmux.log" 80
        kmux_test_fail "C-b C-b c did not create remote tmux window $expected_window_count"
    fi
done

kmux_test_pass 'C-b C-b passed through local tmux and SSH to remote tmux on all trials'
