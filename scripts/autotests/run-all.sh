#!/usr/bin/env bash
# Run every kmux bash autotest in this directory.
#
# Exit contract for each test and this runner:
#   0  pass
#   1  assertion/product failure
#   2  harness/infrastructure failure
#   77 optional skip
#
# Skips fail the aggregate run unless KMUX_TEST_ALLOW_SKIPS=1 is set. This
# keeps missing CI coverage from turning the job green accidentally. The
# runner forwards USE_XVFB / KEEP_LOGS and other KMUX_TEST_* knobs.

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

shopt -s nullglob
tests=("$SCRIPT_DIR"/test-*.sh)

if [[ ${#tests[@]} -eq 0 ]]; then
    echo "no test-*.sh found in $SCRIPT_DIR" >&2
    exit 2
fi

passed=()
failed=()
infrastructure=()
skipped=()
timed_out=()

# Per-test wall-clock budget: any single autotest that exceeds this is killed.
# Without it a hang in one test (e.g. a Qt cleanup that ignores SIGTERM) runs
# to GitHub's 6h job limit. Override via KMUX_TEST_TIMEOUT for slow boxes.
TIMEOUT="${KMUX_TEST_TIMEOUT:-300}"

for t in "${tests[@]}"; do
    name=$(basename "$t" .sh)
    echo "================ $name ================"
    set +e
    timeout --kill-after=10s "$TIMEOUT" bash "$t"
    rc=$?
    set -e
    case "$rc" in
        0)   echo "---- $name: PASS"; passed+=("$name") ;;
        2)   echo "---- $name: INFRASTRUCTURE ($rc)"; infrastructure+=("$name") ;;
        77)  echo "---- $name: SKIP ($rc)"; skipped+=("$name") ;;
        124|137) echo "---- $name: TIMEOUT (${TIMEOUT}s, rc=$rc)"; timed_out+=("$name") ;;
        *)   echo "---- $name: FAIL ($rc)"; failed+=("$name") ;;
    esac
done

echo
echo "Summary: ${#passed[@]} passed, ${#failed[@]} failed, ${#timed_out[@]} timeout, ${#infrastructure[@]} infrastructure, ${#skipped[@]} skipped"
[[ ${#passed[@]}       -gt 0 ]] && echo "  PASS: ${passed[*]}"
[[ ${#failed[@]}       -gt 0 ]] && echo "  FAIL: ${failed[*]}"
[[ ${#timed_out[@]}    -gt 0 ]] && echo "  TIMEOUT: ${timed_out[*]}"
[[ ${#infrastructure[@]} -gt 0 ]] && echo "  INFRASTRUCTURE: ${infrastructure[*]}"
[[ ${#skipped[@]}      -gt 0 ]] && echo "  SKIP: ${skipped[*]}"

if [[ ${#failed[@]} -gt 0 || ${#timed_out[@]} -gt 0 ]]; then
    exit 1
fi
if [[ ${#infrastructure[@]} -gt 0 ]]; then
    exit 2
fi
if [[ ${#skipped[@]} -gt 0 && "${KMUX_TEST_ALLOW_SKIPS:-0}" != "1" ]]; then
    echo "optional skips are not allowed; set KMUX_TEST_ALLOW_SKIPS=1 to permit them" >&2
    exit 2
fi
exit 0
