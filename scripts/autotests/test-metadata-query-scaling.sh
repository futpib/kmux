#!/usr/bin/env bash
# Observe the real control stream: background metadata must stay within the
# attached session and each workspace snapshot must send a window layout once.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.bridge.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
kmux_test_setup --require tmux --require python3

SOCKET="$HOMEDIR/tmux.sock"
kmux_test_register_tmux_socket "$SOCKET"
tmux -S "$SOCKET" -f /dev/null new-session -d -s scale -x 240 -y 80 'sleep 120'
tmux -S "$SOCKET" set-option -g automatic-rename off
for _ in {1..7}; do
    tmux -S "$SOCKET" split-window -d -h -t scale:0.0 -l 2 'sleep 120'
done
tmux -S "$SOCKET" select-layout -t scale tiled
tmux -S "$SOCKET" new-session -d -s unrelated 'sleep 120'
tmux -S "$SOCKET" select-pane -t unrelated -T KMUX_UNRELATED_METADATA_SENTINEL

kmux_test_start "$LOGDIR/kmux.log" -S "$SOCKET" -s scale --qwindowgeometry 1200x800

# Observe snapshots after external workspace changes. Group rows by the
# protocol's record boundary, not wall-clock sleeps.
cat > "$HOMEDIR/check.py" <<'PY'
import pathlib
import re
import subprocess
import sys

socket, logfile, mode, minimum = sys.argv[1:]
result = subprocess.run(
    ["tmux", "-S", socket, "display-message", "-p", "-t", "scale", "#{window_layout}"],
    capture_output=True, text=True,
)
layout = result.stdout.strip()
if result.returncode or not layout:
    sys.exit(1)
text = pathlib.Path(logfile).read_text(errors="replace")
groups = {}
for line in text.splitlines():
    if "konsole.tmux.bridge: << " not in line:
        continue
    boundary = re.search(r"__KMUX_RS_[0-9a-f]{8}__", line)
    if boundary:
        groups.setdefault(boundary[0], []).append(line)
snapshots = [rows for rows in groups.values() if len(rows) == 8 and any(layout in row for row in rows)]
if mode == "count":
    print(len(snapshots))
    sys.exit(0)
if not any(f"metadata-update-{minimum}" in row for rows in snapshots for row in rows):
    sys.exit(1)
if mode == "ready":
    sys.exit(0)

failures = []
if "KMUX_UNRELATED_METADATA_SENTINEL" in text:
    failures.append("metadata from an unrelated session was sent to kmux")
copies = sum(row.count(layout) for row in snapshots[-1])
print(f"Latest 8-pane snapshot: {copies} copies of its window layout")
if copies != 1:
    failures.append(f"one window layout was transmitted {copies} times in a single snapshot")
for failure in failures:
    print("FAIL:", failure)
sys.exit(bool(failures))
PY

# shellcheck disable=SC2329
metadata_ready() {
    kill -0 "$KMUX_TEST_PID" 2>/dev/null && python3 "$HOMEDIR/check.py" "$SOCKET" "$LOGDIR/kmux.log" ready "$snapshot_count"
}
for snapshot_count in 1 2 3; do
    tmux -S "$SOCKET" rename-window -t scale "metadata-update-$snapshot_count"
    if ! kmux_test_wait_until 30 "workspace snapshot $snapshot_count" metadata_ready; then
        kmux_test_dump_log "$LOGDIR/kmux.log"
        kmux_test_fail "workspace snapshots did not arrive"
    fi
done
python3 "$HOMEDIR/check.py" "$SOCKET" "$LOGDIR/kmux.log" check 3 || kmux_test_fail "redundant metadata traffic"
# Allow the final layout/rename notifications to settle, then span two polls.
sleep 1
before=$(python3 "$HOMEDIR/check.py" "$SOCKET" "$LOGDIR/kmux.log" count 0)
sleep 4.3
after=$(python3 "$HOMEDIR/check.py" "$SOCKET" "$LOGDIR/kmux.log" count 0)
[[ "$before" == "$after" ]] || kmux_test_fail "idle workspace sent $((after - before)) redundant snapshots"
kmux_test_pass "metadata is session-scoped and window layouts are not duplicated per pane"
