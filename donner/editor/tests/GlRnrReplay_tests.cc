/// @file

#include "donner/editor/repro/GlRnrReplay.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/Vector2.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

struct PixelCrop {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

std::filesystem::path DiagnosticOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

const repro::GlRnrReplayCapture* FindCapture(const repro::GlRnrReplayResult& result,
                                             std::uint64_t frameIndex) {
  for (const repro::GlRnrReplayCapture& capture : result.captures) {
    if (capture.frameIndex == frameIndex) {
      return &capture;
    }
  }
  return nullptr;
}

svg::RendererBitmap BitmapFromImage(const svg::Image& image) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(image.width, image.height);
  bitmap.rowBytes = image.strideInPixels * 4u;
  bitmap.alphaType = svg::AlphaType::Premultiplied;
  bitmap.pixels = image.data;
  return bitmap;
}

std::optional<svg::RendererBitmap> LoadCaptureBitmap(const repro::GlRnrReplayResult& result,
                                                     std::uint64_t frameIndex) {
  const repro::GlRnrReplayCapture* capture = FindCapture(result, frameIndex);
  if (capture == nullptr) {
    ADD_FAILURE() << "GL replay did not capture frame " << frameIndex;
    return std::nullopt;
  }

  std::optional<svg::Image> image =
      svg::RendererImageTestUtils::readRgbaImageFromPngFile(capture->path.string().c_str());
  if (!image.has_value()) {
    return std::nullopt;
  }
  return BitmapFromImage(*image);
}

svg::RendererBitmap CropBitmap(const svg::RendererBitmap& bitmap, const PixelCrop& crop) {
  const int x = std::clamp(crop.x, 0, bitmap.dimensions.x);
  const int y = std::clamp(crop.y, 0, bitmap.dimensions.y);
  const int width = std::clamp(crop.width, 0, bitmap.dimensions.x - x);
  const int height = std::clamp(crop.height, 0, bitmap.dimensions.y - y);

  svg::RendererBitmap cropped;
  cropped.dimensions = Vector2i(width, height);
  cropped.rowBytes = static_cast<std::size_t>(width) * 4u;
  cropped.alphaType = bitmap.alphaType;
  cropped.pixels.resize(cropped.rowBytes * static_cast<std::size_t>(height));

  for (int row = 0; row < height; ++row) {
    const std::uint8_t* source = bitmap.pixels.data() +
                                 static_cast<std::size_t>(y + row) * bitmap.rowBytes +
                                 static_cast<std::size_t>(x) * 4u;
    std::uint8_t* target = cropped.pixels.data() + static_cast<std::size_t>(row) * cropped.rowBytes;
    std::memcpy(target, source, cropped.rowBytes);
  }

  return cropped;
}

std::optional<double> YellowCentroidY(const svg::RendererBitmap& bitmap) {
  double totalY = 0.0;
  int count = 0;

  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 200 && red > 180 && green > 130 && blue < 90 && red - green < 110) {
        totalY += static_cast<double>(y);
        ++count;
      }
    }
  }

  if (count == 0) {
    return std::nullopt;
  }
  return totalY / static_cast<double>(count);
}

TEST(GlRnrReplayTest, FilteredElementOThenRDragDoesNotPopOBackOnRClick) {
  constexpr std::string_view kRnrPath =
      "donner/editor/tests/filtered-element-flash-after-drags-2.rnr";
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(std::filesystem::path(kRnrPath));
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  ASSERT_TRUE(reproFile->metadata.expect->cropRect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.cropMode, "document-canvas");
  const std::uint64_t beforeClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex - 1);
  const std::uint64_t firstClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex);
  const std::uint64_t settledClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex + 2);

  repro::GlRnrReplayOptions options;
  options.rnrPath = std::string(kRnrPath);
  options.outputDir = DiagnosticOutputDir() / "gl_o_then_r_popback_repro";
  options.captureFrames = {beforeClickFrame, firstClickFrame, settledClickFrame};
  options.maxFrame = static_cast<std::uint64_t>(expect.maxFrameIndex);
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> beforeRClick = LoadCaptureBitmap(result, beforeClickFrame);
  std::optional<svg::RendererBitmap> firstRClickFrame = LoadCaptureBitmap(result, firstClickFrame);
  std::optional<svg::RendererBitmap> settledRClickFrame =
      LoadCaptureBitmap(result, settledClickFrame);
  ASSERT_TRUE(beforeRClick.has_value());
  ASSERT_TRUE(firstRClickFrame.has_value());
  ASSERT_TRUE(settledRClickFrame.has_value());

  const PixelCrop oCrop{
      .x = expect.cropRect->x,
      .y = expect.cropRect->y,
      .width = expect.cropRect->width,
      .height = expect.cropRect->height,
  };
  const svg::RendererBitmap firstRClickO = CropBitmap(*firstRClickFrame, oCrop);
  const svg::RendererBitmap settledRClickO = CropBitmap(*settledRClickFrame, oCrop);

  tests::CompareBitmapToBitmap(firstRClickO, settledRClickO,
                               "gl_o_then_r_frame_153_o_crop_vs_frame_155",
                               tests::PixelmatchIdentityParams());

  const std::optional<double> beforeCentroidY = YellowCentroidY(CropBitmap(*beforeRClick, oCrop));
  const std::optional<double> firstCentroidY = YellowCentroidY(firstRClickO);
  const std::optional<double> settledCentroidY = YellowCentroidY(settledRClickO);
  ASSERT_TRUE(beforeCentroidY.has_value());
  ASSERT_TRUE(firstCentroidY.has_value());
  ASSERT_TRUE(settledCentroidY.has_value());
  EXPECT_NEAR(*beforeCentroidY, *settledCentroidY, 1.0)
      << "The two stable frames should agree on the post-O-drag position.";
  EXPECT_NEAR(*firstCentroidY, *settledCentroidY, 1.0)
      << "The first R-click frame should keep O at its post-drag position instead of popping "
         "back for one presented frame.";
}

}  // namespace
}  // namespace donner::editor
