#!/usr/bin/env bash
# End-to-end test for the kmux --rsh + ssh password-prompt path, exercised
# over a REAL pseudo-terminal.
#
# test-rsh-interactive.sh proves kmux defers its window while a wrapper
# blocks — but it blocks on a FIFO and reads the secret with the `read`
# builtin, explicitly avoiding a pty. So it never checks the thing that
# actually matters for ssh: that the prompt reaches the controlling
# terminal and that the password typed there is delivered to the wrapper.
# ssh reads and writes /dev/tty directly (not stdin), so this needs a real
# terminal, not a pipe.
#
# Here kmux runs under a pty (fixtures/pty-run.py: child is a session
# leader with the pty as its controlling terminal). The --rsh wrapper
# mimics ssh: it opens /dev/tty, prints a recognisable prompt there, reads
# the password from there, and only execs tmux if it matches. The pty
# driver, acting as the user, watches the terminal output and types the
# password when it sees the prompt.
#
# Asserts:
#   1. The wrapper's prompt appears on the terminal (kmux did not sever or
#      swallow the child's controlling terminal).
#   2. tmux comes up — i.e. the password typed at the terminal reached the
#      wrapper, which validated it and execed tmux, and the bridge
#      connected.
#   3. The kmux window becomes visible once tmux handshakes.
#
# By default uses $DISPLAY. Set USE_XVFB=1 for headless Xvfb.
#
# Exit 0 = prompt shown on the terminal, typed password delivered, tmux up.
# Exit 1 = assertion failed.
# Exit 2 = scaffolding failure (missing binary/tool, no DISPLAY, etc.).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=scripts/autotests/lib.sh
source "$SCRIPT_DIR/lib.sh"

kmux_test_setup

command -v tmux >/dev/null || kmux_test_bail 2 "tmux not installed"
command -v python3 >/dev/null || kmux_test_bail 2 "python3 not installed"

DRIVER="$SCRIPT_DIR/fixtures/pty-run.py"
[[ -f "$DRIVER" ]] || kmux_test_bail 2 "missing pty driver $DRIVER"

WRAPPER="$HOMEDIR/rsh-ssh-like.sh"
SOCKET="$HOMEDIR/tmux.sock"
PTY_LOG="$LOGDIR/pty.log"
PROMPT_MARKER="KMUX-PTY-PROMPT"
PASSWORD="hunter2"

# An ssh-like wrapper: talk to the controlling terminal (/dev/tty), not
# stdin/stderr, so this genuinely tests terminal plumbing. Prompt there,
# read the password there (echo off, like ssh), exec tmux only on a match.
cat >"$WRAPPER" <<WRAPPER_EOF
#!/bin/bash
set -u
if ! exec 9<>/dev/tty 2>/dev/null; then
    echo 'rsh-ssh-like: no controlling terminal (/dev/tty unavailable)' >&2
    exit 3
fi
printf '%s password for test@host: ' "$PROMPT_MARKER" >&9
stty -echo <&9 2>/dev/null || true
IFS= read -r pass <&9
stty echo <&9 2>/dev/null || true
printf '\n' >&9
exec 9>&-
if [[ "\$pass" != "$PASSWORD" ]]; then
    echo 'rsh-ssh-like: permission denied' >&2
    exit 1
fi
exec "\$@"
WRAPPER_EOF
chmod +x "$WRAPPER"

echo "=== launching kmux under a pty, --rsh=$WRAPPER -S $SOCKET ==="
# The driver types $PASSWORD once it sees $PROMPT_MARKER on the terminal.
python3 "$DRIVER" "$PROMPT_MARKER" "$PASSWORD" -- \
    "$KMUX" --rsh "$WRAPPER" -S "$SOCKET" >"$PTY_LOG" 2>&1 &
DRIVER_PID=$!

prompt_ok=0
tmux_ok=0
window_ok=0
for _ in $(seq 1 60); do
    if [[ "$prompt_ok" -eq 0 ]] && grep -qF "$PROMPT_MARKER" "$PTY_LOG" 2>/dev/null; then
        prompt_ok=1
        echo "OK: wrapper's password prompt appeared on the terminal"
    fi
    if [[ "$tmux_ok" -eq 0 ]] && tmux -S "$SOCKET" list-sessions >/dev/null 2>&1; then
        tmux_ok=1
        echo "OK: tmux session is up — the typed password reached the wrapper"
    fi
    if [[ "$window_ok" -eq 0 ]] && xdotool search --onlyvisible --class kmux >/dev/null 2>&1; then
        window_ok=1
    fi
    if [[ "$prompt_ok" -eq 1 && "$tmux_ok" -eq 1 && "$window_ok" -eq 1 ]]; then
        echo "PASS: prompt shown on the terminal, password delivered, tmux up, window shown"
        exit 0
    fi
    # If the driver died early, kmux/pty setup failed — stop waiting.
    if ! kill -0 "$DRIVER_PID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done

echo "FAIL: prompt_ok=$prompt_ok tmux_ok=$tmux_ok window_ok=$window_ok" >&2
echo "--- pty.log (terminal as the user would see it) ---" >&2
cat "$PTY_LOG" >&2 2>/dev/null || true
exit 1
