#!/usr/bin/env bash
# End-to-end process-boundary test for sessionToolbar visibility persistence.
#
# MainWindowTest drives the real "Toolbars Shown" QAction and verifies that
# hiding the toolbar is saved. This test covers the complementary boundary:
# a fresh kmux process must restore the saved Hidden=true value from disk.
# Keeping the GUI action in C++ avoids brittle xdotool menu mnemonics while
# retaining a real-binary restart check here.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="org.kde.konsole.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
kmux_test_setup --x11 --require tmux --require kwriteconfig6 --require kreadconfig6

SOCKET="$HOMEDIR/tmux.sock"
STATE_FILE="$XDG_STATE_HOME/kmuxstaterc"
kmux_test_register_tmux_socket "$SOCKET"

LAUNCH_PID=""
launch_and_wait_for_window() {
    local logfile=$1
    kmux_test_start "$logfile" -S "$SOCKET"
    LAUNCH_PID=$KMUX_TEST_PID
    kmux_test_wait_visible_window "$LAUNCH_PID" "$logfile" 20 >/dev/null
}

kill_and_wait() {
    local pid=$1
    kill "$pid" 2>/dev/null || true
    if ! kmux_test_wait_process_exit "$pid" 5; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
}

echo "=== run 1: initialize the real kmux state file ==="
launch_and_wait_for_window "$LOGDIR/run1.log"
PID1=$LAUNCH_PID
sleep 1
kill_and_wait "$PID1"

if [[ ! -f "$STATE_FILE" ]]; then
    kmux_test_dump_log "$LOGDIR/run1.log"
    kmux_test_infra "kmux did not create $STATE_FILE"
fi

kwriteconfig6 \
    --file "$STATE_FILE" \
    --group MainWindow \
    --group "Toolbar sessionToolbar" \
    --key Hidden \
    true

saved_hidden=$(
    kreadconfig6 \
        --file "$STATE_FILE" \
        --group MainWindow \
        --group "Toolbar sessionToolbar" \
        --key Hidden \
        --default missing
)
if [[ "$saved_hidden" != "true" ]]; then
    kmux_test_infra "could not seed sessionToolbar Hidden=true (read $saved_hidden)"
fi

echo "=== run 2: restore the saved toolbar visibility ==="
launch_and_wait_for_window "$LOGDIR/run2.log"
PID2=$LAUNCH_PID
sleep 1
kill_and_wait "$PID2"

final_line=$(grep 'activeViewChanged: after  addClient' "$LOGDIR/run2.log" | tail -1 || true)
if [[ -z "$final_line" ]]; then
    kmux_test_dump_log "$LOGDIR/run2.log"
    kmux_test_fail "could not find activeViewChanged state in run 2"
fi

echo "run2 activeViewChanged: $final_line"
if grep -q 'sessionToolbar=hidden' <<<"$final_line"; then
    kmux_test_pass "a fresh kmux process restored sessionToolbar as hidden"
fi

kmux_test_fail "a fresh kmux process showed sessionToolbar despite Hidden=true"
