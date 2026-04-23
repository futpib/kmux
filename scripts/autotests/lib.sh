# Shared helpers for kmux bash autotests. Source this from any test script;
# then call `kmux_test_setup` to get an isolated HOME, optional Xvfb, and
# an EXIT trap that tears everything down.
#
# Expected to be sourced, not executed directly.
#
# Exposed after setup:
#   $KMUX     — path to the built kmux binary
#   $HOMEDIR  — isolated $HOME used by the test
#   $LOGDIR   — $HOMEDIR/logs (kept if KEEP_LOGS=1)
#
# Environment knobs:
#   USE_XVFB=1        — start a headless Xvfb + dbus-launch instead of $DISPLAY
#   KEEP_LOGS=1       — don't wipe $HOMEDIR on exit (helpful for debugging)
#   KMUX_TEST_NEED_WM=1 — also spawn a tiny window manager (twm) under Xvfb,
#                         for tests whose xdotool keystrokes need a focused
#                         window (e.g. menu navigation)
#
# Exit conventions match the sibling scripts: 0 pass, 1 assertion failed,
# 2 scaffolding broken (missing binary, missing tool, no DISPLAY, etc.).

# shellcheck shell=bash

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "error: lib.sh is meant to be sourced, not executed" >&2
    exit 2
fi

kmux_test_bail() {
    local code=$1
    shift
    echo "error: $*" >&2
    exit "$code"
}

kmux_test__find_display() {
    for n in $(seq 90 120); do
        if [[ ! -e "/tmp/.X${n}-lock" && ! -e "/tmp/.X11-unix/X${n}" ]]; then
            echo ":$n"
            return 0
        fi
    done
    return 1
}

kmux_test__cleanup() {
    local rc=$?
    pkill -P $$ 2>/dev/null || true
    if [[ -n "${DBUS_SESSION_BUS_PID:-}" ]]; then
        kill "$DBUS_SESSION_BUS_PID" 2>/dev/null || true
    fi
    if [[ -n "${_KMUX_TEST_WM_PID:-}" ]]; then
        kill "$_KMUX_TEST_WM_PID" 2>/dev/null || true
        wait "$_KMUX_TEST_WM_PID" 2>/dev/null || true
    fi
    if [[ -n "${_KMUX_TEST_XVFB_PID:-}" ]]; then
        kill "$_KMUX_TEST_XVFB_PID" 2>/dev/null || true
        wait "$_KMUX_TEST_XVFB_PID" 2>/dev/null || true
    fi
    if [[ "${KEEP_LOGS:-0}" == "1" ]]; then
        echo "logs kept in ${LOGDIR:-<unset>}"
    else
        [[ -n "${HOMEDIR:-}" ]] && rm -rf "$HOMEDIR"
    fi
    exit "$rc"
}

# Find the repo root by walking up from this lib.sh until a .git dir
# shows up. Keeps tests relocatable without hardcoding paths.
kmux_test__repo_root() {
    local d
    d=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
    while [[ "$d" != "/" ]]; do
        if [[ -d "$d/.git" ]]; then
            echo "$d"
            return 0
        fi
        d=$(dirname "$d")
    done
    return 1
}

# Set up the standard test environment. Creates an isolated HOME with XDG
# dirs, starts Xvfb+dbus if USE_XVFB=1, verifies required tools, and
# installs an EXIT trap that cleans everything up.
#
# Every test uses xdotool; tests that need other tools should check them
# themselves (via `command -v foo || kmux_test_bail 2 ...`) after calling
# this.
kmux_test_setup() {
    local repo_root
    repo_root=$(kmux_test__repo_root) || kmux_test_bail 2 "could not locate repo root"
    KMUX="$repo_root/build/bin/kmux"
    if [[ ! -x "$KMUX" ]]; then
        kmux_test_bail 2 "$KMUX missing — build with: cmake --build build --target kmux"
    fi

    local required=(xdotool)
    if [[ "${USE_XVFB:-0}" == "1" ]]; then
        required+=(Xvfb dbus-launch xdpyinfo)
    fi
    for t in "${required[@]}"; do
        command -v "$t" >/dev/null || kmux_test_bail 2 "$t not installed"
    done
    if [[ "${USE_XVFB:-0}" != "1" && -z "${DISPLAY:-}" ]]; then
        kmux_test_bail 2 'no $DISPLAY and USE_XVFB=0 — run on a live X session or export USE_XVFB=1'
    fi

    HOMEDIR=$(mktemp -d)
    LOGDIR="$HOMEDIR/logs"
    mkdir -p "$LOGDIR"

    export HOME="$HOMEDIR"
    export XDG_CONFIG_HOME="$HOMEDIR/.config"
    export XDG_STATE_HOME="$HOMEDIR/.local/state"
    export XDG_RUNTIME_DIR="$HOMEDIR/run"
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"

    _KMUX_TEST_XVFB_PID=""
    _KMUX_TEST_WM_PID=""
    trap kmux_test__cleanup EXIT

    if [[ "${USE_XVFB:-0}" == "1" ]]; then
        local display
        display=$(kmux_test__find_display) || kmux_test_bail 2 "no free X display"
        Xvfb "$display" -screen 0 1280x720x24 -nolisten tcp >"$LOGDIR/xvfb.log" 2>&1 &
        _KMUX_TEST_XVFB_PID=$!
        export DISPLAY="$display"
        for _ in $(seq 1 50); do
            xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
            sleep 0.1
        done
        eval "$(dbus-launch --sh-syntax)"
        export DBUS_SESSION_BUS_ADDRESS
        export DBUS_SESSION_BUS_PID

        if [[ "${KMUX_TEST_NEED_WM:-0}" == "1" ]]; then
            command -v twm >/dev/null || kmux_test_bail 2 "KMUX_TEST_NEED_WM=1 but twm not installed"
            twm -display "$DISPLAY" >"$LOGDIR/twm.log" 2>&1 &
            _KMUX_TEST_WM_PID=$!
            # Give twm a beat to claim the root window before any test
            # windows map; without this, early kmux windows can come up
            # override-redirect and miss focus events.
            sleep 0.3
        fi
    fi
}
