#!/usr/bin/env bash
# Poll predicates are passed by name and outcome helpers accept optional messages.
# shellcheck disable=SC2119,SC2329
# Exercise real QApplication argument handling, not just QCommandLineParser:
# Qt consumes --session before kmux can parse it and enters desktop restore.
# Both supported spellings must reach tmux and display the selected session.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

kmux_test_setup --x11 --require tmux

"$KMUX" --help >"$LOGDIR/help.log" 2>&1
if ! grep -q -- '-s, --tmux-session <name>' "$LOGDIR/help.log" ||
    grep -q -- '-s, --session ' "$LOGDIR/help.log"; then
    kmux_test_dump_log "$LOGDIR/help.log"
    kmux_test_fail "help must advertise --tmux-session, not Qt's --session"
fi

selected_session_visible() {
    kill -0 "$KMUX_PID" 2>/dev/null &&
        tmux -S "$SOCKET" list-clients -F '#{session_name}' 2>/dev/null | grep -qxF "$SESSION" &&
        xdotool search --onlyvisible --pid "$KMUX_PID" --class kmux >/dev/null 2>&1
}

case_number=0
for transport in local rsh; do
    for mode in create attach; do
        for spelling in short long equals; do
            case_number=$((case_number + 1))
            SOCKET="$HOMEDIR/session-$case_number.sock"
            SESSION="target-$case_number"
            kmux_test_register_tmux_socket "$SOCKET"
            # A different existing session catches accidentally dropped names.
            tmux -f /dev/null -S "$SOCKET" new-session -d -s decoy
            pane_before=""
            if [[ "$mode" == attach ]]; then
                tmux -S "$SOCKET" new-session -d -s "$SESSION"
                pane_before=$(tmux -S "$SOCKET" list-panes -t "$SESSION" -F '#{pane_id}')
            fi
            args=(--socket "$SOCKET")
            if [[ "$transport" == rsh ]]; then
                args+=(--rsh env)
            fi
            case "$spelling" in
                short) args+=(-s "$SESSION") ;;
                long) args+=(--tmux-session "$SESSION") ;;
                equals) args+=(--tmux-session="$SESSION") ;;
            esac
            logfile="$LOGDIR/$transport-$mode-$spelling.log"
            kmux_test_start "$logfile" "${args[@]}"
            KMUX_PID=$KMUX_TEST_PID
            if ! kmux_test_wait_until 10 "$transport/$mode/$spelling to show the selected session" selected_session_visible; then
                kmux_test_dump_log "$logfile"
                kmux_test_fail
            fi
            sessions=$(tmux -S "$SOCKET" list-sessions -F '#{session_name}' | sort)
            [[ "$sessions" == $'decoy\n'"$SESSION" ]] || kmux_test_fail "unexpected session created: $sessions"
            if [[ "$mode" == attach ]]; then
                pane_after=$(tmux -S "$SOCKET" list-panes -t "$SESSION" -F '#{pane_id}')
                [[ "$pane_before" == "$pane_after" ]] || kmux_test_fail "existing pane was replaced"
            fi
            # Closing the isolated server lets kmux exit without touching real sessions.
            tmux -S "$SOCKET" kill-server
            kmux_test_wait_process_exit "$KMUX_PID" 10 || kmux_test_fail "kmux did not exit after its test server closed"
            echo "PASS: $transport/$mode/$spelling"
        done
    done
done

kmux_test_pass "help and all 12 tmux session selection cases"
