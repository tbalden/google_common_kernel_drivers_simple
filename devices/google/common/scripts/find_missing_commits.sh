#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

REPO_DIR=
TARGET_BRANCH=
SOURCE_BRANCH=HEAD
VERBOSE=

function usage() {
  cat <<EOF
usage: $0 [OPTIONS] REPO_DIR TARGET_BRANCH [SOURCE_BRANCH]

List commits which are in SOURCE_BRANCH but the Change-Ids are not found in TARGET_BRANCH.

If SOURCE_BRANCH is not given, HEAD is used.

OPTIONS:
  -v            - Print more logs.
  -h            - Print help message.

EXAMPLES:
  $0 private/devices/google/common partner/android-gs-pixel-mainline HEAD
EOF
}

function verbose() {
  if [[ -n "${VERBOSE}" ]]; then
    echo "$@" >&2
  fi
}

function parse_args() {
  local -i argc=0
  while (( $# > 0 )); do
    case "$1" in
      -h|--help)
        usage
        exit
        ;;
      -v|--verbose)
        VERBOSE=1
        ;;
      -*)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 1
        ;;
      *)
        case "${argc}" in
          0)
            REPO_DIR="$1"
            ;;
          1)
            TARGET_BRANCH="$1"
            ;;
          2)
            SOURCE_BRANCH="$1"
            ;;
          *)
            echo "Too many arguments!" >&2
            usage >&2
            exit 1
            ;;
        esac
        argc="$(( argc + 1))"
        ;;
    esac
    shift
  done

  if [[ -z "${REPO_DIR}" || -z "${SOURCE_BRANCH}" || -z "${TARGET_BRANCH}" ]]; then
    usage >&2
    exit 1
  fi
}

function fetch_if_needed() {
  local full_branch="$1"
  if [[ "${full_branch}" == */* ]]; then
    local remote="${full_branch%%/*}"
    local branch="${full_branch#*/}"
    git fetch "${remote}" "${branch}"
  fi
}

function main() {
  parse_args "$@"

  set -e

  cd "${REPO_DIR}"
  fetch_if_needed "${SOURCE_BRANCH}"
  fetch_if_needed "${TARGET_BRANCH}"

  local commits=($(git cherry "${TARGET_BRANCH}" "${SOURCE_BRANCH}" | sed -n 's/^+ \(.*\)/\1/p'))
  local merge_base="$(git merge-base "${TARGET_BRANCH}" "${SOURCE_BRANCH}")"

  local -i missing=0
  local -i count=0
  local -i total="${#commits[@]}"
  for commit_id in "${commits[@]}"; do
    local commit_body="$(git show -s --pretty=format:%b "${commit_id}")"
    local other_commit_ids=(
      $(sed -n 's/^\[ Upstream commit \([0-9a-f]\{40\}\) \]$/\1/p' <<<"${commit_body}")
      $(sed -n 's/^commit \([0-9a-f]\{40\}\) upstream\.$/\1/p' <<<"${commit_body}")
      $(sed -n 's/^(cherry[ -]picked from commit \([0-9a-f]\{40\}\).*/\1/p' <<<"${commit_body}")
    )
    local change_ids=(
      $(sed -n 's/^Change-Id: \(I[0-9a-f]\{40\}\)$/\1/p' <<<"${commit_body}")
      $(sed -n 's/^Merged-In: \(I[0-9a-f]\{40\}\)$/\1/p' <<<"${commit_body}")
    )
    local id

    other_commit_ids=($(printf "%s\n" "${other_commit_ids[@]}" | sort -u))
    change_ids=($(printf "%s\n" "${change_ids[@]}" | sort -u))
    verbose ""
    verbose "[$(( ++count ))/${total}] $(git show -s --pretty="%H %s" "${commit_id}")"
    for id in "${other_commit_ids[@]}"; do
      verbose "  Other commit ID: ${id}"
    done
    for id in "${change_ids[@]}"; do
      verbose "  Change ID: ${id}"
    done

    local found=
    for id in "${other_commit_ids[@]}"; do
      if git merge-base --is-ancestor "${id}" "${TARGET_BRANCH}"; then
        verbose "  Found: $(git show -s --pretty="%H %s" "${id}")"
        found=1
        break
      fi
    done
    if [[ -n "${found}" ]]; then
      continue
    fi

    local greps=()
    for id in "${commit_id}" "${other_commit_ids[@]}"; do
      greps+=(--grep "^\[ Upstream commit ${id} \]$")
      greps+=(--grep "^commit ${id} upstream\.$")
      greps+=(--grep "^(cherry[ -]picked from commit ${id}")
    done

    for id in "${change_ids[@]}"; do
      greps+=(--grep "^Change-Id: ${id}$")
    done

    if [[ -n "${greps[@]}" ]]; then
      local match="$(git log -1 --pretty="%H %s" "${merge_base}..${TARGET_BRANCH}" "${greps[@]}")"
      if [[ -n "${match}" ]]; then
        verbose "  Matched: ${match}"
        continue
      fi
    fi

    verbose "  Missing"
    missing="$(( missing + 1 ))"
    git show -s --pretty="%H %s" "${commit_id}"
  done
  verbose ""
  verbose "Found ${missing} missing commits"
}

main "$@"
