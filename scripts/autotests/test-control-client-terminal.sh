#!/usr/bin/env bash
# End-to-end regression: kmux must advertise an xterm-compatible terminal for
# its tmux control client even when the launcher and an SSH-like transport do
# not provide TERM. Otherwise tmux reports client_termname=dumb/unknown and
# terminal-aware pane applications can reject a fully capable kmux window.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

kmux_test_setup --require tmux

SOCKET="$HOMEDIR/tmux.sock"
SESSION="control-client-terminal"
WRAPPER="$HOMEDIR/rsh-without-term.sh"
KMUX_PID=""
kmux_test_register_tmux_socket "$SOCKET"

cat >"$WRAPPER" <<'WRAPPER_EOF'
#!/usr/bin/env bash
unset TERM
exec "$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

echo "=== launching kmux through an SSH-like wrapper without TERM ==="
TERM=dumb kmux_test_start "$LOGDIR/kmux.log" --rsh "$WRAPPER" -S "$SOCKET" -s "$SESSION"
KMUX_PID=$KMUX_TEST_PID

client_term=""
control_client_connected() {
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        return 1
    fi
    client_term=$(tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_termname}' 2>/dev/null || true)
    [[ -n "$client_term" ]]
}
if ! kmux_test_wait_until 10 "kmux control client to connect" control_client_connected; then
    kmux_test_dump_log "$LOGDIR/kmux.log"
    exit 1
fi

if [[ "$client_term" != "xterm-256color" ]]; then
    echo "FAIL: kmux control client advertised client_termname='$client_term'" >&2
    tmux -S "$SOCKET" list-clients -t "$SESSION" \
        -F 'client=#{client_name} flags=#{client_flags} tty=#{client_tty} termname=#{client_termname}' >&2 2>&1 || true
    exit 1
fi

echo "PASS: kmux control client advertised client_termname=$client_term without inherited TERM"
