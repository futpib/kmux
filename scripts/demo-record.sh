#!/usr/bin/env bash
# Record a self-contained kmux demo with no desktop, no clicks — but under a
# real KWin compositor for proper rendering, at 1080p.
#
# Pipeline:  Xvfb (1920x1080)  ->  kwin_wayland nested on it via --x11-display
#            (software compositing, KWIN_COMPOSE=Q)  ->  kmux --fullscreen as
#            a Wayland client  ->  ffmpeg x11grab of the Xvfb.
#
# The choreography is driven entirely from the tmux server side, so it needs
# no synthetic GUI input (none is available for Wayland clients here). That
# also means the Ctrl+B prefix-palette beat from the twm version is gone.
#
# Output: doc/demo.mp4 (1080p master) + doc/demo.webp (README-ready).
#
# Knobs: KMUX_BIN (default /usr/bin/kmux), KEEP_TMP=1, W, H.

set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
KMUX_BIN="${KMUX_BIN:-/usr/bin/kmux}"
W="${W:-1920}"            # logical desktop size
H="${H:-1080}"
SCALE="${SCALE:-2}"       # HiDPI: KWin renders at this device-pixel scale
PW=$(( W * SCALE ))       # physical pixels actually rendered + captured
PH=$(( H * SCALE ))
OUT_DIR="$REPO/doc"
MP4="$OUT_DIR/demo.mp4"

[[ -x "$KMUX_BIN" ]] || { echo "error: kmux not found at $KMUX_BIN" >&2; exit 2; }
for t in Xvfb kwin_wayland xdotool ffmpeg dbus-launch tmux; do
    command -v "$t" >/dev/null || { echo "error: $t not installed" >&2; exit 2; }
done

TMPHOME=$(mktemp -d)
LOGDIR="$TMPHOME/logs"; mkdir -p "$LOGDIR"
SOCKET="$TMPHOME/tmux.sock"
SESSION="demo"

XVFB_PID=""; KWIN_PID=""; FF_PID=""
cleanup() {
    local rc=$?
    [[ -n "$FF_PID"   ]] && kill -INT "$FF_PID" 2>/dev/null
    [[ -e "$SOCKET"   ]] && tmux -S "$SOCKET" kill-server 2>/dev/null
    [[ -n "$KWIN_PID" ]] && kill "$KWIN_PID" 2>/dev/null   # also kills its kmux child
    [[ -n "${DBUS_SESSION_BUS_PID:-}" ]] && kill "$DBUS_SESSION_BUS_PID" 2>/dev/null
    [[ -n "$XVFB_PID" ]] && kill "$XVFB_PID" 2>/dev/null
    sleep 0.3
    if [[ "${KEEP_TMP:-0}" == "1" ]]; then echo "scratch kept: $TMPHOME"; else rm -rf -- "$TMPHOME" 2>/dev/null; fi
    exit "$rc"
}
trap cleanup EXIT

export HOME="$TMPHOME"
export XDG_CONFIG_HOME="$TMPHOME/.config"
export XDG_STATE_HOME="$TMPHOME/.local/state"
export XDG_CACHE_HOME="$TMPHOME/.cache"
export XDG_DATA_HOME="$TMPHOME/.local/share"
export XDG_RUNTIME_DIR="$TMPHOME/run"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
unset WAYLAND_DISPLAY
# No GPU here: force KWin onto its software (QPainter) compositor and any
# GL onto llvmpipe so nothing needs a real DRM device.
export KWIN_COMPOSE=Q
export LIBGL_ALWAYS_SOFTWARE=1
# Look like a real Plasma session so apps load KDE colours/icons/decorations.
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
export KDE_SESSION_VERSION=6
export KDE_FULL_SESSION=true
export QT_QPA_PLATFORMTHEME=KDE

# --- high-res Xvfb --------------------------------------------------------
DISPLAY_NUM=""
for n in $(seq 90 120); do
    if [[ ! -e "/tmp/.X${n}-lock" && ! -e "/tmp/.X11-unix/X${n}" ]]; then DISPLAY_NUM=":$n"; break; fi
done
[[ -n "$DISPLAY_NUM" ]] || { echo "error: no free X display" >&2; exit 2; }
Xvfb "$DISPLAY_NUM" -screen 0 "${PW}x${PH}x24" -nolisten tcp >"$LOGDIR/xvfb.log" 2>&1 &
XVFB_PID=$!
export DISPLAY="$DISPLAY_NUM"
for _ in $(seq 1 50); do xdotool getdisplaygeometry >/dev/null 2>&1 && break; sleep 0.1; done

eval "$(dbus-launch --sh-syntax)"
export DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID

# --- tmux session kmux will attach to (control mode) ----------------------
tmux -S "$SOCKET" new-session -d -s "$SESSION" -x 250 -y 64
tmux -S "$SOCKET" set -g status off
tmux -S "$SOCKET" set -g pane-border-status off

# Pre-seed a clean look: no menu bar, and crucially no "Size: COLS x ROWS"
# resize hint (it flashes on every split/tab/layout change). kmux is a
# Konsole fork — write both component rc names so whichever it reads wins.
for rc in konsolerc kmuxrc; do
    cat >"$XDG_CONFIG_HOME/$rc" <<'RC'
[KonsoleWindow]
ShowMenuBarByDefault=false
ShowTerminalSizeHint=false

[MainWindow]
MenuBar=Disabled
ToolBarsMovable=Disabled

[Toolbar mainToolBar]
Hidden=true
RC
done

# Breeze Dark palette + icons so chrome/text/icons have real contrast
# (bare KWin with no scheme is why they blended into the background).
cp /usr/share/color-schemes/BreezeDark.colors "$XDG_CONFIG_HOME/kdeglobals"
cat >>"$XDG_CONFIG_HOME/kdeglobals" <<'KG'

[General]
ColorScheme=BreezeDark

[Icons]
Theme=breeze-dark

[KDE]
widgetStyle=Breeze
KG

# KWin: Breeze decorations on, and force the kmux window to a large
# centred size so it's framed by the Plasma desktop rather than tiny.
cat >"$XDG_CONFIG_HOME/kwinrc" <<'KW'
[org.kde.kdecoration2]
library=org.kde.breeze
theme=Breeze
KW
cat >"$XDG_CONFIG_HOME/kwinrulesrc" <<'KR'
[General]
count=1
rules=1

[1]
Description=kmux demo
title=kmux
titlematch=2
size=1640,940
sizerule=2
position=140,70
positionrule=2
noborder=false
noborderrule=2
KR

# Backup if the KWin rule misses: a large default terminal grid (HiDPI
# shrinks the default, which also broke the 3-way split for lack of room).
for rc in konsolerc kmuxrc; do
    cat >>"$XDG_CONFIG_HOME/$rc" <<'DP'

[Desktop Entry]
DefaultProfile=Demo.profile
DP
done
for d in "$XDG_DATA_HOME/konsole" "$XDG_DATA_HOME/kmux"; do
    mkdir -p "$d"
    cat >"$d/Demo.profile" <<'PROF'
[General]
Name=Demo
Parent=FALLBACK/
TerminalColumns=185
TerminalRows=48
PROF
done

# kmux launcher KWin will exec once its Wayland socket is up. Wrapper keeps
# the wayland-platform env scoped to kmux (not KWin itself).
LAUNCHER="$TMPHOME/launch-kmux.sh"
cat >"$LAUNCHER" <<EOF
#!/usr/bin/env bash
# Plasma shell gives the wallpaper + panel; kmux runs as a decorated
# Wayland client (QT_WAYLAND_DISABLE_WINDOWDECORATION lets KWin draw the
# Breeze server-side titlebar instead of Qt's own).
plasmashell >"$LOGDIR/plasmashell.log" 2>&1 &
sleep 2
exec env QT_QPA_PLATFORM=wayland QT_WAYLAND_DISABLE_WINDOWDECORATION=1 \
    "$KMUX_BIN" -S "$SOCKET" -s "$SESSION"
EOF
chmod +x "$LAUNCHER"

# --- nest KWin on the Xvfb, let it launch kmux ----------------------------
kwin_wayland --x11-display "$DISPLAY" --width "$W" --height "$H" --scale "$SCALE" \
    --no-lockscreen --no-global-shortcuts --no-kactivities \
    "$LAUNCHER" >"$LOGDIR/kwin.log" 2>&1 &
KWIN_PID=$!

# KWin's host window on the Xvfb — pin it to the origin so the capture and
# the compositor output line up exactly (no outer WM to place it).
KWIN_WIN=""
for _ in $(seq 1 150); do
    kill -0 "$KWIN_PID" 2>/dev/null || { echo "error: kwin exited early (see $LOGDIR/kwin.log)" >&2; exit 1; }
    KWIN_WIN=$(xdotool search --onlyvisible --name "KWin" 2>/dev/null | tail -1 || true)
    [[ -z "$KWIN_WIN" ]] && KWIN_WIN=$(xdotool search --onlyvisible --class kwin 2>/dev/null | tail -1 || true)
    [[ -n "$KWIN_WIN" ]] && break
    sleep 0.2
done
if [[ -n "$KWIN_WIN" ]]; then
    xdotool windowmove --sync "$KWIN_WIN" 0 0 2>/dev/null || true
fi

# kmux is up once it has attached to tmux as a control client.
attached=0
for _ in $(seq 1 150); do
    if [[ "$(tmux -S "$SOCKET" list-clients -t "$SESSION" 2>/dev/null | wc -l)" -ge 1 ]]; then
        attached=1; break
    fi
    kill -0 "$KWIN_PID" 2>/dev/null || { echo "error: kwin/kmux died (see $LOGDIR/kwin.log)" >&2; exit 1; }
    sleep 0.2
done
[[ "$attached" == "1" ]] || { echo "error: kmux never attached to tmux (see $LOGDIR/kwin.log)" >&2; exit 1; }
sleep 8   # let plasmashell paint the desktop + the control-mode handshake settle

# --- record ---------------------------------------------------------------
# 4K HiDPI software capture is heavy; ultrafast keeps the live encode from
# dropping frames. crf 21 keeps the master sharp at a sane size.
ffmpeg -y -f x11grab -draw_mouse 0 -video_size "${PW}x${PH}" -framerate 20 \
    -i "$DISPLAY" -codec:v libx264 -preset ultrafast -crf 21 -pix_fmt yuv420p \
    "$MP4" >"$LOGDIR/ffmpeg.log" 2>&1 &
FF_PID=$!
sleep 1.5

# --- choreography: tmux server-side; kmux mirrors it ----------------------
tgt() { tmux -S "$SOCKET" "$@"; }
type_in() { tgt send-keys -t "$1" "$2" Enter; }

# Beat 1 — one pane, in the kmux repo
type_in "$SESSION:0.0" "cd '$REPO' && clear && git -c color.ui=always log --oneline --graph -18"
sleep 4

# Beat 2 — split right -> native side-by-side split
tgt split-window -h -t "$SESSION:0.0"
sleep 1
type_in "$SESSION:0.1" "top -d 1"
sleep 4

# Beat 3 — split down -> 3 native panes
tgt split-window -v -t "$SESSION:0.1"
sleep 1
type_in "$SESSION:0.2" "cd '$REPO' && git -c color.ui=always status -sb && echo && ls src/tmux"
sleep 4

# Beat 4 — new tmux window -> new native kmux tab
tgt new-window -t "$SESSION"
sleep 1
type_in "$SESSION:1.0" "cd '$REPO' && clear && git -c color.ui=always show --stat HEAD"
sleep 4

# Beat 5 — back to tab 1; hold on the 3-pane layout (the money shot).
# (No select-layout here: the forced re-tile was the last thing still
# flashing Konsole's per-pane size hint.)
tgt select-window -t "$SESSION:0"
sleep 5

# --- stop + transcode -----------------------------------------------------
kill -INT "$FF_PID" 2>/dev/null
wait "$FF_PID" 2>/dev/null
FF_PID=""
[[ -s "$MP4" ]] || { echo "error: no mp4 produced (see $LOGDIR/ffmpeg.log)" >&2; exit 1; }

echo "OK"
echo "mp4: $MP4 ($(du -h "$MP4" | cut -f1))"
