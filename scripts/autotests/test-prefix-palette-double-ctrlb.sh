#!/usr/bin/env bash
# Repro: "Ctrl+B Ctrl+B works every time in tmux but not in kmux."
#
# In tmux, the default `bind-key -T prefix C-b send-prefix` means pressing the
# prefix twice types a literal prefix byte (0x02) into whatever program is
# running in the pane — readline's backward-char, vim's <C-b>, screen/inner-tmux
# prefixes, claude's chords, etc. all depend on this. kmux reimplements the
# prefix as the TmuxPrefixPalette: the first C-b opens the palette, and the
# second C-b is supposed to resolve to the `send-prefix` binding and dispatch it
# through the gateway, so tmux writes 0x02 into the pane.
#
# The user reports this is INTERMITTENT on kmux (Wayland/KWin): now and then the
# palette just closes and the pane never sees the prefix. The cause is in
# TmuxPrefixPalette::keyPressEvent (src/tmux/TmuxPrefixPalette.cpp): the second
# press is turned into a tmux token by keyEventToTmuxToken(). With the Ctrl
# modifier present the token is "C-b" and matches the send-prefix binding. But if
# the second press arrives with its Ctrl modifier *dropped* — a modifier-state
# race Wayland is prone to during the focus change that opening the palette
# triggers — the token is the bare "b", which matches no prefix binding, so the
# palette takes its "unknown key" branch: it closes and sends NOTHING.
#
# The live chord is reliable under synthetic input (verified: Xvfb+xdotool and
# headless cage+wtype both deliver 0x02 100% of the time), so the harness cannot
# make the compositor drop the modifier on demand. Instead we reproduce the
# *event the race produces*: with the palette open (first real C-b), we deliver
# the second press as a bare `b` — byte-for-byte the QKeyEvent the palette sees
# when Ctrl is lost — and assert the literal prefix still reaches the pane.
#
# The fix (TmuxPrefixPalette::isPrefixRepressWithDroppedModifier) recognises that
# the palette was opened by the C-b prefix and that `b` is that prefix's base key
# with a dropped modifier, and still dispatches send-prefix. This test guards it.
#
# Exit 0 = robust (prefix byte reached the pane despite the dropped modifier)
# Exit 1 = regressed (palette swallowed the key; pane got nothing) — the old bug
# Exit 2 = scaffold (missing tool, kmux didn't start/focus, control chord or the
#                    palette-open precondition didn't hold)

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

export QT_LOGGING_RULES="konsole.tmux.gateway.debug=true"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
# xdotool keystrokes need a focused window; bare Xvfb has no WM, so lib.sh spawns
# twm when USE_XVFB=1.
export KMUX_TEST_NEED_WM="${KMUX_TEST_NEED_WM:-1}"
# On a Wayland host Qt would pick the wayland plugin; force kmux onto our Xvfb.
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"
command -v python3 >/dev/null || kmux_test_bail 2 "python3 not installed"

SOCKET="$HOMEDIR/tmux.sock"
SESSION="demo"
OUT="$HOMEDIR/recv.bin"
DET="$HOMEDIR/detector.py"

# A TUI-style detector: enter the alternate screen and put the tty in raw mode,
# then record every byte tmux delivers into $OUT. Raw mode means a delivered
# prefix byte (0x02) lands immediately, with no Enter needed to flush a line —
# matching the claude/vim case the user hits.
cat >"$DET" <<'PY'
import os, tty
out = open(os.environ['DETECT_OUT'], 'wb', buffering=0)
os.write(1, b'\033[?1049h')   # alternate screen, like a real TUI
tty.setraw(0)
while True:
    b = os.read(0, 1)
    if not b:
        break
    out.write(b)
PY
: >"$OUT"

tmux -S "$SOCKET" -f /dev/null new-session -d -s "$SESSION" -x 200 -y 50
tmux -S "$SOCKET" send-keys -t "$SESSION" "DETECT_OUT='$OUT' exec python3 '$DET'" Enter
sleep 1

echo "=== launching kmux ==="
"$KMUX" -S "$SOCKET" -s "$SESSION" >"$LOGDIR/kmux.log" 2>&1 &
KMUX_PID=$!
cleanup_kmux() {
    if [[ -n "${KMUX_PID:-}" ]] && kill -0 "$KMUX_PID" 2>/dev/null; then
        kill "$KMUX_PID" 2>/dev/null || true
        wait "$KMUX_PID" 2>/dev/null || true
    fi
    [[ -e "$SOCKET" ]] && tmux -S "$SOCKET" kill-server 2>/dev/null || true
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
xdotool windowfocus --sync "$WIN" 2>/dev/null || true
sleep 2  # let the control-mode handshake settle

# Bytes appended to $OUT while running "$@", as a python bytes-repr.
delta_bytes() {
    local off
    off=$(python3 -c "print(len(open('$OUT','rb').read()))")
    "$@"
    sleep 0.5
    python3 -c "d=open('$OUT','rb').read(); print(repr(d[$off:]))"
}
has02() { python3 -c "import sys; d=open('$OUT','rb').read(); sys.exit(0 if b'\x02' in d[$1:] else 1)"; }

# --- scaffold check: the real C-b C-b chord must deliver the prefix ----------
# This proves the whole path is wired in this environment: first C-b opens the
# palette, second C-b resolves to send-prefix, tmux writes 0x02, the detector
# records it. If this fails it's an environment/focus problem, not the bug.
control_ok=0
for _ in 1 2 3; do
    xdotool windowfocus "$WIN" 2>/dev/null || true
    xdotool key --clearmodifiers Escape 2>/dev/null || true; sleep 0.2
    off=$(python3 -c "print(len(open('$OUT','rb').read()))")
    xdotool key --clearmodifiers ctrl+b 2>/dev/null || true
    sleep 0.1
    xdotool key --clearmodifiers ctrl+b 2>/dev/null || true
    sleep 0.5
    if has02 "$off"; then control_ok=1; break; fi
done
if [[ "$control_ok" != 1 ]]; then
    echo "SCAFFOLD: real C-b C-b never delivered 0x02 in this harness — focus or" \
         "handshake problem, not the bug under test. kmux.log tail:" >&2
    tail -20 "$LOGDIR/kmux.log" >&2 || true
    exit 2
fi
echo "OK (scaffold): real C-b C-b delivers the literal prefix to the pane"

# --- the repro: second press with the Ctrl modifier dropped -----------------
# Open the palette with a real C-b, then deliver the second press as a bare `b`
# (the exact QKeyEvent the palette sees when the Wayland race strips Ctrl).
xdotool windowfocus "$WIN" 2>/dev/null || true
xdotool key --clearmodifiers Escape 2>/dev/null || true; sleep 0.2
off=$(python3 -c "print(len(open('$OUT','rb').read()))")
sp_before=$(grep -c 'sendCommand: "send-prefix"' "$LOGDIR/kmux.log" 2>/dev/null || true)
seq_bytes=$(delta_bytes bash -c "
    xdotool key --clearmodifiers ctrl+b   # opens the prefix palette
    sleep 0.15
    xdotool key --clearmodifiers b        # 2nd press, Ctrl dropped by the race
")
sp_after=$(grep -c 'sendCommand: "send-prefix"' "$LOGDIR/kmux.log" 2>/dev/null || true)
sp_delta=$(( ${sp_after:-0} - ${sp_before:-0} ))
echo "second-press-loses-ctrl -> pane received: $seq_bytes ; send-prefix dispatched during repro: $sp_delta"

if has02 "$off"; then
    echo "PASS: literal prefix (0x02) reached the pane even though the second"
    echo "      press lost its Ctrl modifier — palette is robust to the race."
    exit 0
fi

# No prefix byte. Distinguish the bug from a scaffold miss: if a plain 'b' (0x62)
# reached the pane, the palette was NOT open when the second key arrived, so this
# run didn't exercise the path — treat as scaffold. If nothing arrived, the
# palette consumed the key and sent nothing: the bug.
if [[ "$seq_bytes" == *"b'b'"* || "$seq_bytes" == *"\\x62"* ]]; then
    echo "SCAFFOLD: the bare 'b' reached the pane, so the palette wasn't open when" \
         "the second key was sent — precondition not met, rerun." >&2
    exit 2
fi

echo "FAIL (bug reproduced): the palette swallowed the modifier-dropped second" >&2
echo "  press and closed without dispatching send-prefix (send-prefix dispatched" >&2
echo "  during the repro: $sp_delta), so the pane received nothing ($seq_bytes)." >&2
echo "  This is the intermittent 'C-b C-b does nothing' the user hits on Wayland." >&2
echo "  See TmuxPrefixPalette::keyPressEvent / keyEventToTmuxToken." >&2
exit 1
