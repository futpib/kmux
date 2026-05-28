#!/usr/bin/env bash
# End-to-end test that kmux renders a full-screen TUI without line wrapping.
#
# Pre-creates a tmux session whose pane runs scripts/autotests/fixtures/
# tui-rect.sh — a fixture that fills its pane with a rectangle whose Nth
# row is uniformly filled with the Nth letter of the alphabet (a..zA..Z),
# sized to whatever stty(1) reports inside the pane. Launches kmux in
# control mode against that session; once kmux's window is up and the
# resize has settled, queries kmux's Screen via D-Bus
# (org.kde.konsole.Session.getAllDisplayedTextList) and asserts:
#
#   1. kmux returns exactly $rows rows, where $rows is the size the
#      fixture itself observed via stty (its TUI_SIZE_FILE).
#   2. Each row contains ONLY its expected letter — no mixing — proving
#      no character spilled across a row boundary.
#
# A wrap bug (kmux's display columns disagreeing with the size the pty
# reports to the fixture, or any other rendering mismatch) shows up as
# a row's letter bleeding into the next row's content; the per-row
# uniformity check catches it.
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = rectangle rendered without wrap.
# Exit 1 = wrap detected (or row counts disagreed).
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
READY_FILE="$HOMEDIR/tui-ready"
SIZE_FILE="$HOMEDIR/tui-size"

# Strips dbus-send's "method return ..." preamble and pulls each
# `   string "..."` line down to its raw content. Our fixture only writes
# letters so no escape unmangling is needed.
parse_dbus_strings() {
    sed -n 's/^[[:space:]]*string "\(.*\)"$/\1/p'
}

# Lists managed children of a path. dbus-send doesn't have a path-listing
# command, so we introspect the parent and pull <node name=...> entries.
introspect_children() {
    local service="$1" parent="$2"
    dbus-send --session --print-reply --dest="$service" "$parent" \
        org.freedesktop.DBus.Introspectable.Introspect 2>/dev/null \
        | sed -n 's/.*<node name="\([^"]*\)".*/\1/p'
}

echo "=== pre-creating tmux session running fixture ==="
TUI_READY_FILE="$READY_FILE" TUI_SIZE_FILE="$SIZE_FILE" \
    tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 120 -y 40 "$FIXTURE"

# Wait until the fixture has produced its first frame; the file is touched
# once it's drawn at least once. Without this, kmux might attach before the
# fixture's first SIGWINCH-triggered redraw and the screen would be empty.
ready=0
for _ in $(seq 1 100); do
    if [[ -e "$READY_FILE" ]]; then
        ready=1
        break
    fi
    sleep 0.1
done
if (( ready != 1 )); then
    echo "FAIL: fixture never reported ready (no $READY_FILE)" >&2
    exit 1
fi
echo "OK: fixture printed first frame"

echo "=== launching kmux to attach to existing session ==="
# Use the short -s/-S forms: Qt's QApplication greedily eats `--session` (it's
# Qt's session-manager option), so kmux's --session never reaches our parser
# and the bridge runs `tmux new-session -A` with no session name — landing on
# a stray session and leaving our pre-created one untouched. The short flags
# are not Qt-reserved and pass through cleanly.
#
# --qwindowgeometry to force a non-trivial pane size: bare Xvfb has no window
# manager enforcing initial sizes, so kmux's default size hint resolves to a
# few cols × one row — too degenerate for the wrap assertion to be meaningful.
# 1100x700 fits inside the 1280x720 Xvfb screen lib.sh provisions and gives
# a comfortable ~150x45 pane. The single-letter `-geometry` form is Qt5-era;
# Qt6's QCommandLineParser only honours `--qwindowgeometry` (or its single-
# dash long form `-qwindowgeometry`). Passing `-geometry` instead made
# QCommandLineParser treat it as `-g <eometry>` and abort with "Unknown
# option 'g'.", which silently broke this test once the CI image got Qt6.
#
# konsole.tmux.* debug logging produces the refresh-client -C lines and
# %layout-change events so a failure dump (below) can show what size kmux
# advertised vs what tmux echoed back.
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

# Wait for kmux's window to appear.
WIN=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        echo "FAIL: kmux exited before window appeared (see $LOGDIR/kmux.log)" >&2
        exit 1
    fi
    WIN=$(xdotool search --name kmux 2>/dev/null | tail -1 || true)
    [[ -n "$WIN" ]] && break
    sleep 0.2
done
[[ -n "$WIN" ]] || { echo "FAIL: kmux window never appeared (see $LOGDIR/kmux.log)" >&2; exit 1; }
echo "OK: kmux window appeared (winid=$WIN)"

# Allow kmux to negotiate pane size with tmux and the fixture to redraw at
# the new dimensions. Sample the fixture's size and tmux's pane size every
# 200ms so a failure dump can show whether they converge or oscillate.
echo "--- fixture size (rows cols) | tmux pane (cols rows) ---"
for _ in $(seq 1 10); do
    sleep 0.2
    fix_sample="(missing)"
    [[ -e "$SIZE_FILE" ]] && fix_sample=$(cat "$SIZE_FILE")
    tmux_sample=$(tmux -S "$SOCKET" display-message -p -t "$SESSION" '#{pane_width} #{pane_height}' 2>/dev/null || echo "(error)")
    printf '  fixture=%-10s tmux=%s\n' "$fix_sample" "$tmux_sample"
done

# Read the size the FIXTURE itself observed via stty, not tmux's
# `display-message #{pane_width}` — those can differ. The fixture's stty is
# the only authoritative source for what dimensions the TUI actually drew at.
if [[ ! -e "$SIZE_FILE" ]]; then
    echo "FAIL: fixture never wrote size file ($SIZE_FILE)" >&2
    exit 1
fi
read -r FIX_ROWS FIX_COLS <"$SIZE_FILE"
echo "fixture observed pane size (stty): ${FIX_COLS}x${FIX_ROWS}"
if (( FIX_COLS < 4 || FIX_ROWS < 4 )); then
    echo "FAIL: fixture saw degenerate size ${FIX_COLS}x${FIX_ROWS}" >&2
    exit 1
fi

# Find kmux's D-Bus service. KDBusService::Multiple registers
# org.kde.kmux-<pid>; fall back to plain org.kde.kmux for Unique mode.
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

# Pick the highest-numbered /Sessions/N — that's the freshly attached pane.
sessions_children=""
for _ in $(seq 1 50); do
    sessions_children=$(introspect_children "$SERVICE" /Sessions)
    [[ -n "$sessions_children" ]] && break
    sleep 0.1
done
[[ -n "$sessions_children" ]] || { echo "FAIL: no /Sessions/* on $SERVICE" >&2; exit 1; }
SESSION_ID=$(echo "$sessions_children" | grep -E '^[0-9]+$' | sort -n | tail -1)
SESSION_PATH="/Sessions/${SESSION_ID}"
echo "kmux session path: $SESSION_PATH"

# getAllDisplayedTextList(false): one element per visible row, no trailing-
# empty-row stripping. dbus-send returns "string \"<line>\"" per element.
mapfile -t LINES < <(dbus-send --session --print-reply --dest="$SERVICE" "$SESSION_PATH" \
    org.kde.konsole.Session.getAllDisplayedTextList boolean:false 2>/dev/null \
    | parse_dbus_strings)

echo "kmux returned ${#LINES[@]} lines (fixture wrote $FIX_ROWS)"

fail=0
fail_examples=0
MAX_EXAMPLES=3
chars=(a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W X Y Z)
nchars=${#chars[@]}

report_fail() {
    fail=1
    if (( fail_examples < MAX_EXAMPLES )); then
        echo "FAIL: $*"
        fail_examples=$((fail_examples + 1))
    fi
}

if (( ${#LINES[@]} != FIX_ROWS )); then
    report_fail "row count mismatch — kmux=${#LINES[@]}, fixture=${FIX_ROWS}"
fi

n_to_check=${#LINES[@]}
if (( FIX_ROWS < n_to_check )); then
    n_to_check=$FIX_ROWS
fi

for ((row=0; row<n_to_check; row++)); do
    expected_ch="${chars[row % nchars]}"
    line="${LINES[row]}"
    actual_len=${#line}

    # Per-row uniformity is the load-bearing assertion: any character that
    # isn't the row's letter must have spilled in from a wrap.
    bad_at=-1
    for ((i=0; i<actual_len; i++)); do
        if [[ "${line:i:1}" != "$expected_ch" ]]; then
            bad_at=$i
            break
        fi
    done
    if (( bad_at >= 0 )); then
        snippet="${line:0:60}"
        report_fail "row $row: expected all '$expected_ch', mismatch at col $bad_at; line[0:60]=[${snippet}]"
        continue
    fi

    # Length must match the fixture's intent: $FIX_COLS for body rows,
    # $FIX_COLS-1 for the last row (which deliberately leaves the bottom-
    # right cell blank). A shorter line means kmux's display is narrower
    # than the pty advertises — the missing characters wrapped instead of
    # fitting on the row.
    if (( row == FIX_ROWS - 1 )); then
        expected_len=$((FIX_COLS - 1))
    else
        expected_len=$FIX_COLS
    fi
    if (( actual_len != expected_len )); then
        report_fail "row $row: length=${actual_len}, expected=${expected_len} (char='$expected_ch')"
    fi
done

if (( fail )); then
    echo "" >&2
    echo "===== diagnostics =====" >&2
    echo "" >&2
    echo "--- tmux display-message ---" >&2
    tmux -S "$SOCKET" display-message -p -t "$SESSION" \
        'pane_width=#{pane_width} pane_height=#{pane_height} window_width=#{window_width} window_height=#{window_height} client_width=#{client_width} client_height=#{client_height}' >&2 || true
    echo "" >&2
    echo "--- tmux list-panes ---" >&2
    tmux -S "$SOCKET" list-panes -t "$SESSION" \
        -F '#{pane_id} #{pane_width}x#{pane_height} active=#{pane_active}' >&2 || true
    echo "" >&2
    echo "--- tmux list-clients ---" >&2
    tmux -S "$SOCKET" list-clients >&2 || true
    echo "" >&2
    echo "--- fixture's last-written size (TUI_SIZE_FILE) ---" >&2
    cat "$SIZE_FILE" >&2 || true
    echo "" >&2
    echo "--- kmux DBus row inventory ---" >&2
    {
        echo "kmux returned ${#LINES[@]} lines; fixture wrote $FIX_ROWS"
        if (( ${#LINES[@]} > 0 )); then
            echo "row 0: len=${#LINES[0]} content='${LINES[0]:0:30}...'"
            mid=$(( ${#LINES[@]} / 2 ))
            echo "row $mid: len=${#LINES[$mid]} content='${LINES[$mid]:0:30}...'"
            last_idx=$(( ${#LINES[@]} - 1 ))
            echo "row $last_idx: len=${#LINES[$last_idx]} content='${LINES[$last_idx]:0:30}...'"
            # Find the longest line — that's kmux's max rendered width.
            max_len=0
            for line in "${LINES[@]}"; do
                if (( ${#line} > max_len )); then max_len=${#line}; fi
            done
            echo "max line length seen: $max_len"
        fi
    } >&2
    echo "" >&2
    echo "--- konsole.tmux.resize log lines (the size kmux advertised) ---" >&2
    grep -aE 'konsole\.tmux\.resize' "$LOGDIR/kmux.log" \
        | tail -40 >&2 || true
    echo "" >&2
    echo "--- bridge: refresh-client / layout-change traffic ---" >&2
    grep -aE 'refresh-client|%layout-change|%window-add|%session-changed' "$LOGDIR/kmux.log" \
        | tail -20 >&2 || true
    echo "" >&2
    echo "(full kmux output: $LOGDIR/kmux.log)" >&2
    exit 1
fi

echo "PASS: full-screen TUI rendered without line wrapping (${FIX_COLS}x${FIX_ROWS})"
exit 0
