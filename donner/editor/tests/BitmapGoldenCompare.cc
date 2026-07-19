#include "donner/editor/tests/BitmapGoldenCompare.h"

#include <pixelmatch/pixelmatch.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/renderer/RendererImageIO.h"  // IWYU pragma: keep
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor::tests {
namespace {

std::string Flatten(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    out.push_back(c == '/' || c == '\\' ? '_' : c);
  }
  return out;
}

std::filesystem::path DiffOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

std::vector<uint8_t> BuildSideBySide(const std::vector<uint8_t>& expected, int expectedWidth,
                                     int expectedHeight, std::size_t expectedStrideInPixels,
                                     const std::vector<uint8_t>& actual, int actualWidth,
                                     int actualHeight, std::size_t actualStrideInPixels) {
  const int combinedWidth = expectedWidth + actualWidth;
  const int combinedHeight = std::max(expectedHeight, actualHeight);
  const std::size_t combinedStride = static_cast<std::size_t>(combinedWidth);
  std::vector<uint8_t> combined(combinedStride * static_cast<std::size_t>(combinedHeight) * 4u, 0u);

  for (int y = 0; y < expectedHeight; ++y) {
    const std::size_t srcOffset = static_cast<std::size_t>(y) * expectedStrideInPixels * 4u;
    uint8_t* dst = combined.data() + static_cast<std::size_t>(y) * combinedStride * 4u;
    std::memcpy(dst, expected.data() + srcOffset, static_cast<std::size_t>(expectedWidth) * 4u);
  }
  for (int y = 0; y < actualHeight; ++y) {
    const std::size_t srcOffset = static_cast<std::size_t>(y) * actualStrideInPixels * 4u;
    uint8_t* dst = combined.data() + static_cast<std::size_t>(y) * combinedStride * 4u +
                   static_cast<std::size_t>(expectedWidth) * 4u;
    std::memcpy(dst, actual.data() + srcOffset, static_cast<std::size_t>(actualWidth) * 4u);
  }
  return combined;
}

int ComparePixels(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual,
                  int width, int height, std::size_t strideInPixels,
                  const BitmapGoldenCompareParams& params, std::vector<uint8_t>& diffImage,
                  int& pixelsExceedingChannelDelta) {
  pixelsExceedingChannelDelta = 0;
  if (params.maxChannelDelta < 0) {
    pixelmatch::Options options;
    options.threshold = params.threshold;
    options.includeAA = params.includeAntiAliasing;
    return pixelmatch::pixelmatch(expected, actual, diffImage, width, height, strideInPixels,
                                  options);
  }

  int mismatched = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t pos =
          (static_cast<std::size_t>(y) * strideInPixels + static_cast<std::size_t>(x)) * 4u;
      bool differs = false;
      bool exceedsLimit = false;
      for (std::size_t channel = 0; channel < 4u; ++channel) {
        const int delta = std::abs(static_cast<int>(expected[pos + channel]) -
                                   static_cast<int>(actual[pos + channel]));
        differs = differs || delta != 0;
        exceedsLimit = exceedsLimit || delta > params.maxChannelDelta;
      }
      if (differs) {
        diffImage[pos] = 255u;
        diffImage[pos + 3u] = 255u;
        ++mismatched;
      }
      if (exceedsLimit) {
        ++pixelsExceedingChannelDelta;
      }
    }
  }
  return mismatched;
}

std::string ChannelLimitDetails(const BitmapGoldenCompareParams& params,
                                int pixelsExceedingChannelDelta) {
  if (params.maxChannelDelta < 0) {
    return {};
  }
  return "; " + std::to_string(pixelsExceedingChannelDelta) +
         " pixels exceed RGBA channel delta " + std::to_string(params.maxChannelDelta);
}

}  // namespace

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
    const std::filesystem::path outDir = DiffOutputDir();
    const std::string flat = Flatten(goldenPath);
    const auto actualPath = outDir / ("actual_" + flat);
    const auto expectedPath = outDir / ("expected_" + flat);
    const auto sideBySidePath = outDir / ("side_by_side_" + flat);
    const auto sideBySide =
        BuildSideBySide(golden.data, golden.width, golden.height, golden.strideInPixels,
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

  std::vector<uint8_t> diffImage(bitmap.pixels.size(), 0u);
  int pixelsExceedingChannelDelta = 0;
  const int mismatched = ComparePixels(golden.data, bitmap.pixels, width, height, strideInPixels,
                                       params, diffImage, pixelsExceedingChannelDelta);

  if (mismatched > params.maxMismatchedPixels || pixelsExceedingChannelDelta > 0) {
    const std::filesystem::path outDir = DiffOutputDir();
    const std::string flat = Flatten(goldenPath);
    const auto actualPath = outDir / ("actual_" + flat);
    const auto expectedPath = outDir / ("expected_" + flat);
    const auto diffPath = outDir / ("diff_" + flat);
    const auto sideBySidePath = outDir / ("side_by_side_" + flat);
    const auto sideBySide = BuildSideBySide(golden.data, width, height, strideInPixels,
                                            bitmap.pixels, width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), bitmap.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), golden.data,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(diffPath.string().c_str(), diffImage, width,
                                                   height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(sideBySidePath.string().c_str(), sideBySide,
                                                   width * 2, height, strideInPixels * 2u);
    ADD_FAILURE() << "[" << testLabel << "] " << mismatched
                  << " pixels differ (max allowed: " << params.maxMismatchedPixels
                  << ChannelLimitDetails(params, pixelsExceedingChannelDelta)
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

void CompareBitmapToBitmap(const svg::RendererBitmap& actual, const svg::RendererBitmap& expected,
                           std::string_view testLabel, const BitmapGoldenCompareParams& params) {
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
    const std::filesystem::path outDir = DiffOutputDir();
    const std::string flat = Flatten(testLabel);
    const auto actualPath = outDir / ("actual_" + flat + ".png");
    const auto expectedPath = outDir / ("expected_" + flat + ".png");
    const auto sideBySidePath = outDir / ("side_by_side_" + flat + ".png");
    const auto sideBySide =
        BuildSideBySide(expected.pixels, expectedWidth, expectedHeight, expectedStrideInPixels,
                        actual.pixels, width, height, strideInPixels);
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
  int pixelsExceedingChannelDelta = 0;
  const int mismatched = ComparePixels(expected.pixels, actual.pixels, width, height, strideInPixels,
                                       params, diffImage, pixelsExceedingChannelDelta);

  if (mismatched > params.maxMismatchedPixels || pixelsExceedingChannelDelta > 0) {
    const std::filesystem::path outDir = DiffOutputDir();
    const std::string flat = Flatten(testLabel);
    const auto actualPath = outDir / ("actual_" + flat + ".png");
    const auto expectedPath = outDir / ("expected_" + flat + ".png");
    const auto diffPath = outDir / ("diff_" + flat + ".png");
    const auto sideBySidePath = outDir / ("side_by_side_" + flat + ".png");
    const auto sideBySide =
        BuildSideBySide(expected.pixels, expectedWidth, expectedHeight, expectedStrideInPixels,
                        actual.pixels, width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), actual.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), expected.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(diffPath.string().c_str(), diffImage, width,
                                                   height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(sideBySidePath.string().c_str(), sideBySide,
                                                   width * 2, height, strideInPixels * 2u);
    ADD_FAILURE() << "[" << testLabel << "] " << mismatched
                  << " pixels differ (max allowed: " << params.maxMismatchedPixels
                  << ChannelLimitDetails(params, pixelsExceedingChannelDelta)
                  << "). Actual: " << actualPath.string() << ". Expected: " << expectedPath.string()
                  << ". Diff: " << diffPath.string()
                  << ". Side-by-side: " << sideBySidePath.string();
  } else {
    std::fprintf(stderr, "[%s] PASS bitmap-to-bitmap (%d px differ, max %d)\n",
                 std::string(testLabel).c_str(), mismatched, params.maxMismatchedPixels);
  }
}

}  // namespace donner::editor::tests
