#!/usr/bin/env bash
#
# editor_wasm_lane.sh - the Bazel phases of the Editor WASM workflow's build job.
#
# prefetch resolves the same graph as build, so only repository downloads can be retried.
# build runs hosted package, pixel, size, link and browser qualification in one Bazel invocation.
# prove-lockfile SNAPSHOT retains the source-stamped candidate's update/error-mode lockfile proof.
# quarantine runs the report-only scheduled Firefox cases through their own Bazel target.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${script_dir}/../.."

usage() {
  echo "Usage: tools/ci/editor_wasm_lane.sh prefetch | build | prove-lockfile SNAPSHOT | quarantine" >&2
  sed -n '3,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

# shellcheck source=tools/ci/lane.sh
source "tools/ci/lane.sh"

source_args=()
if [[ "${GITHUB_SHA+x}" == x ]]; then
  if [[ ! "${GITHUB_SHA}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "GITHUB_SHA must be a full lowercase source commit" >&2
    exit 2
  fi
  source_args=("--//build_defs:release_source_commit=${GITHUB_SHA}")
fi

graph_roots=(//tools/ci:hosted_editor_wasm_graph //tools/ci:hosted_editor_wasm_package)
budget_suite=//tools/ci:hosted_wasm_budgets
case "${PAYLOAD_BUDGET_MODE:-enforce}" in
  enforce) ;;
  report) budget_suite+=_report ;;
  *) echo "PAYLOAD_BUDGET_MODE must be enforce or report" >&2; exit 2 ;;
esac

graph_roots+=("$budget_suite")

# Only a proved browser-only PR subset uses these roots. A full build remains
# the producer of source-stamped artifacts and their complete browser receipt.
if [[ -n "${HOSTED_EDITOR_BROWSER_TARGETS:-}" ]]; then
  if [[ "${GITHUB_EVENT_NAME:-}" != "pull_request" ]]; then
    echo "Hosted browser subsets are only supported on pull requests." >&2
    exit 2
  fi
  python3 tools/ci/hosted_editor_selection.py --validate-browser-targets
  read -r -a graph_roots <<< "$HOSTED_EDITOR_BROWSER_TARGETS"
fi

prefetch() {
  lane_begin "Editor Wasm graph prefetch"
  lane_phase "Fetch hosted Editor Wasm graph" bazelisk fetch \
    ${source_args[@]+"${source_args[@]}"} "${graph_roots[@]}"
}

graph_test() {
  local phase="$1" bep="$2"
  shift 2
  lane_phase "$phase" bazelisk test ${source_args[@]+"${source_args[@]}"} \
    --test_output=errors --keep_going --test_keep_going --nobuild_tests_only \
    --noskip_incompatible_explicit_targets --test_tag_filters= --flaky_test_attempts=1 \
    --local_resources=local_gpu=1 --zip_undeclared_test_outputs \
    --remote_download_outputs=toplevel '--remote_download_regex=.*(test[.]log|test[.]xml|outputs[.]zip)$' \
    --nobuild_event_json_file_path_conversion --build_event_json_file="$bep" "$@"
}

build() {
  lane_begin "Hosted Editor Wasm graph"
  graph_test "Qualify editor package and browsers" "${RUNNER_TEMP}/editor-wasm-graph.bep.json" \
    "${graph_roots[@]}"
}

quarantine() {
  lane_begin "Scheduled Firefox quarantine report"
  graph_test "Run report-only Firefox project" "${RUNNER_TEMP}/editor-wasm-quarantine.bep.json" \
    --nocache_test_results //donner/editor/wasm/tests:hosted_firefox_quarantine_report
}

# The repository ignores MODULE.bazel.lock, so the builds before this resolved one in update mode.
# Rebuilding the package in error mode proves that generated lockfile is complete for the package
# configuration. Later steps may resolve more modules, so the proved lockfile is snapshotted here;
# the deployment candidate retains that snapshot and its digest, as the release CLIs do.
prove_lockfile() {
  local snapshot="$1"
  lane_begin "Prove the package's resolved module graph"
  lane_phase "Build package (lockfile update mode)" \
    bazelisk build --config=editor-wasm ${source_args[@]+"${source_args[@]}"} --lockfile_mode=update //tools/ci:editor_wasm_package
  lane_phase "Build package (lockfile error mode)" \
    bazelisk build --config=editor-wasm ${source_args[@]+"${source_args[@]}"} --lockfile_mode=error //tools/ci:editor_wasm_package
  lane_phase "Check generated lockfile" \
    python3 -B -m tools.release_cli check-lockfile --lockfile MODULE.bazel.lock
  lane_phase "Snapshot proved lockfile" cp MODULE.bazel.lock "${snapshot}"
}

[[ $# -ge 1 ]] || usage
command="$1"
shift
case "${command}" in
  prefetch | build | quarantine)
    [[ $# -eq 0 ]] || usage
    "${command//-/_}"
    ;;
  prove-lockfile)
    [[ $# -eq 1 && -n "$1" ]] || usage
    prove_lockfile "$1"
    ;;
  *) usage ;;
esac
