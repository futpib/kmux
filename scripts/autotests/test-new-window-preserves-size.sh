#!/usr/bin/env bash
# Bug repro: invoking New Window (Ctrl+Shift+N) on a tmux-attached kmux
# window spawns a *second* kmux MainWindow — a second tmux control-mode
# client attached to the same session, showing only the freshly created
# tmux window (Application::createTmuxWindow). Closing that new window
# should leave the ORIGINAL window exactly as it was.
#
# The user reports the opposite: after the new window appears and is closed,
# the pre-existing window's size is "messed up". The damage is in the tmux
# size bookkeeping: TmuxResizeCoordinator caches the last size it advertised
# per window (_lastClientSizes) and the size tmux last reported per window
# (_tmuxWindowSizes, fed by every %layout-change → setWindowSize). The
# second client perturbs the session, and because sendClientSize only
# re-emits `refresh-client -C` when *its own* computed size changes, the
# original window is never re-synced. The corruption is often latent at the
# top level but surfaces the moment you split: onSplitterMoved clamps the
# new layout to _tmuxWindowSizes[window] before sending select-layout, so a
# stale window size yields a split that doesn't tile the window (or a tmux
# "size mismatch").
#
# This test drives the exact manual flow, then splits:
#   1. Pre-create a small tmux session (1 window / 1 pane) and attach kmux at
#      a fixed pixel geometry. kmux negotiates the window up to the size its
#      pixels imply — that negotiated size is the baseline.
#   2. Record the original tmux window/pane size and kmux's X-window size.
#   3. Press Ctrl+Shift+N. Wait until tmux has 2 windows AND a second kmux
#      X window is visible AND the new window's pane has drawn content.
#   4. Close the new kmux window. Wait until only the original remains.
#   5. Re-sample the ORIGINAL window — assert it matches the baseline.
#   6. Split the original window (Ctrl+(). Assert the two panes tile the
#      original window exactly and the window size is still the baseline.
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = original window preserved and splittable (bug fixed).
# Exit 1 = original window size disturbed, or split doesn't tile it
#          (bug present — expected today).
# Exit 2 = scaffolding failure (missing tool, kmux didn't start, etc.).

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

# resize.debug emits the refresh-client -C lines / computed sizes; controller
# emits %layout-change / select-layout; bridge emits raw control traffic. All
# surface in $LOGDIR/kmux.log for triage.
export QT_LOGGING_RULES="konsole.tmux.resize.debug=true;konsole.tmux.controller.debug=true;konsole.tmux.bridge.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
# xdotool keystrokes need a focused window; bare Xvfb has no window manager,
# so have lib.sh spawn twm when USE_XVFB=1.
export KMUX_TEST_NEED_WM="${KMUX_TEST_NEED_WM:-1}"
# On a Wayland host, Qt prefers the wayland plugin even when DISPLAY is set;
# force kmux onto our Xvfb's X server so xdotool can drive it.
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"

SOCKET="$HOMEDIR/tmux.sock"
SESSION="winsize"

echo "=== pre-creating tmux session (1 window, 1 pane, small) ==="
# Start small (tmux's 80x24 default). kmux's TmuxResizeCoordinator grows the
# window to the cell count its 1100x700 px frame implies (max(layoutCells,
# pixelCells) per axis), so the baseline is a window already matched to the
# frame — the realistic state the user is in before pressing Ctrl+Shift+N.
tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 80 -y 24
tmux -S "$SOCKET" set-option -g automatic-rename off 2>/dev/null || true

echo "=== launching kmux to attach ==="
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

# --- helpers --------------------------------------------------------------

tmux_window_cells() {
    tmux -S "$SOCKET" display-message -p -t "$1" '#{window_width}x#{window_height}' 2>/dev/null || echo '?'
}
tmux_pane_cells() {
    tmux -S "$SOCKET" display-message -p -t "$1" '#{pane_width}x#{pane_height}' 2>/dev/null || echo '?'
}
x_window_size() {
    local id="$1" WIDTH="" HEIGHT=""
    eval "$(xdotool getwindowgeometry --shell "$id" 2>/dev/null | grep -E '^(WIDTH|HEIGHT)=')" 2>/dev/null || true
    echo "${WIDTH:-?}x${HEIGHT:-?}"
}
count_kmux_windows() {
    xdotool search --onlyvisible --class kmux 2>/dev/null | wc -l
}
# Echo a visible kmux top-level window id != $WIN, large enough to be a real
# MainWindow (filters Qt's tiny "Selection Owner" helper). Returns 1 if none.
other_kmux_window() {
    local w sz wpx
    for w in $(xdotool search --onlyvisible --class kmux 2>/dev/null); do
        [[ "$w" == "$WIN" ]] && continue
        sz=$(x_window_size "$w"); wpx=${sz%x*}
        [[ "$wpx" =~ ^[0-9]+$ ]] || continue
        if (( wpx > 100 )); then echo "$w"; return 0; fi
    done
    return 1
}
panes_in() {
    tmux -S "$SOCKET" list-panes -t "$1" 2>/dev/null | wc -l
}
wait_for_window_count() {
    local target="$1" what="$2" got=
    for _ in $(seq 1 50); do
        got=$(tmux -S "$SOCKET" list-windows -t "$SESSION" 2>/dev/null | wc -l)
        (( got == target )) && return 0
        sleep 0.2
    done
    echo "FAIL: $what — got $got tmux windows, expected $target" >&2
    return 1
}
dump_state() {
    echo "--- state: $1 ---" >&2
    tmux -S "$SOCKET" list-clients -F 'client: tty=#{client_tty} size=#{client_width}x#{client_height}' >&2 2>&1 || true
    tmux -S "$SOCKET" list-windows -t "$SESSION" \
        -F 'win: id=#{window_id} name=#{window_name} size=#{window_width}x#{window_height} active=#{?window_active,1,0}' >&2 2>&1 || true
    tmux -S "$SOCKET" list-panes -s -t "$SESSION" \
        -F 'pane: id=#{pane_id} win=#{window_index} geom=#{pane_left},#{pane_top}+#{pane_width}x#{pane_height} cmd=#{pane_current_command}' >&2 2>&1 || true
    local w
    for w in $(xdotool search --onlyvisible --class kmux 2>/dev/null); do
        echo "kmux-win $w x-size=$(x_window_size "$w")" >&2
    done
}

# --- wait for kmux's initial window --------------------------------------
WIN=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        echo "FAIL: kmux exited before window appeared" >&2
        tail -50 "$LOGDIR/kmux.log" >&2 2>/dev/null || true
        exit 2
    fi
    WIN=$(xdotool search --onlyvisible --class kmux 2>/dev/null | tail -1 || true)
    [[ -n "$WIN" ]] && break
    sleep 0.2
done
[[ -n "$WIN" ]] || { echo "FAIL: kmux window never appeared (see $LOGDIR/kmux.log)" >&2; exit 2; }
echo "OK: kmux window appeared (winid=$WIN)"

xdotool windowactivate --sync "$WIN" 2>/dev/null || true
sleep 1  # control-mode handshake
# Force a clean, deterministic size negotiation. On a fresh attach kmux can
# latch the window to the session's spawn size before its widgets are laid
# out — and the resize timer only re-fires on focus/view/pane-size changes,
# so it can stay stuck at e.g. 80x24. An explicit resize fires a resize event
# → TmuxResizeCoordinator recomputes from real pixels → grows the window to
# fit the frame. Resize to a size different from the launch geometry so the
# event actually fires. This is the realistic state (a window at its
# negotiated size) the user is in before pressing Ctrl+Shift+N.
xdotool windowsize --sync "$WIN" 1000 650 2>/dev/null || true
sleep 2  # let the renegotiation settle

# --- baseline: one window, negotiated up to the frame --------------------
wait_for_window_count 1 "starting window count" || { dump_state "bad start"; exit 2; }
ORIG_WID=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}' 2>/dev/null | head -1)
ORIG_PANE=$(tmux -S "$SOCKET" list-panes -t "$ORIG_WID" -F '#{pane_id}' 2>/dev/null | head -1)
[[ -n "$ORIG_WID" && -n "$ORIG_PANE" ]] || { echo "FAIL: no original window/pane id" >&2; dump_state "no ids"; exit 2; }

echo "--- baseline size negotiation (window | pane | X-pixels) ---"
for _ in $(seq 1 10); do
    printf '  win=%-9s pane=%-9s x=%s\n' \
        "$(tmux_window_cells "$ORIG_WID")" "$(tmux_pane_cells "$ORIG_PANE")" "$(x_window_size "$WIN")"
    sleep 0.2
done
WIN_CELLS_BEFORE=$(tmux_window_cells "$ORIG_WID")
PANE_CELLS_BEFORE=$(tmux_pane_cells "$ORIG_PANE")
X_SIZE_BEFORE=$(x_window_size "$WIN")
echo "BEFORE: window=$WIN_CELLS_BEFORE pane=$PANE_CELLS_BEFORE x-pixels=$X_SIZE_BEFORE (wid=$ORIG_WID pane=$ORIG_PANE)"

cols_before=${WIN_CELLS_BEFORE%x*}
rows_before=${WIN_CELLS_BEFORE#*x}
# kmux must have grown the window well past tmux's 80x24 default — otherwise it
# never negotiated a frame-matched size and the assertion is meaningless.
if ! [[ "$cols_before" =~ ^[0-9]+$ && "$rows_before" =~ ^[0-9]+$ ]] || (( cols_before < 100 || rows_before < 30 )); then
    echo "SCAFFOLD: kmux didn't negotiate a frame-sized window (got '$WIN_CELLS_BEFORE'," \
         "expected kmux to grow 80x24 toward the 1100x700 frame). Size assertion would" \
         "be meaningless." >&2
    dump_state "no negotiation"
    exit 2
fi

# --- step 1: New Window (Ctrl+Shift+N) ------------------------------------
echo "=== Ctrl+Shift+N (new window) ==="
xdotool key --delay 150 ctrl+shift+n
wait_for_window_count 2 "after Ctrl+Shift+N" || { dump_state "no 2nd window"; exit 2; }
NEW_WID=$(tmux -S "$SOCKET" list-windows -t "$SESSION" -F '#{window_id}' 2>/dev/null | grep -vx "$ORIG_WID" | head -1)
echo "OK: tmux now has 2 windows (original=$ORIG_WID new=$NEW_WID)"

NEWWIN=""
for _ in $(seq 1 100); do
    if ! kill -0 "$KMUX_PID" 2>/dev/null; then
        echo "FAIL: kmux exited while opening the new window" >&2
        tail -50 "$LOGDIR/kmux.log" >&2 2>/dev/null || true
        exit 2
    fi
    NEWWIN=$(other_kmux_window || true)
    [[ -n "$NEWWIN" ]] && break
    sleep 0.2
done
[[ -n "$NEWWIN" ]] || { echo "FAIL: second kmux window never appeared" >&2; dump_state "no 2nd X window"; exit 2; }
echo "OK: second kmux window appeared (winid=$NEWWIN)"

# Wait for the new window to draw content (its shell prompt).
HAVE_CONTENT=0
for _ in $(seq 1 50); do
    txt=$(tmux -S "$SOCKET" capture-pane -p -t "$NEW_WID" 2>/dev/null || true)
    if [[ -n "${txt//[$' \t\r\n']/}" ]]; then HAVE_CONTENT=1; break; fi
    sleep 0.2
done
(( HAVE_CONTENT == 1 )) || { echo "SCAFFOLD: new window never drew content" >&2; dump_state "new window empty"; exit 2; }
echo "OK: new window has content"

# While both clients are attached, watch the ORIGINAL window for transient
# resizes (diagnostic — a flap that settles back would still mislead a user).
echo "--- original window while 2 clients attached ---"
for _ in $(seq 1 5); do
    printf '  orig win=%s pane=%s\n' "$(tmux_window_cells "$ORIG_WID")" "$(tmux_pane_cells "$ORIG_PANE")"
    sleep 0.2
done
dump_state "after new window opened (before close)"

# --- step 2: close the new window -----------------------------------------
echo "=== closing the new window ==="
xdotool windowactivate --sync "$NEWWIN" 2>/dev/null || true
sleep 0.3
xdotool key --delay 150 ctrl+shift+q  # kmux "Close Window"; 1 tab → no confirm
closed=0
for _ in $(seq 1 50); do
    xdotool search --onlyvisible --class kmux 2>/dev/null | grep -qx "$NEWWIN" || { closed=1; break; }
    sleep 0.2
done
if (( closed != 1 )); then
    echo "note: Ctrl+Shift+Q didn't close it; trying WM close" >&2
    xdotool windowclose "$NEWWIN" 2>/dev/null || true
    for _ in $(seq 1 30); do
        xdotool search --onlyvisible --class kmux 2>/dev/null | grep -qx "$NEWWIN" || { closed=1; break; }
        sleep 0.2
    done
fi
(( closed == 1 )) || { echo "FAIL: could not close the new kmux window ($NEWWIN)" >&2; dump_state "won't close"; exit 2; }
echo "OK: new window closed (kmux windows now: $(count_kmux_windows))"

# --- step 3: re-sample the ORIGINAL window --------------------------------
# Re-focus it: closing the new window detaches its tmux client, and a focus
# change is what nudges TmuxResizeCoordinator to re-emit the client size, so
# this is where a fix would restore the original — give it every chance.
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
echo "--- original size after close (window | pane | X-pixels) ---"
for _ in $(seq 1 15); do
    printf '  win=%-9s pane=%-9s x=%s\n' \
        "$(tmux_window_cells "$ORIG_WID")" "$(tmux_pane_cells "$ORIG_PANE")" "$(x_window_size "$WIN")"
    sleep 0.2
done
WIN_CELLS_AFTER=$(tmux_window_cells "$ORIG_WID")
PANE_CELLS_AFTER=$(tmux_pane_cells "$ORIG_PANE")
X_SIZE_AFTER=$(x_window_size "$WIN")
echo "AFTER:  window=$WIN_CELLS_AFTER pane=$PANE_CELLS_AFTER x-pixels=$X_SIZE_AFTER"

# --- step 4: split the original window ------------------------------------
# The user's hint: the damage shows up when you split after the cycle. A
# correct kmux splits the *focused* window — the original (@0). The bug: the
# new-window+close cycle leaves the controller's active pane pointing into the
# new window's now-hidden/orphaned tmux window, so Ctrl+( splits THAT window
# instead of what the user sees. (Same stale-activePaneId family as
# test-tab-split-pane-counts.sh, reached via the new-window path.)
echo "=== splitting the original window (Ctrl+( ) ==="
PANES_ORIG_PRE=$(panes_in "$ORIG_WID")
PANES_TOTAL_PRE=$(tmux -S "$SOCKET" list-panes -s -t "$SESSION" 2>/dev/null | wc -l)
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
sleep 0.3
xdotool key --delay 150 ctrl+parenleft

# Classify the outcome: did the focused window (@0) gain a pane (correct), did
# some OTHER window gain one (split hit the wrong target — the bug), or did
# nothing happen anywhere (keystroke lost — scaffold, not a kmux bug)?
split_target=""
for _ in $(seq 1 50); do
    po=$(panes_in "$ORIG_WID")
    pt=$(tmux -S "$SOCKET" list-panes -s -t "$SESSION" 2>/dev/null | wc -l)
    if (( po > PANES_ORIG_PRE )); then split_target="orig"; break; fi
    if (( pt > PANES_TOTAL_PRE )); then split_target="wrong"; break; fi
    sleep 0.2
done

if [[ -z "$split_target" ]]; then
    echo "SCAFFOLD: Ctrl+( produced no new pane anywhere — the keystroke was lost" \
         "(not a kmux bug); cannot evaluate the split." >&2
    dump_state "split keystroke lost"
    exit 2
fi
if [[ "$split_target" == "wrong" ]]; then
    echo "FAIL: Ctrl+( split the WRONG tmux window. The focused window shows the" \
         "original (@0), but the new pane landed in another window — the leftover" \
         "from the closed new window. After the new-window+close cycle the" \
         "controller's active pane still points into that window, so the split" \
         "(and other pane ops) operate on it instead of what the user sees." >&2
    echo "  @0 panes: $PANES_ORIG_PRE → $(panes_in "$ORIG_WID") (unchanged ⇒ split went elsewhere)" >&2
    echo "" >&2
    echo "===== diagnostics =====" >&2
    dump_state "split hit wrong window" >&2
    echo "--- split / active-pane / window traffic ---" >&2
    # Filter tmux's bind-key / display-menu table dumps — they contain the
    # literal "split-window" from keybinding definitions and would bury the
    # actual commands kmux issued.
    { grep -aE 'split-window|select-layout|activePane|splitView|hideWindow|%window-add|%window-close' \
        "$LOGDIR/kmux.log" 2>/dev/null | grep -av -e 'bind-key' -e 'display-menu' | tail -50; } >&2 || true
    echo "(full kmux output: $LOGDIR/kmux.log)" >&2
    exit 1
fi
sleep 1  # split landed on @0; let select-layout settle
echo "OK: split landed on the original window; it now has $(panes_in "$ORIG_WID") panes"

# Read pane geometry and verify the panes tile the window exactly.
WIN_CELLS_SPLIT=$(tmux_window_cells "$ORIG_WID")
sw=${WIN_CELLS_SPLIT%x*}
sh=${WIN_CELLS_SPLIT#*x}
echo "--- panes after split (window now $WIN_CELLS_SPLIT) ---"
min_l=99999; min_t=99999; max_r=-1; max_b=-1; overflow=0
while read -r pl pt pr pb pw ph pid; do
    [[ "$pl" =~ ^[0-9]+$ ]] || continue
    echo "  pane $pid: left=$pl top=$pt right=$pr bottom=$pb size=${pw}x${ph}"
    (( pl < min_l )) && min_l=$pl
    (( pt < min_t )) && min_t=$pt
    (( pr > max_r )) && max_r=$pr
    (( pb > max_b )) && max_b=$pb
    # No pane may extend past the window the user can actually see.
    (( pr > sw - 1 || pb > sh - 1 )) && overflow=1
done < <(tmux -S "$SOCKET" list-panes -t "$ORIG_WID" \
    -F '#{pane_left} #{pane_top} #{pane_right} #{pane_bottom} #{pane_width} #{pane_height} #{pane_id}' 2>/dev/null)

# --- assertions -----------------------------------------------------------
echo
echo "===== summary ====="
echo "  original tmux window cells : $WIN_CELLS_BEFORE → $WIN_CELLS_AFTER (after split: $WIN_CELLS_SPLIT)"
echo "  original tmux pane  cells  : $PANE_CELLS_BEFORE → $PANE_CELLS_AFTER"
echo "  kmux X-window pixels       : $X_SIZE_BEFORE → $X_SIZE_AFTER"
echo "  split panes bounds         : left>=$min_l top>=$min_t right<=$max_r bottom<=$max_b vs window ${sw}x${sh}"

fail=0
[[ "$WIN_CELLS_AFTER"  == "$WIN_CELLS_BEFORE"  ]] || { echo "FAIL: original tmux window resized $WIN_CELLS_BEFORE → $WIN_CELLS_AFTER" >&2; fail=1; }
[[ "$PANE_CELLS_AFTER" == "$PANE_CELLS_BEFORE" ]] || { echo "FAIL: original tmux pane resized $PANE_CELLS_BEFORE → $PANE_CELLS_AFTER" >&2; fail=1; }
# The X frame is only ever resized by kmux itself under Xvfb+twm.
if [[ "$X_SIZE_AFTER" != "$X_SIZE_BEFORE" && "$X_SIZE_BEFORE" != *'?'* && "$X_SIZE_AFTER" != *'?'* ]]; then
    echo "FAIL: kmux X-window resized $X_SIZE_BEFORE → $X_SIZE_AFTER" >&2; fail=1
fi
# A left/right split legitimately costs ~1 column to the vertical splitter
# handle (124x38 → 123x38), so don't demand the window size be unchanged —
# only that it stayed essentially the baseline and didn't collapse (e.g. to
# the orphan window's 80x24, which is what a mis-targeted/desynced split
# would leave). The tiling checks below confirm the panes fill whatever
# size it settled at.
(( sw >= cols_before - 2 && sw <= cols_before )) || { echo "FAIL: window width changed too much across the split: ${cols_before} → ${sw}" >&2; fail=1; }
(( sh >= rows_before - 2 && sh <= rows_before )) || { echo "FAIL: window height changed too much across the split: ${rows_before} → ${sh}" >&2; fail=1; }
# The two panes must tile the window: start at the origin, reach both far
# edges, and never overflow.
(( overflow == 0 ))      || { echo "FAIL: a split pane extends past the window ${sw}x${sh}" >&2; fail=1; }
(( min_l == 0 && min_t == 0 )) || { echo "FAIL: split panes don't start at the window origin (left=$min_l top=$min_t)" >&2; fail=1; }
(( max_r == sw - 1 ))    || { echo "FAIL: split panes don't reach the right edge (max right=$max_r, expected $((sw-1)))" >&2; fail=1; }
(( max_b == sh - 1 ))    || { echo "FAIL: split panes don't reach the bottom edge (max bottom=$max_b, expected $((sh-1)))" >&2; fail=1; }

if (( fail )); then
    echo "" >&2
    echo "===== diagnostics =====" >&2
    dump_state "final (bug present)" >&2
    echo "--- size / layout traffic ---" >&2
    { grep -aE 'refresh-client|%layout-change|sendClientSize|setWindowSize|select-layout|hideWindow|mismatch|%error' \
        "$LOGDIR/kmux.log" 2>/dev/null | grep -av -e 'bind-key' -e 'display-menu' | tail -80; } >&2 || true
    echo "(full kmux output: $LOGDIR/kmux.log)" >&2
    echo "FAIL: the new-window+close cycle disturbed the pre-existing window" >&2
    exit 1
fi

echo "PASS: original window kept its size and splits cleanly after the new-window cycle"
exit 0
