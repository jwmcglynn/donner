#pragma once
/// @file
///
/// Tiny golden-image comparison helper for editor replay tests. Takes
/// a `RendererBitmap` (the format the async renderer hands back) and
/// compares against a committed PNG on disk using `pixelmatch-cpp17`,
/// mirroring the threshold / diff conventions already in use by
/// `ImageComparisonTestFixture` in the renderer suite.
///
/// Supports the standard `UPDATE_GOLDEN_IMAGES_DIR=<workspace>` escape
/// hatch for regenerating goldens after intentional pixel-output
/// changes:
///
///   UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
///     bazel run //donner/editor/tests:rnr_replay_tests

#include <string_view>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::tests {

struct BitmapGoldenCompareParams {
  /// Per-pixel L∞ threshold (0.0 - 1.0). 0.02 matches the renderer
  /// suite's default; replay drift vs. a golden captured on the same
  /// machine is typically well below this on AA edges.
  float threshold = 0.02f;
  /// Max number of pixels allowed to exceed `threshold` before the
  /// comparison fails.
  int maxMismatchedPixels = 100;
  /// When true, count AA pixels as mismatches (off by default, same
  /// as `ImageComparisonTestFixture`).
  bool includeAntiAliasing = false;
};

/// Strict identity pixelmatch parameters for new replay regressions.
[[nodiscard]] constexpr BitmapGoldenCompareParams PixelmatchIdentityParams() {
  return BitmapGoldenCompareParams{
      .threshold = 0.0f,
      .maxMismatchedPixels = 0,
      .includeAntiAliasing = true,
  };
}

/**
 * Explicitly-approved non-identity pixelmatch parameters.
 *
 * Use only for documented exceptions where identity comparison has
 * been reviewed and rejected.
 *
 * @param threshold Per-channel pixelmatch threshold.
 * @param maxMismatchedPixels Maximum number of mismatched pixels before failure.
 * @param includeAntiAliasing Whether anti-aliased pixels count as mismatches.
 */
[[nodiscard]] constexpr BitmapGoldenCompareParams ApprovedPixelToleranceParams(
    float threshold, int maxMismatchedPixels, bool includeAntiAliasing = false) {
  return BitmapGoldenCompareParams{
      .threshold = threshold,
      .maxMismatchedPixels = maxMismatchedPixels,
      .includeAntiAliasing = includeAntiAliasing,
  };
}

/// Compare `bitmap` to the PNG at `goldenPath`.
///
/// Adds gtest failures on size mismatch or too many divergent pixels.
/// When `UPDATE_GOLDEN_IMAGES_DIR` is set in the environment, writes
/// the current bitmap to `<UPDATE_GOLDEN_IMAGES_DIR>/<goldenPath>` and
/// returns without comparing.
///
/// `testLabel` appears in log output to distinguish checkpoints. On
/// mismatch, writes `actual_...`, `expected_...`, `diff_...`, and
/// `side_by_side_...` PNGs to `$TEST_UNDECLARED_OUTPUTS_DIR` (or `/tmp`)
/// so CI + local runs can inspect the divergence immediately.
void CompareBitmapToGolden(const svg::RendererBitmap& bitmap, std::string_view goldenPath,
                           std::string_view testLabel,
                           const BitmapGoldenCompareParams& params = {});

/// Compare two live bitmaps with pixelmatch. Use when the ground truth
/// comes from running another code path rather than a committed PNG.
///
/// On mismatch, writes `actual_<testLabel>.png`, `expected_<testLabel>.png`,
/// `diff_<testLabel>.png`, and `side_by_side_<testLabel>.png` to
/// `$TEST_UNDECLARED_OUTPUTS_DIR` (or `/tmp`) so a failing replay can
/// be inspected immediately.
void CompareBitmapToBitmap(const svg::RendererBitmap& actual, const svg::RendererBitmap& expected,
                           std::string_view testLabel,
                           const BitmapGoldenCompareParams& params = {});

}  // namespace donner::editor::tests
