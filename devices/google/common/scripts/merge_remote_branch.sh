#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only

FROM_BRANCH=
REMOTE="partner"
FILTER="^private/"
BUG_ID=
ABORT_BRANCH=
BY_COMMIT=
DRY_RUN=
REPO_BRANCH=

function error_out() {
  echo "ERROR: $@" >&2
  usage >&2
  exit 1
}

function usage() {
  cat <<EOF
usage: $0 [OPTIONS] [REMOTE/]FROM_BRANCH
       $0 [OPTIONS] --abort --repo-branch REPO_BRANCH

Merge a remote branch. If REMOTE is not given, "partner" is used.

OPTIONS:
  --abort                       - Abort the in progress merge and abandon the local REPO_BRANCH.
  -b, --bug-id BUG_ID           - Add BUG_ID to the commit message.
  --by-commit                   - Merge by commit.
  --dry-run                     - Dry run.
  -r, --regex REGEX             - Filter the project list based on regex or wildcard
                                  matching of strings. Default: ^private/
  --repo-branch REPO_BRANCH     - Use the specific repo branch. Also can be used to continue or
                                  abort an unsuccessful merge.
  -h, --help                    - Show this help message and exit

EXAMPLES:
  $0 mirror-android14-gs-pixel-6.1
  $0 partner/mirror-android14-gs-pixel-6.1 -r ^private/devices/google/common$ -b 12345678
  $0 --abort --repo-branch merge-12345678
EOF
}

function parse_args() {
  while (( "$#" > 0 )); do
    case "$1" in
      --abort)
        ABORT=1
        ;;
      -b|--bug-id)
        BUG_ID="$2"
        shift
        ;;
      --by-commit)
        BY_COMMIT=1
        ;;
      --dry-run)
        DRY_RUN=1
        ;;
      -r|--regex)
        FILTER="$2"
        shift
        ;;
      -h|--help)
        usage
        exit
        ;;
      -*)
        error_out "Unknown option: $1"
        ;;
      *)
        FROM_BRANCH="$1"
        ;;
    esac
    shift
  done

  if [[ -n "${ABORT}" && -z "${REPO_BRANCH}" ]]; then
    error_out "--repo-branch is required for --abort"
  fi

  if [[ -z "${REPO_BRANCH}" ]]; then
    REPO_BRANCH="merge-$(date +%s)"
  fi

  if [[ -z "${FROM_BRANCH}" && -z "${ABORT}" ]]; then
    error_out "FROM_BRANCH is required"
  fi

  if [[ "${FROM_BRANCH}" == */* ]]; then
    REMOTE="${FROM_BRANCH%%/*}"
    FROM_BRANCH="${FROM_BRANCH#*/}"
  fi
}

readonly RETVALS=(
  MERGE_SUCCESS
  MERGE_FAILURE
  MERGE_CONFLICT
  MERGE_NOTHING
  MERGE_NOSOURCE
  ABORT_SUCCESS
  ABORT_FAILURE
  ABORT_NOTHING
)

function enum() {
  local -i i=0
  for x in "$@"; do
    eval "readonly ${x}=${i}"
    (( i++ ))
  done
}
enum "${RETVALS[@]}"

function abort() {
  local proj="$1"
  (
    cd "${proj}"

    if ! git rev-parse --verify -q "${ABORT_BRANCH}" >/dev/null; then
      return "${ABORT_NOTHING}"
    fi

    if [[ -n "${DRY_RUN}" ]]; then
      return "${ABORT_SUCCESS}"
    fi

    git merge --abort 2>/dev/null

    if ! repo abandon "${ABORT_BRANCH}" .; then
      return "${ABORT_FAILURE}"
    fi

    return "${ABORT_SUCCESS}"
  )
}

function merge() {
  local proj="$1"
  (
    cd "${proj}"

    if ! git fetch "${REMOTE}" "${FROM_BRANCH}"; then
      echo "${proj}: Cannot fetch ${REMOTE}/${FROM_BRANCH}, skipped." >&2
      return "${MERGE_NOSOURCE}"
    fi

    if git merge-base --is-ancestor "${REMOTE}/${FROM_BRANCH}" HEAD; then
      echo "${proj}: Nothing to merge." >&2
      return "${MERGE_NOTHING}"
    fi

    local -a commits_to_merge
    if [[ -n "${BY_COMMIT}" ]]; then
      commits_to_merge=($(git rev-list --first-parent --reverse \
        "$(git merge-base "${REMOTE}/${FROM_BRANCH}" HEAD)".."${REMOTE}/${FROM_BRANCH}"))
    else
      commits_to_merge=($(git rev-parse "${REMOTE}/${FROM_BRANCH}"))
    fi

    local commit_id

    if [[ -n "${DRY_RUN}" ]]; then
      for commit_id in ${commits_to_merge[@]}; do
        echo "${proj}: Dry run merging $(git show -s --oneline --no-decorate ${commit_id})"
      done
      return "${MERGE_SUCCESS}"
    fi

    repo start --head "${REPO_BRANCH}" .

    for commit_id in "${commits_to_merge[@]}"; do
      local msg_file="$(mktemp)"

      if [[ -n "${BY_COMMIT}" ]]; then
        echo "Merge \"$(git show -s --format='%s' "${commit_id}")\"" >> "${msg_file}"
      else
        local to_branch="$(git rev-parse --abbrev-ref HEAD@{upstream} | cut -d '/' -f2)"
        echo "$(git rev-parse ${REMOTE}/${FROM_BRANCH})"$'\t\t'"${FROM_BRANCH}" \
          | git fmt-merge-msg --log --into-name "${to_branch}" > "${msg_file}"
        sed -i '/^#.*/d' "${msg_file}"
      fi

      echo >> "${msg_file}"

      if [[ -n "${BUG_ID}" ]]; then
        echo "Bug: ${BUG_ID}" >> "${msg_file}"
      fi

      if ! git merge --no-ff --signoff --file "${msg_file}" "${commit_id}"; then
        if git status | grep -q "You have unmerged paths"; then
          echo "${proj}: Merge conflict!" >&2
          return "${MERGE_CONFLICT}"
        fi
        echo "${proj}: Merge failed." >&2
        return "${MERGE_FAILURE}"
      fi
    done

    return "${MERGE_SUCCESS}"
  )
}

function check() {
  local proj="$1"
  (
    cd "${proj}"
    if [[ -n "$(git status --porcelain)" ]]; then
      echo "ERROR: ${proj} is not clean" >&2
      return 1
    fi
    return 0
  )
}

function show_projects() {
  local msg="$1"
  local projects="$2"
  for proj in ${projects}; do
    echo "${msg}: ${proj}"
  done
}

function main() {
  parse_args "$@"

  local projects=($(repo list -p -r "${FILTER}"))
  local pids=()

  local proj
  if [[ -n "${ABORT_BRANCH}" ]]; then
    for proj in "${projects[@]}"; do
      abort "${proj}" & pids+=("$!")
    done
  else
    local check_failed=
    for proj in "${projects[@]}"; do
      if ! check "${proj}"; then
        check_failed=1
      fi
    done
    if [[ -n "${check_failed}" ]]; then
      exit 1
    fi
    for proj in "${projects[@]}"; do
      merge "${proj}" & pids+=("$!")
    done
  fi

  local -a result_groups
  local i
  for i in "${!projects[@]}"; do
    wait "${pids[i]}"
    result_groups[$?]+="${projects[i]}"$'\n'
  done

  echo
  echo "Results:"
  show_projects "SKIPPED (No source branch)"            "${result_groups[MERGE_NOSOURCE]}"
  show_projects "SKIPPED (Nothing to merge)"            "${result_groups[MERGE_NOTHING]}"
  show_projects "MERGED"                                "${result_groups[MERGE_SUCCESS]}"
  show_projects "CONFLICT"                              "${result_groups[MERGE_CONFLICT]}"
  show_projects "FAILED"                                "${result_groups[MERGE_FAILURE]}"
  show_projects "SKIPPED (No merge in progress)"        "${result_groups[ABORT_NOTHING]}"
  show_projects "ABORTED"                               "${result_groups[ABORT_SUCCESS]}"
  show_projects "FAILED"                                "${result_groups[ABORT_FAILURE]}"
  echo
  if [[ -n "${ABORT_BRANCH}" ]]; then
    echo "Abandon branch:"
    echo "${ABORT_BRANCH}"
  else
    echo "Repo branch:"
    echo "${REPO_BRANCH}"
  fi
}

main "$@"
