#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "expected the transitioned editor window test executable" >&2
  exit 2
fi
if ! command -v xvfb-run >/dev/null || ! command -v Xvfb >/dev/null ||
   ! command -v xauth >/dev/null; then
  echo "Xvfb and xauth are required for native Vulkan window tests" >&2
  exit 1
fi
icd="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
if [[ ! -f "$icd" ]]; then
  echo "the selected Vulkan ICD is required for native window tests" >&2
  exit 1
fi

export DONNER_GPU_BACKEND=vulkan
export VK_ICD_FILENAMES="$icd"
export XDG_RUNTIME_DIR="${TEST_TMPDIR:-/tmp}"

filter='*EditorWindowBackendTest.OpensOnTheBackendTheProcessSelected/WindowSurface'
filter+=':EditorWindowTest.NativeVulkanWindowsRetainGlfwUntilTheLastWindowCloses'
filter+=':EditorWindowDeathTest.UnprovenNativeRetirementQuarantinesTheWindowAndGlfwClaim'
filter+=':*EditorWindowLifecycleTest.AResizedWindowDrawsAtItsNewExtent/WindowSurface'
filter+=':EditorWindowTest.AMinimizedWindowSkipsTheFrameWithoutHoldingOneOpen'

exec xvfb-run -a -s '-screen 0 1280x720x24' "$1" "--gtest_filter=$filter"
