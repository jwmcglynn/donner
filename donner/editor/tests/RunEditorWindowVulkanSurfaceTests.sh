#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 && ( $# -ne 2 || $2 != default ) ]]; then
  echo "expected the transitioned editor window test executable and optional default mode" >&2
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

export VK_ICD_FILENAMES="$icd"
export XDG_RUNTIME_DIR="${TEST_TMPDIR:-/tmp}"

if [[ $# -eq 2 ]]; then
  # Each run is a fresh process so an empty request cannot inherit a cached selection from the
  # unset request. The one-case gate fails closed if gtest skips the real display path.
  unset WAYLAND_DISPLAY
  filter='EditorWindowTest.NativeVulkanDefaultWindowPresentsAndResizes'
  for mode in unset empty; do
    if [[ "$mode" == unset ]]; then
      unset DONNER_GPU_BACKEND
    else
      export DONNER_GPU_BACKEND=
    fi
    if ! output="$(xvfb-run -a -s '-screen 0 1280x720x24' "$1" "--gtest_filter=$filter" 2>&1)"; then
      printf '%s\n' "$output" >&2
      exit 1
    fi
    printf '%s\n' "$output"
    if [[ "$output" != *'[  PASSED  ] 1 test.'* || "$output" == *'[  SKIPPED ]'* ]]; then
      echo "the $mode native-default window case did not execute and pass exactly once" >&2
      exit 1
    fi
  done
  exit 0
fi

export DONNER_GPU_BACKEND=vulkan
filter='*EditorWindowBackendTest.OpensOnTheBackendTheProcessSelected/WindowSurface'
filter+=':EditorWindowTest.NativeVulkanWindowsRetainGlfwUntilTheLastWindowCloses'
filter+=':EditorWindowDeathTest.UnprovenNativeRetirementQuarantinesTheWindowAndGlfwClaim'
filter+=':*EditorWindowLifecycleTest.AResizedWindowDrawsAtItsNewExtent/WindowSurface'
filter+=':EditorWindowTest.AMinimizedWindowSkipsTheFrameWithoutHoldingOneOpen'

exec xvfb-run -a -s '-screen 0 1280x720x24' "$1" "--gtest_filter=$filter"
