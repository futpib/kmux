#!/usr/bin/env bash
# Regression gate for the suspend-freeze fix.
#
# Bug (see git history): when the machine running kmux suspends while attached to
# a tmux server (esp. over --rsh), kmux stops reading the control stream. With no
# control-mode flow control the server either buffers every pane's output in RAM
# without bound (ballooning toward OOM, wedging the whole server) or stops
# reading a pane's pty — so the pane's app, and the session, freeze.
#
# Fix: kmux enables tmux's pause-after flow control at attach
# (TmuxController::enableFlowControl). tmux then autonomously pauses a pane that
# falls more than pause-after seconds behind — even while kmux is frozen — so the
# server's memory stays bounded and it keeps serving its other clients/sessions.
# (The viewed pane still pauses while suspended; that's inherent to control mode
# — but kmux resumes and resyncs it on wake, see TmuxController::onPanePaused.)
#
# Two checks:
#   1. kmux's control client carries the pause-after flag (the fix is wired).
#   2. With kmux SIGSTOPped (== suspend) and a pane flooding, the server's RSS
#      stays bounded instead of ballooning.
#
# To see it catch a regression, run with KMUX_PAUSE_AFTER=0 (disables flow
# control ≈ pre-fix): the flag is absent and the test fails.
#
# Exit 0 = fixed.  Exit 1 = bug present.  Exit 2 = scaffolding failure.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.bridge.debug=true;konsole.tmux.controller.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb
# Pause a pane after just 1s behind so the memory bound is small and the test is
# fast. KMUX_PAUSE_AFTER=0 disables flow control, reproducing the pre-fix bug.
export KMUX_PAUSE_AFTER="${KMUX_PAUSE_AFTER:-1}"

kmux_test_setup
command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

SOCKET="$HOMEDIR/tmux.sock"
SESSION="demo"
GROWTH_LIMIT_KB=$((80 * 1024))

rss_kb() { ps -o rss= -p "$1" 2>/dev/null | tr -d ' '; }

echo "=== pre-creating tmux server + session ==="
tmux -S "$SOCKET" -f /dev/null start-server 2>/dev/null || true
tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 200 -y 50 || kmux_test_bail 2 "could not create tmux session"

echo "=== launching kmux (KMUX_PAUSE_AFTER=$KMUX_PAUSE_AFTER) ==="
"$KMUX" -S "$SOCKET" -s "$SESSION" >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

cleanup_all() {
    [[ -n "${KMUX_PID:-}" ]] && kill -CONT "$KMUX_PID" 2>/dev/null
    if [[ -n "${KMUX_PID:-}" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    [[ -e "$SOCKET" ]] && tmux -S "$SOCKET" kill-server 2>/dev/null || true
}
trap cleanup_all EXIT

# Wait until kmux has attached as a control-mode client.
attached=0
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        echo "FAIL: kmux exited before attaching (see $LOGDIR/kmux.log)" >&2; exit 2
    fi
    if tmux -S "$SOCKET" list-clients -F '#{client_flags}' 2>/dev/null | grep -q control-mode; then attached=1; break; fi
    sleep 0.2
done
(( attached == 1 )) || { echo "FAIL: kmux never attached (see $LOGDIR/kmux.log)" >&2; exit 2; }

# --- check 1: did kmux enable control-mode flow control? --------------------
# tmux exposes the client flags set via `refresh-client -f`; pause-after appears
# there once kmux has asked for it.
flags=$(tmux -S "$SOCKET" list-clients -F '#{client_flags}' 2>/dev/null | grep control-mode | head -1)
echo "kmux client_flags: $flags"
if ! echo "$flags" | grep -q "pause-after"; then
    echo "FAIL: kmux did not enable pause-after flow control (client has no pause-after flag)."
    exit 1
fi
echo "OK: kmux enabled pause-after flow control."

# --- check 2: server memory stays bounded while kmux is suspended -----------
SRV=$(tmux -S "$SOCKET" display-message -p '#{pid}')
[[ -n "$SRV" ]] || kmux_test_bail 2 "could not read tmux server pid"
tmux -S "$SOCKET" send-keys -t "$SESSION" \
    'while :; do echo PANE_OUTPUT_PADDING_PADDING_PADDING_PADDING_PADDING_PADDING; done' Enter
sleep 0.5
base=$(rss_kb "$SRV")
echo "=== SIGSTOP kmux (simulates suspend); server pid=$SRV baseline RSS=${base}KB ==="
kill -STOP "$KMUX_PID" || kmux_test_bail 2 "could not SIGSTOP kmux"
sleep 0.3
state=$(ps -o state= -p "$KMUX_PID" 2>/dev/null | tr -d ' ')
[[ "$state" == T* ]] || kmux_test_bail 2 "kmux not stopped after SIGSTOP (state='$state')"

ballooned=0
for i in $(seq 1 8); do
    sleep 1
    rss=$(rss_kb "$SRV"); grow=$(( ${rss:-0} - base ))
    printf '  t=%2ds  serverRSS=%8sKB (%+dKB)\n' "$i" "${rss:-?}" "$grow"
    if (( grow > GROWTH_LIMIT_KB )); then ballooned=1; break; fi
done

echo
if (( ballooned )); then
    echo "FAIL: server RSS grew by $(( ($(rss_kb "$SRV") - base) / 1024 ))MB while kmux was suspended — flow control not bounding the buffer."
    exit 1
fi
echo "PASS: kmux enabled flow control and the server stayed bounded while suspended."
exit 0
