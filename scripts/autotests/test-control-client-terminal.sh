#!/usr/bin/env bash
# End-to-end regression: kmux must advertise an xterm-compatible terminal for
# its tmux control client even when the launcher and an SSH-like transport do
# not provide TERM. Otherwise tmux reports client_termname=dumb/unknown and
# terminal-aware pane applications can reject a fully capable kmux window.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

SOCKET="$HOMEDIR/tmux.sock"
SESSION="control-client-terminal"
WRAPPER="$HOMEDIR/rsh-without-term.sh"
KMUX_PID=""

cat >"$WRAPPER" <<'WRAPPER_EOF'
#!/usr/bin/env bash
unset TERM
exec "$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

cleanup_tmux() {
    local rc=$1
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

echo "=== launching kmux through an SSH-like wrapper without TERM ==="
TERM=dumb "$KMUX" --rsh "$WRAPPER" -S "$SOCKET" -s "$SESSION" >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

client_term=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        tail -80 "$LOGDIR/kmux.log" >&2 2>/dev/null || true
        kmux_test_bail 2 "kmux exited before its control client connected"
    fi
    client_term=$(tmux -S "$SOCKET" list-clients -t "$SESSION" -F '#{client_termname}' 2>/dev/null || true)
    [[ -n "$client_term" ]] && break
    sleep 0.1
done

if [[ "$client_term" != "xterm-256color" ]]; then
    echo "FAIL: kmux control client advertised client_termname='$client_term'" >&2
    tmux -S "$SOCKET" list-clients -t "$SESSION" \
        -F 'client=#{client_name} flags=#{client_flags} tty=#{client_tty} termname=#{client_termname}' >&2 2>&1 || true
    exit 1
fi

echo "PASS: kmux control client advertised client_termname=$client_term without inherited TERM"
