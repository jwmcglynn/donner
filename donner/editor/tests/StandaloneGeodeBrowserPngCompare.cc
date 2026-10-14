/// @file
/// Compare PNGs captured by the browser tests through the shared pixelmatch helpers.
///
/// `DONNER_BROWSER_COMPARE_MODE` selects the comparison of `DONNER_ACTUAL_PNG`:
/// - Unset: against the committed golden `DONNER_GOLDEN_PNG`, at identity.
/// - `overlay`: a retained browser overlay capture against its committed golden
///   `DONNER_GOLDEN_PNG`, with a reviewed tolerance.
/// - `presented_canvas`: a 2D canvas readback against `DONNER_EXPECTED_PNG`, the renderer RGBA
///   that the page presented on that canvas, at identity.

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

namespace donner::editor::tests {
namespace {

std::optional<svg::RendererBitmap> ReadPng(const char* path) {
  auto image = svg::RendererImageTestUtils::readRgbaImageFromPngFile(path);
  if (!image.has_value()) {
    return std::nullopt;
  }
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(image->width, image->height);
  bitmap.rowBytes = image->strideInPixels * 4u;
  bitmap.pixels = std::move(image->data);
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  return bitmap;
}

TEST(StandaloneGeodeBrowserPngCompare, CanvasMatchesGolden) {
  const char* modeVariable = std::getenv("DONNER_BROWSER_COMPARE_MODE");
  const std::string_view mode = modeVariable != nullptr ? modeVariable : "";
  if (modeVariable != nullptr && mode != "overlay" && mode != "presented_canvas") {
    FAIL() << "unknown browser bitmap comparison mode: " << mode;
  }

  const char* actualPath = std::getenv("DONNER_ACTUAL_PNG");
  ASSERT_NE(actualPath, nullptr) << "DONNER_ACTUAL_PNG must name the captured canvas";
  const std::optional<svg::RendererBitmap> actual = ReadPng(actualPath);
  ASSERT_TRUE(actual.has_value()) << "could not decode captured canvas PNG: " << actualPath;

  if (mode == "presented_canvas") {
    const char* expectedPath = std::getenv("DONNER_EXPECTED_PNG");
    ASSERT_NE(expectedPath, nullptr)
        << "DONNER_EXPECTED_PNG must name the renderer output the canvas presents";
    const std::optional<svg::RendererBitmap> expected = ReadPng(expectedPath);
    ASSERT_TRUE(expected.has_value()) << "could not decode renderer output PNG: " << expectedPath;
    // The 2D canvas stores premultiplied alpha and getImageData() divides it out again. The
    // pinned Chromium's readback reproduces the presented RGBA exactly, so this comparison is
    // identity. A browser that rounds those conversions differently fails here: Chromium 151
    // rounded the standalone page's exact halves down and changed eight partially covered pixels
    // by one. A threshold cannot absorb such rounding in general: on that page, a truncating
    // readback needs a larger pixelmatch threshold than a one-step change to some opaque colors.
    CompareBitmapToBitmap(*actual, *expected, "presented_canvas", PixelmatchIdentityParams());
    return;
  }

  const char* goldenPath = std::getenv("DONNER_GOLDEN_PNG");
  ASSERT_NE(goldenPath, nullptr) << "DONNER_GOLDEN_PNG must name the committed reference";
  // In overlay mode, one channel differs by one in a retained browser overlay capture across
  // runners. Keep zero mismatched pixels and include every edge pixel.
  const BitmapGoldenCompareParams params = mode == "overlay"
                                               ? ApprovedPixelToleranceParams(0.004f, 0, true)
                                               : PixelmatchIdentityParams();
  CompareBitmapToGolden(*actual, goldenPath, "browser_pixel_golden", params);
}

}  // namespace
}  // namespace donner::editor::tests
