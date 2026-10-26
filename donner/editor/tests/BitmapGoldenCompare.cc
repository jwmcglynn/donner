#include "donner/editor/tests/BitmapGoldenCompare.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapCompareCommon.h"
#include "donner/svg/renderer/RendererImageIO.h"  // IWYU pragma: keep
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "gtest/gtest.h"
#include "tools/svg_suite/ImageCompare.h"

namespace donner::editor::tests {

void CompareBitmapToGolden(const svg::RendererBitmap& bitmap, std::string_view goldenPath,
                           std::string_view testLabel, const BitmapGoldenCompareParams& params) {
  ASSERT_FALSE(bitmap.empty()) << "[" << testLabel << "] bitmap is empty";
  ASSERT_EQ(bitmap.rowBytes % 4u, 0u)
      << "[" << testLabel << "] bitmap rowBytes must be RGBA-aligned";

  const int width = bitmap.dimensions.x;
  const int height = bitmap.dimensions.y;
  const std::size_t strideInPixels = bitmap.rowBytes / 4u;
  ASSERT_EQ(bitmap.pixels.size(), bitmap.rowBytes * static_cast<std::size_t>(height))
      << "[" << testLabel << "] bitmap size is inconsistent with rowBytes × height";

  if (const char* updateDir = std::getenv("UPDATE_GOLDEN_IMAGES_DIR"); updateDir != nullptr) {
    const std::filesystem::path goldenDest =
        std::filesystem::path(updateDir) / std::string(goldenPath);
    std::filesystem::create_directories(goldenDest.parent_path());
    svg::RendererImageIO::writeRgbaPixelsToPngFile(goldenDest.string().c_str(), bitmap.pixels,
                                                   width, height, strideInPixels);
    std::fprintf(stderr, "[%s] wrote golden: %s\n", std::string(testLabel).c_str(),
                 goldenDest.string().c_str());
    return;
  }

  auto maybeGolden =
      svg::RendererImageTestUtils::readRgbaImageFromPngFile(std::string(goldenPath).c_str());
  ASSERT_TRUE(maybeGolden.has_value())
      << "[" << testLabel << "] could not load golden PNG: " << goldenPath;
  svg::Image& golden = *maybeGolden;

  if (golden.width != width || golden.height != height || golden.strideInPixels != strideInPixels ||
      golden.data.size() != bitmap.pixels.size()) {
    const std::filesystem::path outDir = detail::DiffOutputDir();
    const std::string flat = detail::Flatten(goldenPath);
    const auto actualPath = outDir / ("actual_" + flat);
    const auto expectedPath = outDir / ("expected_" + flat);
    const auto sideBySidePath = outDir / ("side_by_side_" + flat);
    const auto sideBySide =
        detail::BuildSideBySide(golden.data, golden.width, golden.height, golden.strideInPixels,
                                bitmap.pixels, width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), bitmap.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), golden.data,
                                                   golden.width, golden.height,
                                                   golden.strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(
        sideBySidePath.string().c_str(), sideBySide, golden.width + width,
        std::max(golden.height, height), golden.width + width);
    ADD_FAILURE() << "[" << testLabel << "] golden size/layout mismatch. Expected " << golden.width
                  << "x" << golden.height << " stride " << golden.strideInPixels << "; actual "
                  << width << "x" << height << " stride " << strideInPixels
                  << ". Actual: " << actualPath.string() << ". Expected: " << expectedPath.string()
                  << ". Side-by-side: " << sideBySidePath.string();
    return;
  }

  auto comparison = svg_suite::CompareImages(
      {width, height, strideInPixels, golden.data}, {width, height, strideInPixels, bitmap.pixels},
      {params.threshold, params.maxMismatchedPixels, params.includeAntiAliasing});
  ASSERT_TRUE(comparison.error.empty()) << "[" << testLabel << "] " << comparison.error;
  ASSERT_TRUE(comparison.mismatchedPixels.has_value());
  const int mismatched = *comparison.mismatchedPixels;
  const auto& diffImage = comparison.diff->data;

  if (mismatched > params.maxMismatchedPixels) {
    const std::filesystem::path outDir = detail::DiffOutputDir();
    const std::string flat = detail::Flatten(goldenPath);
    const auto actualPath = outDir / ("actual_" + flat);
    const auto expectedPath = outDir / ("expected_" + flat);
    const auto diffPath = outDir / ("diff_" + flat);
    const auto sideBySidePath = outDir / ("side_by_side_" + flat);
    const auto sideBySide = detail::BuildSideBySide(golden.data, width, height, strideInPixels,
                                                    bitmap.pixels, width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), bitmap.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), golden.data,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(diffPath.string().c_str(), diffImage, width,
                                                   height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(sideBySidePath.string().c_str(), sideBySide,
                                                   width * 2, height,
                                                   static_cast<size_t>(width) * 2u);
    ADD_FAILURE() << "[" << testLabel << "] " << mismatched
                  << " pixels differ (max allowed: " << params.maxMismatchedPixels
                  << "). Golden: " << goldenPath << ". Actual: " << actualPath.string()
                  << ". Expected: " << expectedPath.string() << ". Diff: " << diffPath.string()
                  << ". Side-by-side: " << sideBySidePath.string()
                  << ". To regenerate: UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) "
                     "bazel run <test_target>.";
  } else {
    std::fprintf(stderr, "[%s] PASS (%d px differ, max %d)\n", std::string(testLabel).c_str(),
                 mismatched, params.maxMismatchedPixels);
  }
}

svg::RendererBitmap CropBitmap(const svg::RendererBitmap& bitmap, Vector2i origin, Vector2i size) {
  svg::RendererBitmap window{size,
                             std::vector<uint8_t>(static_cast<std::size_t>(size.x) * size.y * 4),
                             static_cast<std::size_t>(size.x) * 4, bitmap.alphaType};
  if (origin.x < 0 || origin.y < 0 || origin.x + size.x > bitmap.dimensions.x ||
      origin.y + size.y > bitmap.dimensions.y) {
    ADD_FAILURE() << "crop window lies outside the " << bitmap.dimensions.x << "x"
                  << bitmap.dimensions.y << " bitmap";
    return window;
  }
  for (int y = 0; y < size.y; ++y) {
    const std::size_t source = static_cast<std::size_t>(origin.y + y) * bitmap.rowBytes +
                               static_cast<std::size_t>(origin.x) * 4;
    std::copy_n(bitmap.pixels.begin() + static_cast<std::ptrdiff_t>(source), window.rowBytes,
                window.pixels.begin() + static_cast<std::ptrdiff_t>(y * window.rowBytes));
  }
  return window;
}

}  // namespace donner::editor::tests
