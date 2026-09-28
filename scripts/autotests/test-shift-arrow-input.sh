#!/usr/bin/env bash
# Verify that tab shortcuts take precedence, and unbinding them lets shifted
# horizontal arrows reach pane programs on both primary and alternate screens.
# shellcheck disable=SC2119,SC2329

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"
export QT_ASSUME_STDERR_HAS_CONSOLE=1
kmux_test_setup --wm --require tmux --require python3 --require qdbus6
export XDG_DATA_HOME="$HOMEDIR/.local/share"

SOCKET="$HOMEDIR/tmux.sock"
MODE="$HOMEDIR/screen-mode"
DETECTOR="$HOMEDIR/detector.py"
kmux_test_register_tmux_socket "$SOCKET"
printf primary > "$MODE"

# Each real pane program records raw input and switches its own screen on
# request. No input is injected into tmux directly during the assertions.
cat > "$DETECTOR" <<'PY'
import os
import select
import sys
import termios
import tty
from pathlib import Path

output = open(sys.argv[1], 'wb', buffering=0)
mode_file = Path(sys.argv[2])
ready_file = Path(sys.argv[3])
original = termios.tcgetattr(0)
tty.setraw(0)
mode = None
try:
    while True:
        requested = mode_file.read_text()
        if requested in ('primary', 'alternate') and requested != mode:
            os.write(1, b'\033[?1049h' if requested == 'alternate' else b'\033[?1049l')
            os.write(1, b'\033[?1lREADY\r\n')
            mode = requested
            ready_file.write_text(mode)
        if select.select([0], [], [], 0.05)[0]:
            data = os.read(0, 4096)
            if not data:
                break
            output.write(data)
finally:
    os.write(1, b'\033[?1049l')
    termios.tcsetattr(0, termios.TCSANOW, original)
PY

tmux -S "$SOCKET" -f /dev/null new-session -d -s arrows -x 120 -y 35 \
    "exec python3 '$DETECTOR' '$HOMEDIR/recv0' '$MODE' '$HOMEDIR/ready0'"
tmux -S "$SOCKET" new-window -d -t arrows:1 \
    "exec python3 '$DETECTOR' '$HOMEDIR/recv1' '$MODE' '$HOMEDIR/ready1'"

screen_ready() {
    local wanted=$1 alternate=$2 index
    for index in 0 1; do
        [[ -f "$HOMEDIR/ready$index" ]] || return 1
        [[ $(cat "$HOMEDIR/ready$index") == "$wanted" ]] || return 1
        [[ $(tmux -S "$SOCKET" display-message -p -t "arrows:$index" '#{alternate_on}') == "$alternate" ]] || return 1
    done
}

set_screen() {
    printf '%s' "$1" > "$MODE"
    kmux_test_wait_until 5 "both pane programs on the $1 screen" screen_ready "$1" "$2"
}

active_tab_is() {
    [[ $(qdbus6 "$KMUX_TEST_DBUS_SERVICE" /Windows/1 org.kde.konsole.Window.currentSession) == "${SESSION_IDS[$1]}" ]]
}

launch_kmux() {
    kmux_test_start "$LOGDIR/$1.log" --separate -S "$SOCKET" -s arrows --builtin-profile --qwindowgeometry 1000x650
    KMUX_PID=$KMUX_TEST_PID
    kmux_test_wait_visible_window "$KMUX_PID" "$LOGDIR/$1.log" 20 || kmux_test_infra
    WIN=$KMUX_TEST_WINDOW_ID
    kmux_test_wait_dbus_service "$KMUX_PID" || kmux_test_infra
    kmux_test_wait_until 10 "both tmux tabs to appear in kmux" tabs_ready || kmux_test_infra
    xdotool windowactivate --sync "$WIN" 2>/dev/null || true
    xdotool windowfocus --sync "$WIN"
    mapfile -t SESSION_IDS < <(qdbus6 "$KMUX_TEST_DBUS_SERVICE" /Windows/1 org.kde.konsole.Window.sessionList)
    xdotool key --clearmodifiers alt+1
    kmux_test_wait_until 5 "the first kmux tab to become active" active_tab_is 0 || kmux_test_infra
}

tabs_ready() {
    local -a sessions
    mapfile -t sessions < <(qdbus6 "$KMUX_TEST_DBUS_SERVICE" /Windows/1 org.kde.konsole.Window.sessionList 2>/dev/null)
    [[ ${#sessions[@]} == 2 ]]
}

input_matches() {
    python3 - "$HOMEDIR/recv0" "$1" <<'PY'
import sys
from pathlib import Path
sys.exit(Path(sys.argv[1]).read_bytes().hex() != sys.argv[2])
PY
}

expect_input() {
    local expected=$1
    shift
    xdotool key --clearmodifiers "$@"
    if ! kmux_test_wait_until 5 "pane input $expected" input_matches "$expected"; then
        python3 - "$HOMEDIR/recv0" <<'PY'
import sys
from pathlib import Path
print('Actual pane input:', repr(Path(sys.argv[1]).read_bytes()))
PY
        kmux_test_fail "unexpected bytes from $*"
    fi
}

set_screen primary 0
launch_kmux bound
# Prove focus and the complete GUI -> tmux -> raw pane input path first.
expect_input 1b5b44 Left

for screen in primary alternate; do
    alternate=0
    [[ $screen == primary ]] || alternate=1
    set_screen "$screen" "$alternate"
    xdotool key --clearmodifiers shift+Right
    kmux_test_wait_until 5 "Next Tab on $screen screen" active_tab_is 1 || kmux_test_fail
    xdotool key --clearmodifiers shift+Left
    kmux_test_wait_until 5 "Previous Tab on $screen screen" active_tab_is 0 || kmux_test_fail
    input_matches 1b5b44 || kmux_test_fail "bound tab shortcut leaked to pane 0"
    [[ ! -s "$HOMEDIR/recv1" ]] || kmux_test_fail "bound tab shortcut leaked to pane 1"
    echo "PASS: bound Shift+Left/Right switch tabs on the $screen screen"
done

kill "$KMUX_PID"
kmux_test_wait_process_exit "$KMUX_PID" 10 || kmux_test_infra "kmux did not exit"
# Configure Shortcuts saves these actions in the local XMLGUI file.
python3 - "$SCRIPT_DIR/../../desktop/kmuxui.rc" "$XDG_DATA_HOME/kxmlgui5/kmux/kmuxui.rc" <<'PY'
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

tree = ET.parse(sys.argv[1])
properties = ET.SubElement(tree.getroot(), 'ActionProperties', {'scheme': 'Default'})
for name in ('next-tab', 'previous-tab'):
    ET.SubElement(properties, 'Action', {'name': name, 'shortcut': ''})
destination = Path(sys.argv[2])
destination.parent.mkdir(parents=True, exist_ok=True)
tree.write(destination, encoding='utf-8', xml_declaration=True)
PY

set_screen primary 0
launch_kmux unbound
expected=1b5b44
for screen in primary alternate; do
    alternate=0
    [[ $screen == primary ]] || alternate=1
    set_screen "$screen" "$alternate"
    expected+=1b5b44
    expect_input "$expected" Left
    expected+=1b5b313b3244
    expect_input "$expected" shift+Left
    expected+=1b5b313b3243
    expect_input "$expected" shift+Right
    active_tab_is 0 || kmux_test_fail "unbound shortcut switched tabs"
    [[ ! -s "$HOMEDIR/recv1" ]] || kmux_test_fail "unbound shortcut reached the wrong pane"
    echo "PASS: unbound Shift+Left/Right reach the pane on the $screen screen"
done

kmux_test_pass "shifted arrows preserve tab shortcuts and reach pane programs when unbound"
