#!/usr/bin/env bash
# Exercise OSC 8 through a real tmux pane, mouse clicks, confirmation, and KIO.
# shellcheck disable=SC2329

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"
kmux_test_setup --wm --require tmux --require python3 --require update-desktop-database --require kbuildsycoca6
export XDG_DATA_HOME="$HOMEDIR/.local/share"
export XDG_CACHE_HOME="$HOMEDIR/.cache"
export LC_ALL=C.UTF-8
mkdir -p "$XDG_DATA_HOME/applications" "$XDG_CACHE_HOME"

SOCKET="$HOMEDIR/tmux.sock"
REQUEST="$HOMEDIR/request"
READY="$HOMEDIR/ready"
OPENED="$HOMEDIR/opened"
kmux_test_register_tmux_socket "$SOCKET"

# Record browser dispatch without contacting a site or opening the user's browser.
cat > "$HOMEDIR/browser" <<'PY'
#!/usr/bin/env python3
import pathlib
import sys
with pathlib.Path(__file__).with_name('opened').open('a') as output:
    output.write(sys.argv[1] + '\n')
PY
chmod +x "$HOMEDIR/browser"
cat > "$XDG_DATA_HOME/applications/kmux-test-browser.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=kmux test browser
Exec=$HOMEDIR/browser %u
MimeType=x-scheme-handler/http;x-scheme-handler/https;text/plain;application/x-shellscript;text/x-shellscript;
NoDisplay=true
EOF
cat > "$XDG_CONFIG_HOME/mimeapps.list" <<'EOF'
[Default Applications]
x-scheme-handler/http=kmux-test-browser.desktop
x-scheme-handler/https=kmux-test-browser.desktop
text/plain=kmux-test-browser.desktop
application/x-shellscript=kmux-test-browser.desktop
text/x-shellscript=kmux-test-browser.desktop
EOF
cat > "$XDG_CONFIG_HOME/kdeglobals" <<'EOF'
[General]
BrowserApplication=kmux-test-browser.desktop
EOF
update-desktop-database "$XDG_DATA_HOME/applications"
kbuildsycoca6 --noincremental > "$LOGDIR/sycoca.log" 2>&1
cat > "$HOMEDIR/payload.sh" <<EOF
#!/bin/sh
touch '$HOMEDIR/executed'
EOF
chmod +x "$HOMEDIR/payload.sh"

cat > "$HOMEDIR/output.py" <<'PY'
import os
import pathlib
import sys
import time
request, ready = map(pathlib.Path, sys.argv[1:])
previous = None
while True:
    mode = request.read_text() if request.exists() else ''
    if mode and mode != previous:
        url = 'https://example.org/actual'
        label = 'https://trusted.example/'
        if mode in ('literal', 'overwritten', 'concealed', 'invisible'):
            label = url
        elif mode == 'blocked':
            url, label = 'javascript:alert(1)', 'blocked link'
            os.write(1, b'\033]50;AllowEscapedLinks=true;EscapedLinksSchema=javascript://\a')
            time.sleep(0.3)
        elif mode == 'file':
            url, label = pathlib.Path(__file__).with_name('payload.sh').as_uri(), 'local script'
        columns, rows = os.get_terminal_size()
        output = '\033[2J\033[H'
        if mode == 'concealed':
            output += '\033[8m'
        elif mode == 'invisible':
            output += '\033[30;40m'
        for _ in range(rows - 1):
            output += '\033]8;;' + url + '\033\\' + label + '\033]8;;\033\\'
            if mode == 'overwritten':
                output += '\rhttps://trusted.example/'
            output += '\r\n'
        os.write(1, (output + '\033[0m').encode())
        ready.write_text(mode)
        previous = mode
    time.sleep(0.05)
PY
tmux -S "$SOCKET" -f /dev/null new-session -d -s links -x 100 -y 35 \
    "exec python3 '$HOMEDIR/output.py' '$REQUEST' '$READY'"
kmux_test_start "$LOGDIR/kmux.log" --separate -S "$SOCKET" -s links --builtin-profile \
    --hide-menubar --hide-toolbars --hide-tabbar --qwindowgeometry 900x600
KMUX_PID=$KMUX_TEST_PID
kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/kmux.log" 20 || kmux_test_infra
WIN=$KMUX_TEST_WINDOW_ID
xdotool windowactivate --sync "$WIN" 2>/dev/null || true
xdotool windowfocus --sync "$WIN"

output_ready() { [[ -f "$READY" && $(cat "$READY") == "$1" ]]; }
show_links() {
    printf '%s' "$1" > "$REQUEST"
    kmux_test_wait_until 10 "OSC 8 output for $1" output_ready "$1"
    sleep 0.3
}
click_link() {
    xdotool windowactivate --sync "$WIN" 2>/dev/null || true
    xdotool windowfocus --sync "$WIN"
    sleep 0.5
    xdotool mousemove --window "$WIN" 70 250 keydown ctrl click 1 keyup ctrl
}
dialog_visible() {
    DIALOG=$(xdotool search --all --onlyvisible --pid "$KMUX_PID" --name '^Open Terminal Link$' 2>/dev/null || true)
    [[ -n "$DIALOG" ]]
}
dialog_closed() { ! dialog_visible; }
open_count_is() { [[ -f "$OPENED" && $(wc -l < "$OPENED") -eq "$1" ]]; }
answer_dialog() {
    xdotool windowactivate --sync "$DIALOG" 2>/dev/null || true
    xdotool windowfocus --sync "$DIALOG"
    xdotool key --clearmodifiers "$1"
}

show_links hidden
click_link
kmux_test_wait_until 5 "hidden destination confirmation" dialog_visible
[[ ! -e "$OPENED" ]] || kmux_test_fail "opened before confirmation"
answer_dialog Return
if ! kmux_test_wait_until 5 "default Cancel to close confirmation" dialog_closed; then
    command -v import >/dev/null && import -window root "$LOGDIR/dialog-failure.png"
    xdotool getactivewindow getwindowname
    kmux_test_fail "default Cancel did not close confirmation"
fi
[[ ! -e "$OPENED" ]] || kmux_test_fail "default button opened the link"

click_link
kmux_test_wait_until 5 "second confirmation" dialog_visible
answer_dialog alt+o
kmux_test_wait_until 10 "confirmed browser dispatch" open_count_is 1
[[ $(cat "$OPENED") == 'https://example.org/actual' ]] || kmux_test_fail "wrong destination dispatched"

for mode in overwritten concealed invisible; do
    show_links "$mode"
    click_link
    kmux_test_wait_until 5 "$mode label confirmation" dialog_visible
    answer_dialog Escape
    kmux_test_wait_until 5 "Escape to cancel confirmation" dialog_closed
    open_count_is 1 || kmux_test_fail "$mode label bypassed confirmation"
done

show_links literal
click_link
kmux_test_wait_until 10 "literal URL dispatch without confirmation" open_count_is 2
dialog_closed || kmux_test_fail "literal URL unexpectedly required confirmation"

show_links blocked
click_link
sleep 0.5
dialog_closed || kmux_test_fail "disallowed scheme became an OSC 8 link"
open_count_is 2 || kmux_test_fail "disallowed scheme was dispatched"

show_links file
click_link
kmux_test_wait_until 5 "local file confirmation" dialog_visible
answer_dialog alt+o
kmux_test_wait_until 10 "local script opened as text" open_count_is 3
[[ ! -e "$HOMEDIR/executed" ]] || kmux_test_fail "local script was executed"
opened_file=$(tail -1 "$OPENED")
[[ "$opened_file" == "$HOMEDIR/payload.sh" || "$opened_file" == "file://$HOMEDIR/payload.sh" ]] \
    || kmux_test_fail "wrong local file dispatched: $opened_file"
kmux_test_pass "default OSC 8 links confirm hidden destinations, cancel safely, and dispatch only allowed URLs"
