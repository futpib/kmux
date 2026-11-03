#!/usr/bin/env bash
# Run the nested prefix regression with the control connection over --rsh.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
export KMUX_TEST_PREFIX_RSH=1
exec bash "$SCRIPT_DIR/test-prefix-through-ssh-remote-tmux.sh"
