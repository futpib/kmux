#!/usr/bin/env bash
# Classify the cumulative upstream delta without adding upstream objects to the
# caller's repository. actions-template-sync mistakes any locally available
# source-tip object for a commit that is already part of the target history.

set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "usage: $0 TARGET_REPOSITORY TARGET_REVISION UPSTREAM_REPOSITORY UPSTREAM_REVISION" >&2
    exit 2
fi

target_repository=$1
target_revision=$2
upstream_repository=$3
upstream_revision=$4
temporary_root=${RUNNER_TEMP:-${TMPDIR:-/tmp}}
preflight_repository=$(mktemp -d "$temporary_root/kmux-upstream-preflight.XXXXXX")

cleanup() {
    rm -rf -- "$preflight_repository"
}
trap cleanup EXIT

target_ref=refs/kmux-preflight/target
upstream_ref=refs/kmux-preflight/upstream

git -C "$preflight_repository" init --bare --quiet
git -C "$preflight_repository" fetch --quiet --no-tags --force \
    "$target_repository" "$target_revision:$target_ref"
git -C "$preflight_repository" fetch --quiet --no-tags --force \
    "$upstream_repository" "$upstream_revision:$upstream_ref"

if ! merge_base=$(git -C "$preflight_repository" merge-base "$target_ref" "$upstream_ref"); then
    echo "No common history with upstream; a sync is required." >&2
    echo true
    exit 0
fi

mapfile -t pending_paths < <(
    git -C "$preflight_repository" diff --name-only "$merge_base" "$upstream_ref" --
)

echo "Pending upstream paths:" >&2
printf '  %s\n' "${pending_paths[@]}" >&2

if [[ ${#pending_paths[@]} -eq 0 ]]; then
    echo "Already synchronized with upstream." >&2
    echo false
    exit 0
fi

path_has_only_ignorable_metadata_changes() {
    local path=$1
    local line content
    local saw_changed_line=false

    case "$path" in
        po/*)
            return 0
            ;;
        desktop/*.desktop)
            while IFS= read -r line; do
                case "$line" in
                    '+++ '*|'--- '*)
                        continue
                        ;;
                    '+'*|'-'*)
                        saw_changed_line=true
                        content=${line:1}
                        # KDE's translation sync adds locale-qualified keys
                        # such as Name[nn], Comment[he], and Keywords[ia].
                        if [[ ! $content =~ ^[[:space:]]*[[:alnum:]_-]+\[[^]]+\]= ]]; then
                            return 1
                        fi
                        ;;
                esac
            done < <(
                git -C "$preflight_repository" diff --no-ext-diff --unified=0 \
                    "$merge_base" "$upstream_ref" -- "$path"
            )
            ;;
        desktop/*.appdata.xml)
            while IFS= read -r line; do
                case "$line" in
                    '+++ '*|'--- '*)
                        continue
                        ;;
                    '+'*|'-'*)
                        saw_changed_line=true
                        content=${line:1}
                        # AppStream's release list is publication metadata. Be
                        # deliberately strict so descriptions, launchables,
                        # requirements, and other product metadata still sync.
                        if [[ ! $content =~ ^[[:space:]]*\<release[[:space:]]+version=\"[^\"]+\"[[:space:]]+date=\"[0-9]{4}-[0-9]{2}-[0-9]{2}\"[[:space:]]*/\>[[:space:]]*$ \
                            && ! $content =~ ^[[:space:]]*\<release[[:space:]]+date=\"[0-9]{4}-[0-9]{2}-[0-9]{2}\"[[:space:]]+version=\"[^\"]+\"[[:space:]]*/\>[[:space:]]*$ ]]; then
                            return 1
                        fi
                        ;;
                esac
            done < <(
                git -C "$preflight_repository" diff --no-ext-diff --unified=0 \
                    "$merge_base" "$upstream_ref" -- "$path"
            )
            ;;
        *)
            return 1
            ;;
    esac

    [[ $saw_changed_line == true ]]
}

for path in "${pending_paths[@]}"; do
    if ! path_has_only_ignorable_metadata_changes "$path"; then
        echo "Sync-relevant changes found in $path; a sync is required." >&2
        echo true
        exit 0
    fi
done

echo "Only localization or release-list metadata changes found; skipping the sync PR." >&2
echo false
