# Shared helpers for kmux bash autotests. Source this from any test script;
# then call `kmux_test_setup` to get an isolated HOME, optional Xvfb, and
# an EXIT trap that tears everything down. Tests can register their processes,
# tmux sockets, and custom cleanup callbacks without replacing that trap.
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
#   KMUX_TEST_POLL_INTERVAL — polling interval used by wait helpers (default .1s)
#
# Exit conventions match the runner: 0 pass, 1 assertion failed, 2 harness or
# infrastructure broken, and 77 an explicitly optional skip.

# shellcheck shell=bash
# Public result globals are consumed by callers, and wait predicates are
# intentionally invoked indirectly by kmux_test_wait_until.
# shellcheck disable=SC2034,SC2329

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "error: lib.sh is meant to be sourced, not executed" >&2
    exit 2
fi

_KMUX_TEST_CLEANUPS=()
_KMUX_TEST_PIDS=()
_KMUX_TEST_TMUX_SOCKETS=()
_KMUX_TEST_CLEANING_UP=0
KMUX_TEST_PID=""
KMUX_TEST_WINDOW_ID=""
KMUX_TEST_EXIT_STATUS=""
KMUX_TEST_DBUS_SERVICE=""

# Terminal outcome helpers. The message is optional so tests can emit detailed
# diagnostics first, then terminate with a named status instead of a magic
# number. Predicate helpers should continue to use return, not these functions.
kmux_test_pass() {
    (( $# == 0 )) || printf 'PASS: %s\n' "$*"
    exit 0
}

kmux_test_fail() {
    (( $# == 0 )) || printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

kmux_test_infra() {
    (( $# == 0 )) || printf 'INFRASTRUCTURE: %s\n' "$*" >&2
    exit 2
}

kmux_test_skip() {
    (( $# == 0 )) || printf 'SKIP: %s\n' "$*" >&2
    exit 77
}

kmux_test_add_cleanup() {
    local callback=$1
    if ! declare -F "$callback" >/dev/null; then
        kmux_test_infra "cleanup callback is not a function: $callback"
    fi
    _KMUX_TEST_CLEANUPS+=("$callback")
}

kmux_test_register_pid() {
    local pid=$1
    [[ "$pid" =~ ^[0-9]+$ ]] || kmux_test_infra "invalid process id: $pid"
    _KMUX_TEST_PIDS+=("$pid")
}

kmux_test_register_tmux_socket() {
    local socket=$1
    [[ -n "$socket" ]] || kmux_test_infra "cannot register an empty tmux socket path"
    _KMUX_TEST_TMUX_SOCKETS+=("$socket")
}

kmux_test__stop_pid() {
    local pid=$1
    kill -CONT "$pid" 2>/dev/null || true
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

kmux_test__cleanup() {
    local rc=$?
    if (( _KMUX_TEST_CLEANING_UP )); then
        exit "$rc"
    fi
    _KMUX_TEST_CLEANING_UP=1
    trap - EXIT
    set +e

    local i
    for ((i = ${#_KMUX_TEST_CLEANUPS[@]} - 1; i >= 0; i--)); do
        "${_KMUX_TEST_CLEANUPS[$i]}" "$rc"
    done
    for ((i = ${#_KMUX_TEST_PIDS[@]} - 1; i >= 0; i--)); do
        kmux_test__stop_pid "${_KMUX_TEST_PIDS[$i]}"
    done
    for ((i = ${#_KMUX_TEST_TMUX_SOCKETS[@]} - 1; i >= 0; i--)); do
        if [[ -e "${_KMUX_TEST_TMUX_SOCKETS[$i]}" ]] && command -v tmux >/dev/null; then
            tmux -S "${_KMUX_TEST_TMUX_SOCKETS[$i]}" kill-server 2>/dev/null || true
        fi
    done
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
    pkill -P $$ 2>/dev/null || true
    if [[ "${KEEP_LOGS:-0}" == "1" ]]; then
        echo "logs kept in ${LOGDIR:-<unset>}"
    else
        [[ -n "${HOMEDIR:-}" ]] && rm -rf "$HOMEDIR"
    fi
    exit "$rc"
}

kmux_test_start_process() {
    local logfile=$1
    shift
    (( $# > 0 )) || kmux_test_infra "kmux_test_start_process requires a command"
    "$@" >"$logfile" 2>&1 &
    KMUX_TEST_PID=$!
    kmux_test_register_pid "$KMUX_TEST_PID"
}

kmux_test_start() {
    local logfile=$1
    shift
    kmux_test_start_process "$logfile" "$KMUX" "$@"
}

kmux_test_wait_until() {
    local timeout=$1
    local description=$2
    shift 2
    [[ "$timeout" =~ ^[0-9]+$ ]] || kmux_test_infra "invalid wait timeout: $timeout"
    (( $# > 0 )) || kmux_test_infra "kmux_test_wait_until requires a predicate"

    local started=$SECONDS
    while (( SECONDS - started < timeout )); do
        if "$@"; then
            return 0
        fi
        sleep "${KMUX_TEST_POLL_INTERVAL:-0.1}"
    done
    echo "FAIL: timed out after ${timeout}s waiting for $description" >&2
    return 1
}

kmux_test_dump_log() {
    local logfile=$1
    local lines=${2:-80}
    echo "--- $logfile (last $lines lines) ---" >&2
    tail -n "$lines" "$logfile" >&2 2>/dev/null || true
}

kmux_test_dump_tmux() {
    local socket=$1
    local session=${2:-}
    echo "--- tmux clients ---" >&2
    tmux -S "$socket" list-clients \
        -F 'client=#{client_name} pid=#{client_pid} flags=#{client_flags} size=#{client_width}x#{client_height}' >&2 2>&1 || true
    echo "--- tmux windows ---" >&2
    if [[ -n "$session" ]]; then
        tmux -S "$socket" list-windows -t "$session" \
            -F 'window=#{window_id}:#{window_index} size=#{window_width}x#{window_height} active=#{window_active}' >&2 2>&1 || true
        echo "--- tmux panes ---" >&2
        tmux -S "$socket" list-panes -s -t "$session" \
            -F 'pane=#{pane_id} window=#{window_id} geom=#{pane_left},#{pane_top}+#{pane_width}x#{pane_height}' >&2 2>&1 || true
    else
        tmux -S "$socket" list-windows \
            -F 'window=#{window_id}:#{window_index} size=#{window_width}x#{window_height} active=#{window_active}' >&2 2>&1 || true
    fi
}

kmux_test__find_visible_window() {
    local excluded=${1:-}
    local candidate=""
    local window
    while IFS= read -r window; do
        [[ -n "$window" && "$window" != "$excluded" ]] && candidate=$window
    done < <(xdotool search --onlyvisible --class kmux 2>/dev/null || true)
    [[ -n "$candidate" ]] || return 1
    printf '%s\n' "$candidate"
}

kmux_test_wait_visible_window() {
    local pid=$1
    local logfile=$2
    local timeout=${3:-20}
    local excluded=${4:-}
    [[ "$timeout" =~ ^[0-9]+$ ]] || kmux_test_infra "invalid window wait timeout: $timeout"

    KMUX_TEST_WINDOW_ID=""
    local started=$SECONDS
    while (( SECONDS - started < timeout )); do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null || true
            echo "FAIL: process $pid exited before its kmux window appeared" >&2
            kmux_test_dump_log "$logfile" 80
            return 1
        fi
        KMUX_TEST_WINDOW_ID=$(kmux_test__find_visible_window "$excluded" || true)
        [[ -n "$KMUX_TEST_WINDOW_ID" ]] && return 0
        sleep "${KMUX_TEST_POLL_INTERVAL:-0.1}"
    done
    echo "FAIL: no visible kmux window appeared within ${timeout}s" >&2
    kmux_test_dump_log "$logfile" 80
    return 1
}

kmux_test_wait_process_exit() {
    local pid=$1
    local timeout=$2
    [[ "$timeout" =~ ^[0-9]+$ ]] || kmux_test_infra "invalid process wait timeout: $timeout"

    KMUX_TEST_EXIT_STATUS=""
    local started=$SECONDS
    while (( SECONDS - started < timeout )); do
        if ! kill -0 "$pid" 2>/dev/null; then
            if wait "$pid"; then
                KMUX_TEST_EXIT_STATUS=0
            else
                KMUX_TEST_EXIT_STATUS=$?
            fi
            return 0
        fi
        sleep "${KMUX_TEST_POLL_INTERVAL:-0.1}"
    done
    return 1
}

kmux_test_parse_dbus_strings() {
    sed -n 's/^[[:space:]]*string "\(.*\)"$/\1/p'
}

kmux_test_dbus_children() {
    local service=$1
    local parent=$2
    dbus-send --session --print-reply --dest="$service" "$parent" \
        org.freedesktop.DBus.Introspectable.Introspect 2>/dev/null \
        | sed -n 's/.*<node name="\([^"]*\)".*/\1/p'
}

kmux_test_wait_dbus_service() {
    local pid=$1
    local timeout=${2:-5}
    local names fallback
    KMUX_TEST_DBUS_SERVICE=""

    dbus_service_registered() {
        names=$(dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
            org.freedesktop.DBus.ListNames 2>/dev/null | kmux_test_parse_dbus_strings)
        if grep -qx "org.kde.kmux-${pid}" <<<"$names"; then
            KMUX_TEST_DBUS_SERVICE="org.kde.kmux-${pid}"
            return 0
        fi
        fallback=$(grep -E '^org\.kde\.kmux(-[0-9]+)?$' <<<"$names" | head -1 || true)
        if [[ -n "$fallback" ]]; then
            KMUX_TEST_DBUS_SERVICE=$fallback
            return 0
        fi
        return 1
    }

    kmux_test_wait_until "$timeout" "kmux D-Bus service to register" dbus_service_registered
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
# dirs, starts Xvfb+dbus if USE_XVFB=1, verifies declared tools, and installs
# the library-owned EXIT trap. Options:
#   --x11            require xdotool and force Qt onto the X11 backend
#   --wm             start twm under Xvfb (implies --x11)
#   --require <tool> require an executable before creating the environment
kmux_test_setup() {
    local need_wm=${KMUX_TEST_NEED_WM:-0}
    local force_x11=0
    local -a required=()
    while (( $# > 0 )); do
        case "$1" in
            --wm)
                need_wm=1
                force_x11=1
                required+=(xdotool)
                shift
                ;;
            --x11)
                force_x11=1
                required+=(xdotool)
                shift
                ;;
            --require)
                (( $# >= 2 )) || kmux_test_infra "--require needs a tool name"
                required+=("$2")
                shift 2
                ;;
            *)
                kmux_test_infra "unknown kmux_test_setup option: $1"
                ;;
        esac
    done

    local repo_root
    repo_root=$(kmux_test__repo_root) || kmux_test_infra "could not locate repo root"
    KMUX="$repo_root/build/bin/kmux"
    if [[ ! -x "$KMUX" ]]; then
        kmux_test_infra "$KMUX missing — build with: cmake --build build --target kmux"
    fi

    if [[ "${USE_XVFB:-0}" == "1" ]]; then
        required+=(Xvfb dbus-launch)
    fi
    if [[ "$need_wm" == "1" ]]; then
        force_x11=1
        required+=(xdotool)
        if [[ "${USE_XVFB:-0}" == "1" ]]; then
            required+=(twm)
        fi
    fi
    for t in "${required[@]}"; do
        command -v "$t" >/dev/null || kmux_test_infra "$t not installed"
    done
    if [[ "${USE_XVFB:-0}" != "1" && -z "${DISPLAY:-}" ]]; then
        kmux_test_infra "no \$DISPLAY and USE_XVFB=0 — run on a live X session or export USE_XVFB=1"
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

    trap kmux_test__cleanup EXIT
    _KMUX_TEST_XVFB_PID=""
    _KMUX_TEST_WM_PID=""

    if [[ "${USE_XVFB:-0}" == "1" || "$force_x11" == "1" ]]; then
        unset WAYLAND_DISPLAY
        export QT_QPA_PLATFORM=xcb
    fi

    if [[ "${USE_XVFB:-0}" == "1" ]]; then
        local display_file="$HOMEDIR/xvfb-display"
        : >"$display_file"
        Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp >"$LOGDIR/xvfb.log" 2>&1 3>"$display_file" &
        _KMUX_TEST_XVFB_PID=$!
        local display_number=""
        local started=$SECONDS
        while (( SECONDS - started < 10 )); do
            if [[ -s "$display_file" ]]; then
                IFS= read -r display_number <"$display_file" || true
                [[ "$display_number" =~ ^[0-9]+$ ]] && break
            fi
            if ! kill -0 "$_KMUX_TEST_XVFB_PID" 2>/dev/null; then
                break
            fi
            sleep 0.05
        done
        if ! [[ "$display_number" =~ ^[0-9]+$ ]]; then
            echo "FAIL: Xvfb did not allocate a display within 10s" >&2
            echo "--- xvfb.log ---" >&2
            cat "$LOGDIR/xvfb.log" >&2 2>/dev/null || true
            kmux_test_infra "Xvfb startup timed out"
        fi
        export DISPLAY=":$display_number"

        eval "$(dbus-launch --sh-syntax)"
        export DBUS_SESSION_BUS_ADDRESS
        export DBUS_SESSION_BUS_PID

        if [[ "$need_wm" == "1" ]]; then
            # Stock twm config asks the user to manually place every new
            # top-level window, and grabs the X server while waiting. That
            # grab blocks every other client — including xdotool, which is
            # how every test interacts with kmux — until someone clicks.
            # Headless tests have no clicker, so the grab persists forever
            # and xdotool hangs. RandomPlacement makes twm pick a spot and
            # skip the grab entirely. NoGrabServer is belt-and-suspenders
            # for window-management operations that would otherwise grab.
            local twmrc="$HOMEDIR/.twmrc"
            cat >"$twmrc" <<'TWMRC_EOF'
RandomPlacement
NoGrabServer
TWMRC_EOF
            twm -display "$DISPLAY" -f "$twmrc" >"$LOGDIR/twm.log" 2>&1 &
            _KMUX_TEST_WM_PID=$!
            # Give twm a beat to claim the root window before any test
            # windows map; without this, early kmux windows can come up
            # override-redirect and miss focus events.
            sleep 0.3
        fi
    fi
}
