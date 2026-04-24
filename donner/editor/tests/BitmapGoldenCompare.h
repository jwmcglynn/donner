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
/// changes — the same convention the renderer tests use, so the
/// workflow stays consistent across suites:
///
///   UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) \
///     bazel run //donner/editor/tests:composited_promote_replay_tests

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::tests {

struct BitmapGoldenCompareParams {
  /// Per-pixel L∞ threshold (0.0 – 1.0). 0.02 matches the renderer
  /// suite's default; composited-render drift vs. a golden captured
  /// on the same machine is typically <1% on AA edges.
  float threshold = 0.02f;
  /// Max number of pixels allowed to exceed `threshold` before the
  /// comparison fails.
  int maxMismatchedPixels = 100;
  /// When true, count AA pixels as mismatches (off by default, same
  /// as `ImageComparisonTestFixture`).
  bool includeAntiAliasing = false;
};

/// Compare `bitmap` to the PNG at `goldenPath`.
///
/// Adds gtest failures (via `ADD_FAILURE`) on size mismatch or too
/// many divergent pixels. When `UPDATE_GOLDEN_IMAGES_DIR` is set in
/// the environment, writes the current bitmap to
/// `<UPDATE_GOLDEN_IMAGES_DIR>/<goldenPath>` and returns without
/// comparing — matches the `ImageComparisonTestFixture` workflow.
///
/// `testLabel` appears in log output to distinguish checkpoints. On
/// mismatch, writes an `actual_<goldenPath>` + `diff_<goldenPath>` to
/// `$TEST_UNDECLARED_OUTPUTS_DIR` (or `/tmp`) so CI + local runs can
/// inspect the divergence.
void CompareBitmapToGolden(const svg::RendererBitmap& bitmap, std::string_view goldenPath,
                           std::string_view testLabel,
                           const BitmapGoldenCompareParams& params = {});

/// Compare two live bitmaps with pixelmatch. Use when the "ground truth"
/// comes from running another code path (e.g. a direct SVG re-rasterize)
/// rather than a committed PNG — the same threshold semantics + per-
/// channel diff, but no `UPDATE_GOLDEN_IMAGES_DIR` path because there's
/// no on-disk golden to regenerate.
///
/// On mismatch, writes `actual_<testLabel>.png`, `expected_<testLabel>.png`,
/// and `diff_<testLabel>.png` to `$TEST_UNDECLARED_OUTPUTS_DIR` (or
/// `/tmp`) so a failing replay can be inspected immediately.
void CompareBitmapToBitmap(const svg::RendererBitmap& actual,
                           const svg::RendererBitmap& expected, std::string_view testLabel,
                           const BitmapGoldenCompareParams& params = {});

}  // namespace donner::editor::tests
