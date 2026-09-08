/// @file
/// PNG encoding and failure diagnostics preserve pixels with padded rows.

#include "donner/svg/renderer/RendererImageIO.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"

namespace donner::svg {
namespace {

RendererBitmap ExpectedCorners() {
  return {.dimensions = {2, 2},
          .pixels = {255, 0, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255, 255, 255, 255, 255},
          .rowBytes = 8};
}

std::vector<uint8_t> PaddedCorners() {
  return {255, 0,   0, 255, 0,   0,   255, 255, 17, 18, 19, 20,
          0,   255, 0, 255, 255, 255, 255, 255, 21, 22, 23, 24};
}

void ExpectPng(const std::filesystem::path& filename, const RendererBitmap& expected) {
  const auto image = RendererImageTestUtils::readRgbaImageFromPngFile(filename.string().c_str());
  ASSERT_TRUE(image.has_value());
  const RendererBitmap actual{.dimensions = {image->width, image->height},
                              .pixels = image->data,
                              .rowBytes = image->strideInPixels * 4};
  editor::tests::CompareBitmapToBitmap(actual, expected, filename.filename().string(),
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(RendererImageIOTests, PaddedRowsRoundTripToFile) {
  const auto filename = std::filesystem::path(testing::TempDir()) / "padded_file.png";
  const auto pixels = PaddedCorners();
  ASSERT_TRUE(
      RendererImageIO::writeRgbaPixelsToPngFile(filename.string().c_str(), pixels, 2, 2, 3));
  ExpectPng(filename, ExpectedCorners());
}

TEST(RendererImageIOTests, PaddedRowsWithoutFinalPaddingRoundTripToMemory) {
  auto pixels = PaddedCorners();
  pixels.resize(20);
  const auto encoded = RendererImageIO::writeRgbaPixelsToPngMemory(pixels, 2, 2, 3);
  ASSERT_THAT(encoded, testing::Not(testing::IsEmpty()));
  const auto filename = std::filesystem::path(testing::TempDir()) / "padded_memory.png";
  {
    std::ofstream output(filename, std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    ASSERT_TRUE(output.good());
  }
  ExpectPng(filename, ExpectedCorners());
}

TEST(RendererImageIOTests, RejectsInvalidLayoutsBeforeOpeningAFile) {
  struct Layout {
    int width;
    int height;
    size_t stride;
    size_t bytes;
  };
  const std::array<uint8_t, 16> pixels{};
  const auto filename = std::filesystem::path(testing::TempDir()) / "invalid_layout.png";
  for (const Layout& layout :
       {Layout{0, 1, 0, 4}, Layout{1, 0, 0, 4}, Layout{-1, 1, 0, 4}, Layout{1, -1, 0, 4},
        Layout{1, 1, 0, 3}, Layout{2, 1, 1, 8}, Layout{1, 2, 4, 16},
        Layout{1, 1, std::numeric_limits<size_t>::max(), 4},
        Layout{std::numeric_limits<int>::max(), 1, 0, 4},
        Layout{1, std::numeric_limits<int>::max(), 0, 4}}) {
    SCOPED_TRACE(testing::Message() << layout.width << "x" << layout.height
                                    << " stride=" << layout.stride << " bytes=" << layout.bytes);
    const std::span<const uint8_t> data(pixels.data(), layout.bytes);
    EXPECT_THAT(RendererImageIO::writeRgbaPixelsToPngMemory(data, layout.width, layout.height,
                                                            layout.stride),
                testing::IsEmpty());
    EXPECT_FALSE(RendererImageIO::writeRgbaPixelsToPngFile(
        filename.string().c_str(), data, layout.width, layout.height, layout.stride));
    EXPECT_FALSE(std::filesystem::exists(filename));
  }
}

TEST(RendererImageIOTests, PaddedMismatchProducesReadableDiagnosticImages) {
  const char* outputDirectory = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  ASSERT_NE(outputDirectory, nullptr);
  RendererBitmap actual{.dimensions = {1, 2}, .pixels = std::vector<uint8_t>(512), .rowBytes = 256};
  RendererBitmap expected = actual;
  for (size_t row : {0u, 1u}) {
    actual.pixels[row * 256 + 2] = 255;
    actual.pixels[row * 256 + 3] = 255;
    expected.pixels[row * 256] = 255;
    expected.pixels[row * 256 + 3] = 255;
  }
  EXPECT_NONFATAL_FAILURE(
      editor::tests::CompareBitmapToBitmap(actual, expected, "strided_png_diagnostics",
                                           editor::tests::PixelmatchIdentityParams()),
      "pixels differ");
  const auto directory = std::filesystem::path(outputDirectory);
  const RendererBitmap blue{
      .dimensions = {1, 2}, .pixels = {0, 0, 255, 255, 0, 0, 255, 255}, .rowBytes = 4};
  const RendererBitmap red{
      .dimensions = {1, 2}, .pixels = {255, 0, 0, 255, 255, 0, 0, 255}, .rowBytes = 4};
  const RendererBitmap sideBySide{
      .dimensions = {2, 2},
      .pixels = {255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255},
      .rowBytes = 8};
  ExpectPng(directory / "actual_strided_png_diagnostics.png", blue);
  ExpectPng(directory / "expected_strided_png_diagnostics.png", red);
  ExpectPng(directory / "side_by_side_strided_png_diagnostics.png", sideBySide);
  const auto diff = RendererImageTestUtils::readRgbaImageFromPngFile(
      (directory / "diff_strided_png_diagnostics.png").string().c_str());
  ASSERT_TRUE(diff.has_value());
  EXPECT_EQ(diff->width, 1);
  EXPECT_EQ(diff->height, 2);
}

}  // namespace
}  // namespace donner::svg
