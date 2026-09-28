/// @file
/// Compare the standalone browser renderer's captured canvas through the shared golden helper.

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <utility>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

namespace donner::editor::tests {
namespace {

TEST(StandaloneGeodeBrowserPngCompare, CanvasMatchesGolden) {
  const char* actualPath = std::getenv("DONNER_ACTUAL_PNG");
  const char* goldenPath = std::getenv("DONNER_GOLDEN_PNG");
  ASSERT_NE(actualPath, nullptr) << "DONNER_ACTUAL_PNG must name the captured canvas";
  ASSERT_NE(goldenPath, nullptr) << "DONNER_GOLDEN_PNG must name the committed reference";

  auto actual = svg::RendererImageTestUtils::readRgbaImageFromPngFile(actualPath);
  ASSERT_TRUE(actual.has_value()) << "could not decode captured canvas PNG: " << actualPath;
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(actual->width, actual->height);
  bitmap.rowBytes = actual->strideInPixels * 4u;
  bitmap.pixels = std::move(actual->data);
  bitmap.alphaType = svg::AlphaType::Unpremultiplied;
  BitmapGoldenCompareParams params = PixelmatchIdentityParams();
  if (const char* mode = std::getenv("DONNER_BROWSER_COMPARE_MODE"); mode != nullptr) {
    if (std::strcmp(mode, "overlay") != 0) {
      FAIL() << "unknown browser bitmap comparison mode";
    }
    // One channel differs by one in a retained browser overlay capture across runners.
    // Keep zero mismatched pixels and include every edge pixel.
    params = ApprovedPixelToleranceParams(0.004f, 0, true);
  }
  CompareBitmapToGolden(bitmap, goldenPath, "browser_pixel_golden", params);
}

}  // namespace
}  // namespace donner::editor::tests
