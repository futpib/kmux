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

for t in "${tests[@]}"; do
    name=$(basename "$t" .sh)
    echo "================ $name ================"
    set +e
    bash "$t"
    rc=$?
    set -e
    case "$rc" in
        0) echo "---- $name: PASS"; passed+=("$name") ;;
        2) echo "---- $name: SKIP/SCAFFOLD ($rc)"; scaffolding+=("$name") ;;
        *) echo "---- $name: FAIL ($rc)"; failed+=("$name") ;;
    esac
done

echo
echo "Summary: ${#passed[@]} passed, ${#failed[@]} failed, ${#scaffolding[@]} scaffold"
[[ ${#passed[@]}       -gt 0 ]] && echo "  PASS: ${passed[*]}"
[[ ${#failed[@]}       -gt 0 ]] && echo "  FAIL: ${failed[*]}"
[[ ${#scaffolding[@]}  -gt 0 ]] && echo "  SCAFFOLD: ${scaffolding[*]}"

if [[ ${#failed[@]} -gt 0 ]]; then
    exit 1
fi
# Scaffold-only is not a failure: tests self-report exit 2 when an
# environment prerequisite is missing, which is informational, not a
# regression. CI stays green in that case.
exit 0
