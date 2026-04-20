#!/usr/bin/env bash
# Run any command against the locally-built Mesa in ./mesa-prefix/.
#
# Must be run AFTER `./build_patched_mesa.sh`.
#
# Usage:
#     tools/mesa_repro/run_with_patched_mesa.sh [--driver=lvp|intel] <command ...>
#
# Examples:
#     tools/mesa_repro/run_with_patched_mesa.sh \
#         bazel-bin/tools/mesa_repro/lvp_compute_churn 500
#
#     tools/mesa_repro/run_with_patched_mesa.sh --driver=intel \
#         bazel test --config=geode \
#             //donner/svg/renderer/tests:renderer_geode_tests
#
# The wrapper sets:
#   LD_LIBRARY_PATH   → patched Mesa's lib/ dir (overrides system Mesa)
#   VK_ICD_FILENAMES  → patched Mesa's lavapipe OR intel ICD JSON
#
# It does NOT unset VK_LAYER_PATH or anything else — the Vulkan validation
# layers installed on the host will still load, which helps catch wgpu-
# native misuse even under the local Mesa.

set -euo pipefail

DRIVER="lvp"
POSITIONAL=()

for arg in "$@"; do
  case "$arg" in
    --driver=lvp|--driver=llvmpipe)  DRIVER="lvp"   ; shift ;;
    --driver=intel|--driver=anv)     DRIVER="intel" ; shift ;;
    --)                               shift ; break ;;
    --*)
      echo "unknown flag: $arg" >&2
      echo "usage: $0 [--driver=lvp|intel] <command ...>" >&2
      exit 2
      ;;
    *) break ;;  # stop flag parsing at first non-flag arg
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX_DIR="${SCRIPT_DIR}/mesa-prefix"

if [[ ! -d "${PREFIX_DIR}" ]]; then
  echo "!! ${PREFIX_DIR} not found — did you run build_patched_mesa.sh?" >&2
  exit 1
fi

# Pick the ICD JSON by driver. Mesa installs them with an arch suffix
# (lvp_icd.x86_64.json or lvp_icd.aarch64.json) so glob rather than hard-code.
ICD_JSON=""
case "${DRIVER}" in
  lvp)
    ICD_JSON="$(ls "${PREFIX_DIR}"/share/vulkan/icd.d/lvp_icd.*.json 2>/dev/null || true)"
    ;;
  intel)
    ICD_JSON="$(ls "${PREFIX_DIR}"/share/vulkan/icd.d/intel_icd.*.json 2>/dev/null || true)"
    ;;
esac
if [[ -z "${ICD_JSON}" ]]; then
  echo "!! No ${DRIVER} ICD JSON found in ${PREFIX_DIR}/share/vulkan/icd.d/" >&2
  exit 1
fi

LIB_DIR="${PREFIX_DIR}/lib"

if [[ $# -eq 0 ]]; then
  echo "usage: $0 [--driver=lvp|intel] <command ...>" >&2
  exit 2
fi

# Some glibc versions abort on `corrupted double-linked list` but let the
# process hang in the abort-time backtrace. `MALLOC_CHECK_=2` makes glibc
# abort(3) immediately on heap corruption instead of trying to continue.
# Uncomment locally if the repro hangs instead of crashing.
#export MALLOC_CHECK_=2

echo "==> Using patched Mesa at ${PREFIX_DIR}" >&2
echo "    Driver:  ${DRIVER} (${ICD_JSON})" >&2
echo "    Command: $*" >&2

exec env \
  LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  VK_ICD_FILENAMES="${ICD_JSON}" \
  "$@"
