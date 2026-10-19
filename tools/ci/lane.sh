# lane.sh - shared phase reporting for the tools/ci lane scripts. Source it; do not run it.
#
# A lane script runs each of its phases through `lane_phase NAME COMMAND...`. The phase gets its
# own collapsible `::group::` in the GitHub Actions log and a result row in the job summary when
# $GITHUB_STEP_SUMMARY is set (outside Actions the summary is skipped). A failing phase ends the
# lane at once with that phase's exit status, as a failing workflow step would.

# Appends lines to the job summary, if there is one.
lane_summary() {
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '%s\n' "$@" >>"${GITHUB_STEP_SUMMARY}"
  fi
}

# Starts the summary table for a lane titled $1.
lane_begin() {
  lane_summary "### $1" "" "| Phase | Result |" "| --- | --- |"
}

# Runs one phase. The command runs with errexit suspended (it is the left side of `||`), so a
# shell function used as a phase must chain its own commands with `&&`.
lane_phase() {
  local name="$1"
  shift
  echo "::group::${name}"
  local status=0
  "$@" || status=$?
  echo "::endgroup::"
  if [[ ${status} -ne 0 ]]; then
    lane_summary "| ${name} | failed (exit ${status}) |"
    echo "::error::${name} failed with exit status ${status}"
    exit "${status}"
  fi
  lane_summary "| ${name} | passed |"
}
