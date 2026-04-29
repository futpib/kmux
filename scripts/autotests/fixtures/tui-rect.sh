#!/usr/bin/env bash
# TUI fixture: fills the entire pane with a rectangle whose Nth row is filled
# with the Nth letter of the alphabet (wrapping at Z back to a). Refreshes
# every 100ms and re-reads $LINES/$COLUMNS on SIGWINCH.
#
# A line-wrap bug in kmux's tmux integration (e.g. kmux's display columns
# disagreeing with tmux's pane_width) would manifest as a row's character
# spilling onto the next row, which test-tui-no-wrap.sh asserts against.
#
# When TUI_READY_FILE is set, touches "$TUI_READY_FILE" after drawing the
# first frame so the harness knows the fixture is producing output.
#
# When TUI_SIZE_FILE is set, writes "<rows> <cols>" (the dimensions actually
# observed via stty(1) inside the pane) to that file before each draw. The
# harness reads it as authoritative — `tmux display-message #{pane_width}`
# can drift from the pty's TIOCGWINSZ in some edge cases, so the value the
# TUI itself sees is the only source of truth for what it drew.
#
# Each non-last row writes COLS characters; the last row writes COLS-1
# characters. Avoiding the bottom-right cell sidesteps the late-wrap /
# scroll behaviour that varies between terminals — leaving the rendering
# question on the test purely about line wrapping, not corner-cell scroll.

set -u

ESC=$'\033'

ROWS=24
COLS=80
update_size() {
    local size
    if size=$(stty size 2>/dev/null); then
        ROWS="${size% *}"
        COLS="${size#* }"
    fi
}

cleanup() {
    printf '%s[?25h%s[0m%s[2J%s[H' "$ESC" "$ESC" "$ESC" "$ESC" 2>/dev/null || true
}

trap update_size WINCH
trap 'cleanup; exit 0' INT TERM
trap cleanup EXIT

update_size
printf '%s[?25l%s[2J' "$ESC" "$ESC"

ready_file="${TUI_READY_FILE:-}"
size_file="${TUI_SIZE_FILE:-}"
chars=(a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W X Y Z)
nchars=${#chars[@]}

first=1
while true; do
    if (( ROWS < 2 || COLS < 2 )); then
        sleep 0.1
        continue
    fi

    if [[ -n "$size_file" ]]; then
        printf '%s %s\n' "$ROWS" "$COLS" >"$size_file" 2>/dev/null || true
    fi

    # \033[2J wipes the visible screen each frame so cells the fixture
    # doesn't itself rewrite this iteration (e.g. the bottom-right cell
    # the last row deliberately leaves blank, or content that belonged to
    # a now-truncated row after a resize-shrink) read back as default
    # fill rather than as residue from a larger previous pane size.
    out="${ESC}[2J"
    for ((row=0; row<ROWS; row++)); do
        ch="${chars[row % nchars]}"
        printf -v pad '%*s' "$COLS" ''
        line="${pad// /$ch}"
        out+="${ESC}[$((row+1));1H"
        if (( row == ROWS - 1 )); then
            out+="${line:0:COLS-1}"
        else
            out+="$line"
        fi
    done
    printf '%s' "$out"

    if [[ -n "$first" && -n "$ready_file" ]]; then
        : >"$ready_file" 2>/dev/null || true
        first=
    fi
    sleep 0.1
done
