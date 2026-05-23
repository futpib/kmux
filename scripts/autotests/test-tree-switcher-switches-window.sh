#!/usr/bin/env bash
# Repro: switching to a different window via kmux's native tree switcher
# (the popup that replaces tmux's `Prefix + w` choose-tree picker) does not
# actually change which Konsole tab is shown — the user reports staying on
# the tab they were viewing before opening the picker, even after Enter on
# a different row.
#
# Trigger path under test (bypassing the prefix palette by binding a
# static shortcut directly to the action — see the [Shortcuts] seed
# below for why):
#   xdotool Ctrl+Alt+W        → tmux-tree-switcher-windows action fires
#                               and opens TmuxTreeSwitcher in Windows mode
#   xdotool Down              → move from the pre-selected active-window
#                               row to the next window
#   xdotool Return            → TmuxTreeSwitcher::activateCurrent →
#                               TmuxController::requestSelectWindow
#
# Observable: kmux's main-window X title contains the active tab's title.
# We seed a profile with LocalTabTitleFormat=%w so the OSC pane title (which we
# set explicitly per window to ALPHA/BETA/GAMMA) surfaces in the X title — and
# we can tell whether the visible tab changed via xdotool getwindowname.
#
# Exit 0 = bug fixed (kmux window title contains BETA after the switch).
# Exit 1 = bug still present (window title still contains ALPHA — expected today).
# Exit 2 = scaffolding failure (missing tool, kmux didn't start, title format
#         seed didn't take, etc.).

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

# Help future debugging: bridge + control-mode logs surface in $LOGDIR/kmux.log
export QT_LOGGING_RULES="konsole.tmux.bridge.debug=true;konsole.tmux.controller.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
# xdotool keystrokes need a focused window; bare Xvfb has no WM, so lib.sh
# spawns twm when USE_XVFB=1.
export KMUX_TEST_NEED_WM="${KMUX_TEST_NEED_WM:-1}"
# On a Wayland host, Qt picks the wayland plugin over xcb even when DISPLAY
# is set; force kmux onto our Xvfb's X server.
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"
command -v xdotool >/dev/null || kmux_test_bail 2 "xdotool not installed"

# --- seed a Konsole profile that exposes %w in the tab title format ------
# That's how we observe the active tab changing (or not): the X-window title
# of kmux follows the active session's title, which the format pipeline pulls
# from `userTitle()` — i.e., what OSC 0/2 wrote. Without %w in the format the
# default `%d : %n` collapses every window's title to the same string.
export XDG_DATA_HOME="${XDG_DATA_HOME:-$HOMEDIR/.local/share}"
mkdir -p "$XDG_DATA_HOME"

PROFILE_NAME="TreeSwitchRepro"
for rc in konsolerc kmuxrc; do
    cat >"$XDG_CONFIG_HOME/$rc" <<RC
[Desktop Entry]
DefaultProfile=${PROFILE_NAME}.profile

# Bypass the Prefix+w → prefix-palette → tree-switcher chain by binding a
# static shortcut straight to the tree-switcher's windows-mode action. The
# bug under test is in the switcher's activation path (Enter on a row →
# active Konsole tab follows), not in the palette dispatch — testing the
# palette path is blocked separately by the dynamic prefix-shortcut bind
# not being live in time / the terminal display swallowing Ctrl+B.
[Shortcuts]
tmux-tree-switcher-windows=Ctrl+Alt+W
RC
done
for d in "$XDG_DATA_HOME/konsole" "$XDG_DATA_HOME/kmux"; do
    mkdir -p "$d"
    cat >"$d/${PROFILE_NAME}.profile" <<PROF
[General]
Name=${PROFILE_NAME}
Parent=FALLBACK/
LocalTabTitleFormat=%w
TerminalColumns=120
TerminalRows=30
PROF
done

SOCKET="$HOMEDIR/tmux.sock"
SESSION="demo"

dump_tmux_state() {
    local label="$1"
    echo "--- tmux state: $label ---" >&2
    tmux -S "$SOCKET" list-clients -F 'client: tty=#{client_tty} session=#{client_session} session_id=#{session_id}' 2>&1 >&2 || true
    tmux -S "$SOCKET" list-windows -t "$SESSION" \
        -F 'win: idx=#{window_index} id=#{window_id} name=#{window_name} panes=#{window_panes} active=#{?window_active,1,0}' 2>&1 >&2 || true
    tmux -S "$SOCKET" list-panes -s -t "$SESSION" \
        -F 'pane: id=#{pane_id} win=#{window_index} cmd=#{pane_current_command} title="#{pane_title}" active=#{?pane_active,1,0}' 2>&1 >&2 || true
}
dump_pane_capture() {
    local target="$1" label="$2"
    echo "--- capture-pane $target ($label) ---" >&2
    tmux -S "$SOCKET" capture-pane -p -t "$target" 2>&1 >&2 || true
}
dump_xdotool_state() {
    local label="$1"
    echo "--- xdotool state: $label ---" >&2
    echo "focused-win: $(xdotool getactivewindow 2>&1 || true)" >&2
    echo "focused-name: $(xdotool getactivewindow getwindowname 2>&1 || true)" >&2
    echo "kmux-name: $(xdotool getwindowname "$WIN" 2>&1 || true)" >&2
}

echo "=== pre-creating tmux session with 3 windows ==="
# Disable automatic-rename globally so window_name doesn't shift under us
# when the foreground process changes (sleep/title-and-hold.sh) — we keep
# window-name stable for the test's own diagnostic readability, and
# pane_current_command for triage.
tmux -S "$SOCKET" -f /dev/null start-server 2>/dev/null || true
tmux -S "$SOCKET" set-option -g automatic-rename off 2>/dev/null || true
tmux -S "$SOCKET" new-session -d -s "$SESSION" -n alpha -x 200 -y 50
tmux -S "$SOCKET" new-window -t "${SESSION}:1" -n beta
tmux -S "$SOCKET" new-window -t "${SESSION}:2" -n gamma
tmux -S "$SOCKET" select-window -t "${SESSION}:0"
dump_tmux_state "after session setup, before kmux"

echo "=== launching kmux to attach ==="
"$KMUX" -S "$SOCKET" -s "$SESSION" >"$LOGDIR/kmux.log" 2>&1 &
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
        echo "FAIL: kmux exited before window appeared (see $LOGDIR/kmux.log)" >&2
        exit 2
    fi
    WIN=$(xdotool search --onlyvisible --class kmux 2>/dev/null | tail -1 || true)
    [[ -n "$WIN" ]] && break
    sleep 0.2
done
[[ -n "$WIN" ]] || { echo "FAIL: kmux window never appeared (see $LOGDIR/kmux.log)" >&2; exit 2; }

xdotool windowactivate --sync "$WIN" 2>/dev/null || true
sleep 2 # let the control-mode handshake settle
dump_tmux_state "after kmux attached"
dump_xdotool_state "after kmux attached"

# --- assign distinct OSC titles per window's primary pane ----------------
# Use `respawn-pane` instead of send-keys: bash's default PS1 emits an OSC
# title on every prompt redraw, which racily clobbers anything we type. The
# helper sets the title and then sleeps, so the pane's process is no longer
# bash at all and the title stays put for the rest of the test.
HELPER="$HOMEDIR/title-and-hold.sh"
cat >"$HELPER" <<'EOF'
#!/usr/bin/env bash
printf '\033]2;%s\007' "$1"
exec sleep 999999
EOF
chmod +x "$HELPER"
for spec in "0:ALPHA" "1:BETA" "2:GAMMA"; do
    idx="${spec%%:*}"
    name="${spec##*:}"
    echo "respawn pane ${SESSION}:${idx}.0 → '$HELPER $name'"
    tmux -S "$SOCKET" respawn-pane -k -t "${SESSION}:${idx}.0" "$HELPER $name" 2>&1 || true
done
sleep 2  # let the OSC sequences flow through control mode into kmux
dump_tmux_state "after respawn-pane title seeding"
dump_pane_capture "${SESSION}:0.0" "post-respawn window 0"
dump_pane_capture "${SESSION}:1.0" "post-respawn window 1"
dump_pane_capture "${SESSION}:2.0" "post-respawn window 2"

# --- starting state: window 0 (ALPHA) is the active tab ------------------
tmux -S "$SOCKET" select-window -t "${SESSION}:0"
sleep 0.5

get_title() { xdotool getwindowname "$WIN" 2>/dev/null || echo "<error>"; }
initial_title=$(get_title)
echo "initial kmux window title: $initial_title"
if [[ "$initial_title" != *ALPHA* ]]; then
    echo "SCAFFOLD: kmux X title didn't pick up the seeded OSC titles" \
         "(\"$initial_title\"). The LocalTabTitleFormat=%w seed isn't" \
         "in effect — investigate before trusting this test." >&2
    exit 2
fi

# --- exercise the bug repro path -----------------------------------------
# kmux binds the tmux prefix as the shortcut for `tmux-prefix-palette`
# dynamically once the controller reports the prefix; give that a beat
# beyond the control-mode handshake to land before we send Ctrl+B.
sleep 1.5
DELAY_MS=150
press_key() {
    local key="$1" label="$2"
    xdotool windowactivate --sync "$WIN" 2>/dev/null || true
    echo "--- press: $key  ($label) ---" >&2
    dump_xdotool_state "before $key"
    xdotool key --delay "$DELAY_MS" --window "$WIN" "$key"
}
echo "=== Ctrl+Alt+W → tmux-tree-switcher-windows action → TmuxTreeSwitcher ==="
press_key ctrl+alt+w "open tree switcher (windows mode)"
sleep 1.5
dump_pane_capture "${SESSION}:0.0" "after ctrl+alt+w (terminal must NOT have consumed it)"
dump_xdotool_state "after ctrl+alt+w (switcher should be open)"
echo "=== Down → select the window 1 (BETA) row ==="
press_key Down "navigate to BETA"
sleep 0.5
echo "=== Return → activate ==="
press_key Return "activate the selection"
sleep 1.5
dump_tmux_state "after activation"

# --- assertion: kmux's UI is now showing the BETA tab --------------------
final_title=$(get_title)
echo "final kmux window title: $final_title"

# Diagnostic dump — useful regardless of pass/fail to triage whether the
# tmux side moved (issued select-window) but the Konsole tab didn't follow.
echo "--- tmux state after switch ---" >&2
tmux -S "$SOCKET" list-windows -t "$SESSION" \
    -F '#{window_index} #{window_name} #{?window_active,ACTIVE,}' >&2 || true

if [[ "$final_title" == *BETA* && "$final_title" != *ALPHA* ]]; then
    echo "PASS: kmux switched from ALPHA to BETA via the tree switcher"
    exit 0
fi

echo "FAIL: kmux X title is still \"$final_title\" — bug present (the" \
     "tree-switcher selection didn't change the active Konsole tab)" >&2
exit 1
