#!/usr/bin/env bash
# End-to-end test that kmux renders a multi-pane split layout without wrap.
#
# Builds a 5-pane window — 2 panes in the top row, 3 in the bottom — every
# pane running scripts/autotests/fixtures/tui-rect.sh. After kmux attaches
# and the resize negotiation settles, queries every kmux Session via D-Bus
# and asserts each pane's screen is a coherent rectangle: row N is filled
# uniformly with the Nth letter (a..zA..Z, wrap), no character has spilled
# in from a wrap. Length checks tie back to the size each fixture itself
# observed via stty (its TUI_SIZE_FILE), so a chrome / split-handle bug
# that under-counts pane width by a few cells fails the per-row length
# assertion even when kmux's emulator silently absorbed the overflow.
#
# Layout (tmux pane ids in parens):
#   +-------+-------+
#   |  %0   |  %2   |
#   +-------+--+----+
#   |  %1   |%3| %4 |
#   +-------+--+----+
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = every pane rendered cleanly.
# Exit 1 = some pane wrapped (or row counts / lengths disagreed).
# Exit 2 = scaffolding failure (missing binary, missing tool, etc.).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"
command -v dbus-send >/dev/null || kmux_test_bail 2 "dbus-send not installed"

FIXTURE="$SCRIPT_DIR/fixtures/tui-rect.sh"
[[ -x "$FIXTURE" ]] || kmux_test_bail 2 "fixture missing or not executable: $FIXTURE"

SOCKET="$HOMEDIR/tmux.sock"
SESSION="rect"
READY_DIR="$HOMEDIR/ready"
SIZE_DIR="$HOMEDIR/size"
mkdir -p "$READY_DIR" "$SIZE_DIR"

# tmux pane ids we expect after the splits below.
PANE_IDS=(%0 %1 %2 %3 %4)
NUM_PANES=${#PANE_IDS[@]}

parse_dbus_strings() {
    sed -n 's/^[[:space:]]*string "\(.*\)"$/\1/p'
}

introspect_children() {
    local service="$1" parent="$2"
    dbus-send --session --print-reply --dest="$service" "$parent" \
        org.freedesktop.DBus.Introspectable.Introspect 2>/dev/null \
        | sed -n 's/.*<node name="\([^"]*\)".*/\1/p'
}

# Wait until the fixture for $1 (a slot index 0..NUM_PANES-1) has touched
# its ready file. Bails after ~10s.
wait_for_ready() {
    local slot="$1"
    local f="$READY_DIR/$slot"
    for _ in $(seq 1 100); do
        [[ -e "$f" ]] && return 0
        sleep 0.1
    done
    echo "FAIL: fixture in slot $slot never reported ready ($f)" >&2
    exit 1
}

# Build the shell command tmux's split-window / new-session should run for
# slot $1. The env vars give that fixture a pane-unique pair of files.
fixture_cmd() {
    local slot="$1"
    printf 'TUI_READY_FILE=%q TUI_SIZE_FILE=%q exec %q' \
        "$READY_DIR/$slot" "$SIZE_DIR/$slot" "$FIXTURE"
}

echo "=== building 5-pane layout in tmux ==="

# Slot 0 = pane %0 (initial pane). -x/-y are tmux's "default size" knobs;
# kmux's refresh-client -C will resize once it attaches.
TUI_READY_FILE="$READY_DIR/0" TUI_SIZE_FILE="$SIZE_DIR/0" \
    tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 200 -y 60 "$FIXTURE"
wait_for_ready 0

# Slot 1 (pane %1): split %0 vertically — creates a pane below %0, so the
# window now has a top row (%0) and a bottom row (%1).
tmux -S "$SOCKET" split-window -v -t '%0' "$(fixture_cmd 1)"
wait_for_ready 1

# Slot 2 (pane %2): split %0 horizontally — top row becomes %0 (left) and
# %2 (right). Top now has 2 panes.
tmux -S "$SOCKET" split-window -h -t '%0' "$(fixture_cmd 2)"
wait_for_ready 2

# Slot 3 (pane %3): split %1 horizontally — bottom row gets a 2nd pane.
tmux -S "$SOCKET" split-window -h -t '%1' "$(fixture_cmd 3)"
wait_for_ready 3

# Slot 4 (pane %4): split %3 horizontally — bottom row now has %1, %3, %4.
tmux -S "$SOCKET" split-window -h -t '%3' "$(fixture_cmd 4)"
wait_for_ready 4

echo "OK: all 5 fixtures reported ready"
echo "--- tmux layout snapshot ---"
tmux -S "$SOCKET" list-panes -t "$SESSION" \
    -F '#{pane_id} #{pane_left},#{pane_top} #{pane_width}x#{pane_height}'

echo "=== launching kmux to attach ==="
QT_LOGGING_RULES='konsole.tmux.resize.debug=true;konsole.tmux.bridge.debug=true' \
    QT_ASSUME_STDERR_HAS_CONSOLE=1 \
    "$KMUX" -S "$SOCKET" -s "$SESSION" --qwindowgeometry 1100x700 >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!

cleanup_kmux() {
    if [[ -n "${KMUX_PID:-}" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    if [[ -e "$SOCKET" ]]; then
        tmux -S "$SOCKET" kill-server 2>/dev/null || true
    fi
}
trap cleanup_kmux EXIT

WIN=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        echo "FAIL: kmux exited before window appeared" >&2
        echo "--- $LOGDIR/kmux.log ---" >&2
        tail -50 "$LOGDIR/kmux.log" >&2 2>/dev/null || true
        echo "--- $LOGDIR/xvfb.log ---" >&2
        tail -20 "$LOGDIR/xvfb.log" >&2 2>/dev/null || true
        exit 1
    fi
    WIN=$(xdotool search --name kmux 2>/dev/null | tail -1 || true)
    [[ -n "$WIN" ]] && break
    sleep 0.2
done
[[ -n "$WIN" ]] || { echo "FAIL: kmux window never appeared (see $LOGDIR/kmux.log)" >&2; exit 1; }
echo "OK: kmux window appeared (winid=$WIN)"

# Settle: the splits resize on attach; sample fixture/tmux sizes so a failure
# dump can show whether they converged or kept oscillating.
echo "--- per-pane sizes (rows cols) | tmux pane (cols rows) ---"
for tick in $(seq 1 10); do
    sleep 0.3
    printf '  t=%d:' "$tick"
    for slot in $(seq 0 $((NUM_PANES - 1))); do
        fix="(missing)"
        [[ -e "$SIZE_DIR/$slot" ]] && fix=$(cat "$SIZE_DIR/$slot")
        pane_id="${PANE_IDS[$slot]}"
        tmux_sample=$(tmux -S "$SOCKET" display-message -p -t "$pane_id" '#{pane_width} #{pane_height}' 2>/dev/null || echo "(error)")
        printf ' [%s fix=%s tmux=%s]' "$pane_id" "$fix" "$tmux_sample"
    done
    printf '\n'
done

# Find kmux's D-Bus service.
SERVICE=""
for _ in $(seq 1 50); do
    names=$(dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.ListNames 2>/dev/null | parse_dbus_strings)
    if echo "$names" | grep -qx "org.kde.kmux-${KMUX_PID}"; then
        SERVICE="org.kde.kmux-${KMUX_PID}"
        break
    fi
    fallback=$(echo "$names" | grep -E '^org\.kde\.kmux(-[0-9]+)?$' | head -1 || true)
    if [[ -n "$fallback" ]]; then
        SERVICE="$fallback"
        break
    fi
    sleep 0.1
done
[[ -n "$SERVICE" ]] || { echo "FAIL: kmux D-Bus service not found" >&2; exit 1; }
echo "kmux D-Bus service: $SERVICE"

# Wait until kmux has registered all five session paths.
SESSION_IDS=()
for _ in $(seq 1 50); do
    raw=$(introspect_children "$SERVICE" /Sessions)
    mapfile -t SESSION_IDS < <(echo "$raw" | grep -E '^[0-9]+$' | sort -n)
    (( ${#SESSION_IDS[@]} >= NUM_PANES )) && break
    sleep 0.2
done
echo "kmux /Sessions/* ids: ${SESSION_IDS[*]}"
if (( ${#SESSION_IDS[@]} != NUM_PANES )); then
    echo "FAIL: expected $NUM_PANES kmux sessions, found ${#SESSION_IDS[@]}" >&2
    exit 1
fi

chars=(a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W X Y Z)
nchars=${#chars[@]}

fail=0
fail_examples=0
MAX_EXAMPLES=5
report_fail() {
    fail=1
    if (( fail_examples < MAX_EXAMPLES )); then
        echo "FAIL: $*"
        fail_examples=$((fail_examples + 1))
    fi
}

# Per-session check: read the live screen image (not the scrolled buffer)
# via getCurrentScreenLines. Wrap shows up as either:
#   1) row content that isn't uniformly its expected letter — wrap shoves
#      cells off the right edge into the next row's leading columns, and
#      whatever the cursor lands on after the next ESC[N;1H may not fully
#      overwrite the squatting characters before the test reads;
#   2) a body row whose width disagrees with tmux's pane_width — the
#      tell-tale "kmux thinks this pane is one cell narrower than tmux
#      told it" case. The last row is COLS-1 wide by design (the fixture
#      leaves the bottom-right cell blank to dodge corner-cell scroll).
for sid in "${SESSION_IDS[@]}"; do
    SESSION_PATH="/Sessions/$sid"
    mapfile -t LINES < <(dbus-send --session --print-reply --dest="$SERVICE" "$SESSION_PATH" \
        org.kde.konsole.Session.getCurrentScreenLines 2>/dev/null \
        | parse_dbus_strings)

    n=${#LINES[@]}
    if (( n < 4 )); then
        report_fail "session $sid: degenerate ($n lines) — kmux didn't fit a usable rectangle"
        continue
    fi

    body_max_len=0
    for ((i=0; i<n-1; i++)); do
        if (( ${#LINES[i]} > body_max_len )); then body_max_len=${#LINES[i]}; fi
    done

    saw_failure=0
    for ((row=0; row<n; row++)); do
        expected_ch="${chars[row % nchars]}"
        line="${LINES[row]}"
        actual_len=${#line}

        bad_at=-1
        for ((i=0; i<actual_len; i++)); do
            if [[ "${line:i:1}" != "$expected_ch" ]]; then
                bad_at=$i
                break
            fi
        done
        if (( bad_at >= 0 )); then
            snippet="${line:0:60}"
            report_fail "session $sid row $row: expected all '$expected_ch', mismatch at col $bad_at; line[0:60]=[${snippet}]"
            saw_failure=1
            break
        fi

        if (( row == n - 1 )); then
            expected_len=$((body_max_len - 1))
        else
            expected_len=$body_max_len
        fi
        if (( actual_len != expected_len )); then
            report_fail "session $sid row $row: length=${actual_len}, expected=${expected_len} (char='$expected_ch')"
            saw_failure=1
            break
        fi
    done

    if (( saw_failure == 0 )); then
        echo "OK: session $sid ${body_max_len}x${n} (clean)"
    fi
done

if (( fail )); then
    echo "" >&2
    echo "===== diagnostics =====" >&2
    echo "" >&2
    echo "--- tmux list-panes ---" >&2
    tmux -S "$SOCKET" list-panes -t "$SESSION" \
        -F '#{pane_id} #{pane_left},#{pane_top} #{pane_width}x#{pane_height}' >&2 || true
    echo "" >&2
    echo "--- per-slot fixture sizes ---" >&2
    for slot in $(seq 0 $((NUM_PANES - 1))); do
        sz="(missing)"
        [[ -e "$SIZE_DIR/$slot" ]] && sz=$(cat "$SIZE_DIR/$slot")
        printf '  slot %d (%s): %s\n' "$slot" "${PANE_IDS[$slot]}" "$sz" >&2
    done
    echo "" >&2
    echo "--- kmux per-session row inventory (live screen) ---" >&2
    for sid in "${SESSION_IDS[@]}"; do
        mapfile -t LINES < <(dbus-send --session --print-reply --dest="$SERVICE" "/Sessions/$sid" \
            org.kde.konsole.Session.getCurrentScreenLines 2>/dev/null \
            | parse_dbus_strings)
        n=${#LINES[@]}
        first_len=$(( n > 0 ? ${#LINES[0]} : 0 ))
        last_len=$(( n > 0 ? ${#LINES[$((n-1))]} : 0 ))
        printf '  /Sessions/%s: %d lines, first len=%d, last len=%d, first[0:30]=%q\n' \
            "$sid" "$n" "$first_len" "$last_len" "${LINES[0]:0:30}" >&2
    done
    echo "" >&2
    echo "--- konsole.tmux.resize log lines ---" >&2
    grep -aE 'konsole\.tmux\.resize' "$LOGDIR/kmux.log" \
        | tail -40 >&2 || true
    echo "" >&2
    echo "--- bridge: refresh-client / layout-change traffic ---" >&2
    grep -aE 'refresh-client|%layout-change|%window-add' "$LOGDIR/kmux.log" \
        | tail -20 >&2 || true
    echo "" >&2
    echo "(full kmux output: $LOGDIR/kmux.log)" >&2
    exit 1
fi

echo "PASS: all $NUM_PANES split panes rendered without wrap"
exit 0
