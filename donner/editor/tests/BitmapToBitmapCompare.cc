#include <pixelmatch/pixelmatch.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapCompareCommon.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "gtest/gtest.h"

namespace donner::editor::tests {

void CompareBitmapToBitmap(const svg::RendererBitmap& actual, const svg::RendererBitmap& expected,
                           std::string_view testLabel, const BitmapGoldenCompareParams& params,
                           int* mismatchedPixels) {
  if (mismatchedPixels != nullptr) {
    *mismatchedPixels = -1;
  }
  ASSERT_FALSE(actual.empty()) << "[" << testLabel << "] actual bitmap is empty";
  ASSERT_FALSE(expected.empty()) << "[" << testLabel << "] expected bitmap is empty";
  ASSERT_EQ(actual.rowBytes % 4u, 0u)
      << "[" << testLabel << "] actual rowBytes must be RGBA-aligned";
  const int width = actual.dimensions.x;
  const int height = actual.dimensions.y;
  const int expectedWidth = expected.dimensions.x;
  const int expectedHeight = expected.dimensions.y;
  const std::size_t strideInPixels = actual.rowBytes / 4u;
  const std::size_t expectedStrideInPixels = expected.rowBytes / 4u;

  if (actual.dimensions != expected.dimensions) {
    const std::filesystem::path outDir = detail::DiffOutputDir();
    const std::string flat = detail::Flatten(testLabel);
    const auto actualPath = outDir / ("actual_" + flat + ".png");
    const auto expectedPath = outDir / ("expected_" + flat + ".png");
    const auto sideBySidePath = outDir / ("side_by_side_" + flat + ".png");
    const auto sideBySide = detail::BuildSideBySide(expected.pixels, expectedWidth, expectedHeight,
                                                    expectedStrideInPixels, actual.pixels, width,
                                                    height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), actual.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), expected.pixels,
                                                   expectedWidth, expectedHeight,
                                                   expectedStrideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(
        sideBySidePath.string().c_str(), sideBySide, expectedWidth + width,
        std::max(expectedHeight, height), expectedWidth + width);
    ADD_FAILURE() << "[" << testLabel << "] dimensions mismatch (actual=" << width << "x" << height
                  << ", expected=" << expectedWidth << "x" << expectedHeight
                  << "). Actual: " << actualPath.string() << ". Expected: " << expectedPath.string()
                  << ". Side-by-side: " << sideBySidePath.string();
    return;
  }
  ASSERT_EQ(expected.rowBytes, actual.rowBytes) << "[" << testLabel << "] rowBytes mismatch";

  std::vector<uint8_t> diffImage(actual.pixels.size(), 0u);
  pixelmatch::Options options;
  options.threshold = params.threshold;
  options.includeAA = params.includeAntiAliasing;
  const int mismatched = pixelmatch::pixelmatch(expected.pixels, actual.pixels, diffImage, width,
                                                height, strideInPixels, options);
  if (mismatchedPixels != nullptr) {
    *mismatchedPixels = mismatched;
  }

  if (mismatched > params.maxMismatchedPixels) {
    const std::filesystem::path outDir = detail::DiffOutputDir();
    const std::string flat = detail::Flatten(testLabel);
    const auto actualPath = outDir / ("actual_" + flat + ".png");
    const auto expectedPath = outDir / ("expected_" + flat + ".png");
    const auto diffPath = outDir / ("diff_" + flat + ".png");
    const auto sideBySidePath = outDir / ("side_by_side_" + flat + ".png");
    const auto sideBySide = detail::BuildSideBySide(expected.pixels, expectedWidth, expectedHeight,
                                                    expectedStrideInPixels, actual.pixels, width,
                                                    height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), actual.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), expected.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(diffPath.string().c_str(), diffImage, width,
                                                   height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(sideBySidePath.string().c_str(), sideBySide,
                                                   width * 2, height,
                                                   static_cast<size_t>(width) * 2u);
    ADD_FAILURE() << "[" << testLabel << "] " << mismatched
                  << " pixels differ (max allowed: " << params.maxMismatchedPixels
                  << "). Actual: " << actualPath.string() << ". Expected: " << expectedPath.string()
                  << ". Diff: " << diffPath.string()
                  << ". Side-by-side: " << sideBySidePath.string();
  } else {
    std::fprintf(stderr, "[%s] PASS bitmap-to-bitmap (%d px differ, max %d)\n",
                 std::string(testLabel).c_str(), mismatched, params.maxMismatchedPixels);
  }
}

}  // namespace donner::editor::tests
