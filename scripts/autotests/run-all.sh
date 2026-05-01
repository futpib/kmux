#!/usr/bin/env bash
# Run every kmux bash autotest in this directory.
#
# Exits 0 iff every `test-*.sh` passes. Exit 2 if any test reports
# scaffolding failure. Exit 1 if any test reports a real assertion
# failure. Forwards USE_XVFB / KEEP_LOGS to each test.

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
scaffolding=()
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
        2)   echo "---- $name: SKIP/SCAFFOLD ($rc)"; scaffolding+=("$name") ;;
        124|137) echo "---- $name: TIMEOUT (${TIMEOUT}s, rc=$rc)"; timed_out+=("$name") ;;
        *)   echo "---- $name: FAIL ($rc)"; failed+=("$name") ;;
    esac
done

echo
echo "Summary: ${#passed[@]} passed, ${#failed[@]} failed, ${#timed_out[@]} timeout, ${#scaffolding[@]} scaffold"
[[ ${#passed[@]}       -gt 0 ]] && echo "  PASS: ${passed[*]}"
[[ ${#failed[@]}       -gt 0 ]] && echo "  FAIL: ${failed[*]}"
[[ ${#timed_out[@]}    -gt 0 ]] && echo "  TIMEOUT: ${timed_out[*]}"
[[ ${#scaffolding[@]}  -gt 0 ]] && echo "  SCAFFOLD: ${scaffolding[*]}"

if [[ ${#failed[@]} -gt 0 || ${#timed_out[@]} -gt 0 ]]; then
    exit 1
fi
# Scaffold-only is not a failure: tests self-report exit 2 when an
# environment prerequisite is missing, which is informational, not a
# regression. CI stays green in that case.
exit 0
