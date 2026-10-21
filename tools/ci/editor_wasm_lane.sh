#!/usr/bin/env bash
#
# editor_wasm_lane.sh - the Bazel phases of the Editor WASM workflow's build job.
#
# Each subcommand is one workflow step, so CI and a contributor run the same commands:
#
#   tools/ci/editor_wasm_lane.sh prefetch
#       Fetch the repositories every later phase needs. The workflow retries this step once,
#       so a download failure is retried but a build or test failure never is.
#   tools/ci/editor_wasm_lane.sh build
#       Run the audits, renderer pixel check and both Wasm payload inspections.
#       Numeric payload and module-complexity ceilings enforce by default.
#       PAYLOAD_BUDGET_MODE=report reports every metric without enforcing numeric ceilings.
#   tools/ci/editor_wasm_lane.sh prove-lockfile SNAPSHOT
#       Build the package with the generated MODULE.bazel.lock in update and then error mode,
#       check that lockfile, and copy it to SNAPSHOT for the deployment candidate.
#   tools/ci/editor_wasm_lane.sh link-bridge
#       Link both browser GPU bridge probes and the standalone Geode module, then run its
#       browser default audit.
#
# Each Bazel command is a collapsible `::group::` in the Actions log and a row in the job summary
# when $GITHUB_STEP_SUMMARY is set; the first failing command ends the lane with its exit status.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "${script_dir}/../.."

usage() {
  sed -n '3,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

# shellcheck source=tools/ci/lane.sh
source "tools/ci/lane.sh"

prefetch() {
  lane_begin "Editor WASM dependency prefetch"
  lane_phase "Fetch editor Wasm audits" \
    bazelisk fetch //tools/ci:editor_wasm_audits
  lane_phase "Fetch renderer Wasm size tests (wasm-size)" \
    bazelisk fetch --config=wasm-size //tools/ci:wasm_size_tests
  lane_phase "Fetch editor Wasm size tests (editor-wasm)" \
    bazelisk fetch --config=editor-wasm //tools/ci:editor_wasm_size_tests
  lane_phase "Fetch browser GPU bridge (wasm-geode)" \
    bazelisk fetch --config=wasm-geode \
    //tools/ci:browser_bridge_link_probes //tools/ci:geode_wasm_browser_module
}

build() {
  local budget_mode="${PAYLOAD_BUDGET_MODE:-enforce}"
  local budget_test_output="errors"
  case "${budget_mode}" in
    enforce) ;;
    report) budget_test_output=all ;;
    *) echo "PAYLOAD_BUDGET_MODE must be enforce or report" >&2; return 2 ;;
  esac
  local budget_args=("--test_arg=--payload-budget-mode=${budget_mode}")
  lane_begin "Build and size-check Geode editor Wasm package"
  lane_phase "Editor Wasm audits" \
    bazelisk test --test_output=errors //tools/ci:editor_wasm_audits
  lane_phase "Shipped browser dependency audit (editor-wasm)" \
    bazelisk test --config=editor-wasm --test_output=errors \
    //tools/ci:editor_wasm_shipped_browser_audit
  lane_phase "Renderer Wasm pixel test (wasm-size)" \
    bazelisk test --config=wasm-size --test_output=errors //donner/svg/renderer/wasm:render_test
  lane_phase "Renderer Wasm size tests (wasm-size)" \
    bazelisk test --config=wasm-size --test_output="${budget_test_output}" "${budget_args[@]}" \
    //donner/svg/renderer/wasm:wasm_size_tests
  lane_phase "Editor Wasm package size tests (editor-wasm)" \
    bazelisk test --config=editor-wasm --test_output="${budget_test_output}" --remote_download_outputs=all \
    "${budget_args[@]}" //tools/ci:editor_wasm_size_tests
}

# The repository ignores MODULE.bazel.lock, so the builds before this resolved one in update mode.
# Rebuilding the package in error mode proves that generated lockfile is complete for the package
# configuration. Later steps may resolve more modules, so the proved lockfile is snapshotted here;
# the deployment candidate retains that snapshot and its digest, as the release CLIs do.
prove_lockfile() {
  local snapshot="$1"
  lane_begin "Prove the package's resolved module graph"
  lane_phase "Build package (lockfile update mode)" \
    bazelisk build --config=editor-wasm --lockfile_mode=update //tools/ci:editor_wasm_package
  lane_phase "Build package (lockfile error mode)" \
    bazelisk build --config=editor-wasm --lockfile_mode=error //tools/ci:editor_wasm_package
  lane_phase "Check generated lockfile" \
    python3 -B -m tools.release_cli check-lockfile --lockfile MODULE.bazel.lock
  lane_phase "Snapshot proved lockfile" cp MODULE.bazel.lock "${snapshot}"
}

# Both bridge link probes resolve every JavaScript entry point, with and without threads. The
# standalone Geode module is incompatible without Browser selection, so requesting it explicitly
# fails such a configuration; the configured audit then pins its browser bridge.
link_bridge() {
  lane_begin "Link the browser GPU bridge"
  lane_phase "Link bridge probes and Geode module (wasm-geode)" \
    bazelisk build --config=wasm-geode \
    //tools/ci:browser_bridge_link_probes //tools/ci:geode_wasm_browser_module
  lane_phase "Geode browser default audit (wasm-geode)" \
    bazelisk test --config=wasm-geode --test_output=errors \
    //tools/ci:geode_wasm_browser_default_audit
}

[[ $# -ge 1 ]] || usage
command="$1"
shift
case "${command}" in
  prefetch | build | link-bridge)
    [[ $# -eq 0 ]] || usage
    "${command//-/_}"
    ;;
  prove-lockfile)
    [[ $# -eq 1 && -n "$1" ]] || usage
    prove_lockfile "$1"
    ;;
  *) usage ;;
esac
