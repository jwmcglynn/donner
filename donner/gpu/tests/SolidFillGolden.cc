#include "donner/gpu/tests/SolidFillGolden.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/tests/LiteralFillGoldenTolerance.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

namespace donner::gpu::tests {

void ExpectMatchesSolidFillGolden(const svg::RendererBitmap& render, std::string_view testLabel) {
  svg::RendererBitmap straight = render;
  if (straight.alphaType == svg::AlphaType::Premultiplied) {
    straight.pixels = svg::UnpremultiplyRgbaRows(render.pixels, render.dimensions.x,
                                                 render.dimensions.y, render.rowBytes);
    straight.rowBytes = static_cast<std::size_t>(render.dimensions.x) * 4u;
    straight.alphaType = svg::AlphaType::Unpremultiplied;
  }

  std::optional<svg::Image> golden = svg::RendererImageTestUtils::readRgbaImageFromPngFile(
      std::string(kSolidFillGoldenPath).c_str());
  ASSERT_TRUE(golden.has_value()) << "cannot read " << kSolidFillGoldenPath;
  svg::RendererBitmap expected;
  expected.dimensions = Vector2i(golden->width, golden->height);
  expected.rowBytes = golden->strideInPixels * 4u;
  expected.pixels = std::move(golden->data);
  expected.alphaType = svg::AlphaType::Unpremultiplied;

  editor::tests::CompareBitmapToBitmap(straight, expected, testLabel,
                                       editor::tests::ApprovedPixelToleranceParams(
                                           svg::tests::kLiteralFillGoldenThreshold,
                                           svg::tests::kLiteralFillGoldenMaxMismatchedPixels));
}

}  // namespace donner::gpu::tests
