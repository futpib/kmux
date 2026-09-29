#!/usr/bin/env bash
# Drive desktop-launched rsh authentication and the resulting tmux pane through
# real X11 key events. The wrapper reads both stdin and /dev/tty, without a
# launching terminal or an askpass helper.
# shellcheck disable=SC2329

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

kmux_test_setup --x11 --require tmux --require setsid
export LC_ALL=C.UTF-8
unset SSH_ASKPASS SSH_ASKPASS_REQUIRE

SOCKET="$HOMEDIR/tmux.sock"
WRAPPER="$HOMEDIR/rsh-input.sh"
STAGE="$HOMEDIR/stage"
export KMUX_TEST_INPUT_STAGE="$STAGE"
kmux_test_register_tmux_socket "$SOCKET"

cat >"$WRAPPER" <<'WRAPPER_EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'Confirm host (yes/no): '
printf 'confirm\n' >"$KMUX_TEST_INPUT_STAGE"
read -r answer
[[ "$answer" == yes ]]
exec 9<>/dev/tty
stty -echo <&9
printf 'Password: ' >&9
printf 'password\n' >"$KMUX_TEST_INPUT_STAGE"
read -r secret <&9
stty echo <&9
[[ "$secret" == GUI-secret-42 ]]
exec 9>&-
exec "$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

kmux_test_start_process "$LOGDIR/kmux.log" setsid "$KMUX" --rsh "$WRAPPER" -S "$SOCKET"
KMUX_PID=$KMUX_TEST_PID

prompt_visible() {
    PROMPT=$(xdotool search --all --onlyvisible --pid "$KMUX_PID" --name '^Remote Shell — ' 2>/dev/null | head -1 || true)
    [[ -n "$PROMPT" ]]
}
kmux_test_wait_until 10 'remote shell input window' prompt_visible || kmux_test_fail 'no GUI prompt'
[[ ! -S "$SOCKET" ]] || kmux_test_fail 'tmux started before authentication'
xdotool windowfocus --sync "$PROMPT"
xdotool type --window "$PROMPT" --delay 30 yes
xdotool key --window "$PROMPT" Return

password_requested() {
    [[ -f "$STAGE" && $(cat "$STAGE") == password ]]
}
kmux_test_wait_until 5 'password reader' password_requested || kmux_test_fail 'stdin response was not delivered'
xdotool type --window "$PROMPT" --delay 30 GUI-secret-42
xdotool key --window "$PROMPT" Return

authenticated() {
    tmux -S "$SOCKET" has-session 2>/dev/null && ! prompt_visible
}
kmux_test_wait_until 15 'authenticated tmux and closed prompt' authenticated || kmux_test_fail 'authentication handoff failed'
kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 10 || kmux_test_fail 'workspace did not appear'
WINDOW=$KMUX_TEST_WINDOW_ID
xdotool windowfocus --sync "$WINDOW"
xdotool type --window "$WINDOW" --delay 10 "tmux -S '$SOCKET' set-option -g @gui-input yes"
xdotool key --window "$WINDOW" Return

pane_input_delivered() {
    [[ $(tmux -S "$SOCKET" show-options -gv @gui-input 2>/dev/null || true) == yes ]]
}
kmux_test_wait_until 10 'normal pane input after authentication' pane_input_delivered || kmux_test_fail 'tmux input handoff failed'
if grep -qF GUI-secret-42 "$LOGDIR/kmux.log"; then
    kmux_test_fail 'password leaked into diagnostics'
fi
kmux_test_pass 'desktop rsh accepted stdin and hidden tty input, closed its prompt, and delivered pane input'
