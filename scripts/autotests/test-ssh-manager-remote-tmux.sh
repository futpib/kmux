#!/usr/bin/env bash
# End-to-end test for SSH Manager's kmux-native remote connection action.
#
# The test starts the real kmux binary, loads a manual host from kmux's own
# kmuxsshconfig, selects it through the SSH Manager quick-access UI, and lets a
# fake ssh executable run the appended tmux command locally. It verifies the
# user-visible second window, the remote tmux session, structured destination
# arguments, and GUI askpass environment.
# Predicate helpers are invoked indirectly by kmux_test_wait_until.
# shellcheck disable=SC2329

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

kmux_test_setup --wm --require tmux

LOCAL_SOCKET="$HOMEDIR/local-tmux.sock"
REMOTE_SOCKET="$HOMEDIR/remote-tmux.sock"
REMOTE_SESSION="ssh-manager-e2e"
TRANSPORT_LOG="$LOGDIR/ssh-transport.log"
export KMUX_TEST_TRANSPORT_LOG="$TRANSPORT_LOG"
kmux_test_register_tmux_socket "$LOCAL_SOCKET"
kmux_test_register_tmux_socket "$REMOTE_SOCKET"

mkdir -p "$HOMEDIR/bin"
cat >"$HOMEDIR/bin/ssh" <<'WRAPPER_EOF'
#!/usr/bin/env bash
set -euo pipefail

destination=$1
shift
{
    printf 'destination=%s\n' "$destination"
    printf 'askpass=%s\n' "${SSH_ASKPASS:-}"
    printf 'askpass_require=%s\n' "${SSH_ASKPASS_REQUIRE:-}"
    printf 'command='
    printf '%q ' "$@"
    printf '\n'
} >"$KMUX_TEST_TRANSPORT_LOG"

exec "$@"
WRAPPER_EOF
chmod +x "$HOMEDIR/bin/ssh"
export PATH="$HOMEDIR/bin:$PATH"

# This entry exists only in kmuxsshconfig. A decoy Konsole entry makes an
# accidental fallback to the old namespace fail to select the requested host.
cat >"$XDG_CONFIG_HOME/kmuxsshconfig" <<KMUX_CONFIG_EOF
[Remote hosts][E2E Remote]
hostname=remote.invalid
identifier=E2E Remote
importedFromSshConfig=false
port=
profileName=
remoteWorkingDirectory=$HOMEDIR
sshkey=
tmuxPath=
tmuxSession=$REMOTE_SESSION
tmuxSocketName=
tmuxSocketPath=$REMOTE_SOCKET
useSshConfig=false
username=tester
KMUX_CONFIG_EOF

cat >"$XDG_CONFIG_HOME/konsolesshconfig" <<'KONSOLE_CONFIG_EOF'
[Legacy][Konsole Decoy]
hostname=legacy.invalid
identifier=Konsole Decoy
useSshConfig=false
username=legacy
KONSOLE_CONFIG_EOF

kmux_test_start "$LOGDIR/kmux.log" -S "$LOCAL_SOCKET" -s local-e2e
KMUX_PID=$KMUX_TEST_PID
if ! kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 20; then
    kmux_test_fail
fi
INITIAL_WINDOW=$KMUX_TEST_WINDOW_ID

if ! xdotool windowactivate --sync "$INITIAL_WINDOW" 2>/dev/null; then
    xdotool windowfocus --sync "$INITIAL_WINDOW"
fi
sleep 1
xdotool key --window "$INITIAL_WINDOW" ctrl+alt+h
sleep 1
xdotool type --delay 20 "E2E Remote"
xdotool key Return

remote_connection_ready() {
    [[ -s "$TRANSPORT_LOG" ]] && tmux -S "$REMOTE_SOCKET" has-session -t "$REMOTE_SESSION" 2>/dev/null
}
if ! kmux_test_wait_until 20 "SSH Manager remote tmux connection" remote_connection_ready; then
    kmux_test_dump_log "$LOGDIR/kmux.log" 120
    kmux_test_dump_log "$TRANSPORT_LOG" 40
    kmux_test_fail
fi

second_window_visible() {
    local windows
    windows=$(xdotool search --onlyvisible --class kmux 2>/dev/null || true)
    [[ $(wc -l <<<"$windows") -ge 2 ]]
}
if ! kmux_test_wait_until 10 "second kmux window" second_window_visible; then
    kmux_test_dump_log "$LOGDIR/kmux.log" 120
    kmux_test_fail
fi

grep -qxF 'destination=tester@remote.invalid' "$TRANSPORT_LOG" \
    || kmux_test_fail "SSH Manager did not pass the structured manual destination"
grep -qxF 'askpass_require=force' "$TRANSPORT_LOG" \
    || kmux_test_fail "SSH Manager transport did not force GUI askpass"

askpass_path=$(sed -n 's/^askpass=//p' "$TRANSPORT_LOG")
if [[ -z "$askpass_path" || ! -x "$askpass_path" ]]; then
    kmux_test_dump_log "$TRANSPORT_LOG" 40
    kmux_test_fail "SSH_ASKPASS does not name an executable helper"
fi

kmux_test_pass "SSH Manager opened kmuxsshconfig host as a remote tmux window"
