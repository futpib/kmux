#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 failed|resolved RUN_URL" >&2
    exit 2
fi

outcome=$1
run_url=$2
issue_title="Upstream sync from KDE/konsole failed"

case "$outcome" in
    failed | resolved) ;;
    *)
        echo "unknown upstream-sync outcome: $outcome" >&2
        exit 2
        ;;
esac

if [[ -z ${GITHUB_REPOSITORY:-} ]]; then
    echo "GITHUB_REPOSITORY is required" >&2
    exit 2
fi

mapfile -t open_issues < <(
    gh api --method GET \
        "repos/${GITHUB_REPOSITORY}/issues?state=open&labels=upstream-sync&per_page=100" \
        --jq '
            .[]
            | select(has("pull_request") | not)
            | select(.user.login == "github-actions[bot]")
            | select(.title == "Upstream sync from KDE/konsole failed")
            | [.number, .created_at]
            | @tsv
        ' \
        | sort -t $'\t' -k2,2 -k1,1n
)

if [[ $outcome == failed ]]; then
    issue_body="The scheduled upstream sync is failing. See the latest failed [run]($run_url)."

    if [[ ${#open_issues[@]} -eq 0 ]]; then
        gh issue create \
            --repo "$GITHUB_REPOSITORY" \
            --title "$issue_title" \
            --label upstream-sync \
            --body "$issue_body"
        exit 0
    fi

    canonical_issue=${open_issues[0]%%$'\t'*}
    gh issue edit "$canonical_issue" \
        --repo "$GITHUB_REPOSITORY" \
        --body "$issue_body"

    for issue in "${open_issues[@]:1}"; do
        issue_number=${issue%%$'\t'*}
        gh issue close "$issue_number" \
            --repo "$GITHUB_REPOSITORY" \
            --reason "not planned"
    done
else
    for issue in "${open_issues[@]}"; do
        issue_number=${issue%%$'\t'*}
        gh issue close "$issue_number" \
            --repo "$GITHUB_REPOSITORY" \
            --reason completed
    done
fi
