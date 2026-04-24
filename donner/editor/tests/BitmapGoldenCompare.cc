#include "donner/editor/tests/BitmapGoldenCompare.h"

#include <pixelmatch/pixelmatch.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "donner/svg/renderer/RendererImageIO.h"  // IWYU pragma: keep
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor::tests {

namespace {

// Replace path separators so we can dump `actual_…` and `diff_…`
// variants into a flat output dir without colliding with siblings.
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

}  // namespace

void CompareBitmapToGolden(const svg::RendererBitmap& bitmap, std::string_view goldenPath,
                           std::string_view testLabel, const BitmapGoldenCompareParams& params) {
  ASSERT_FALSE(bitmap.empty()) << "[" << testLabel << "] bitmap is empty — nothing to compare";
  ASSERT_EQ(bitmap.rowBytes % 4u, 0u)
      << "[" << testLabel << "] bitmap rowBytes must be a multiple of 4 (RGBA)";

  const int width = bitmap.dimensions.x;
  const int height = bitmap.dimensions.y;
  const std::size_t strideInPixels = bitmap.rowBytes / 4u;
  ASSERT_EQ(bitmap.pixels.size(), bitmap.rowBytes * static_cast<std::size_t>(height))
      << "[" << testLabel << "] bitmap pixel size inconsistent with rowBytes × height";

  // `UPDATE_GOLDEN_IMAGES_DIR=<workspace> bazel run …` regenerates the
  // golden without comparing. Matches the renderer suite's convention.
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
      << "[" << testLabel << "] could not load golden PNG: " << goldenPath
      << " — regenerate with UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run …";
  svg::Image& golden = *maybeGolden;

  ASSERT_EQ(golden.width, width) << "[" << testLabel << "] golden width mismatch";
  ASSERT_EQ(golden.height, height) << "[" << testLabel << "] golden height mismatch";
  ASSERT_EQ(golden.strideInPixels, strideInPixels)
      << "[" << testLabel << "] golden stride mismatch";
  ASSERT_EQ(golden.data.size(), bitmap.pixels.size())
      << "[" << testLabel << "] golden pixel buffer size mismatch";

  std::vector<uint8_t> diffImage(bitmap.pixels.size(), 0u);
  pixelmatch::Options options;
  options.threshold = params.threshold;
  options.includeAA = params.includeAntiAliasing;
  const int mismatched = pixelmatch::pixelmatch(golden.data, bitmap.pixels, diffImage, width,
                                                height, strideInPixels, options);

  if (mismatched > params.maxMismatchedPixels) {
    const std::filesystem::path outDir = DiffOutputDir();
    const std::string flat = Flatten(goldenPath);
    const auto actualPath = outDir / ("actual_" + flat);
    const auto diffPath = outDir / ("diff_" + flat);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), bitmap.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(diffPath.string().c_str(), diffImage, width,
                                                   height, strideInPixels);
    ADD_FAILURE() << "[" << testLabel << "] " << mismatched
                  << " pixels differ (max allowed: " << params.maxMismatchedPixels
                  << "). Golden: " << goldenPath << ". Actual: " << actualPath.string()
                  << ". Diff: " << diffPath.string()
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
      << "[" << testLabel << "] actual rowBytes must be a multiple of 4 (RGBA)";
  ASSERT_EQ(expected.rowBytes, actual.rowBytes)
      << "[" << testLabel << "] rowBytes mismatch (actual=" << actual.rowBytes
      << ", expected=" << expected.rowBytes << ")";
  ASSERT_EQ(actual.dimensions, expected.dimensions)
      << "[" << testLabel << "] dimensions mismatch (actual=" << actual.dimensions.x << "x"
      << actual.dimensions.y << ", expected=" << expected.dimensions.x << "x"
      << expected.dimensions.y << ")";

  const int width = actual.dimensions.x;
  const int height = actual.dimensions.y;
  const std::size_t strideInPixels = actual.rowBytes / 4u;

  std::vector<uint8_t> diffImage(actual.pixels.size(), 0u);
  pixelmatch::Options options;
  options.threshold = params.threshold;
  options.includeAA = params.includeAntiAliasing;
  const int mismatched = pixelmatch::pixelmatch(expected.pixels, actual.pixels, diffImage, width,
                                                height, strideInPixels, options);

  if (mismatched > params.maxMismatchedPixels) {
    const std::filesystem::path outDir = DiffOutputDir();
    const std::string flat = Flatten(testLabel);
    const auto actualPath = outDir / ("actual_" + flat + ".png");
    const auto expectedPath = outDir / ("expected_" + flat + ".png");
    const auto diffPath = outDir / ("diff_" + flat + ".png");
    svg::RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), actual.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), expected.pixels,
                                                   width, height, strideInPixels);
    svg::RendererImageIO::writeRgbaPixelsToPngFile(diffPath.string().c_str(), diffImage, width,
                                                   height, strideInPixels);
    ADD_FAILURE() << "[" << testLabel << "] " << mismatched
                  << " pixels differ (max allowed: " << params.maxMismatchedPixels
                  << "). Actual: " << actualPath.string() << ". Expected: " << expectedPath.string()
                  << ". Diff: " << diffPath.string();
  } else {
    std::fprintf(stderr, "[%s] PASS bitmap-to-bitmap (%d px differ, max %d)\n",
                 std::string(testLabel).c_str(), mismatched, params.maxMismatchedPixels);
  }
}

}  // namespace donner::editor::tests
