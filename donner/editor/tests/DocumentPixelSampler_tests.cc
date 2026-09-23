#include "donner/editor/DocumentPixelSampler.h"

#include <cstdint>
#include <limits>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

using ::testing::Eq;

svg::RendererBitmap OnePixel(std::vector<std::uint8_t> rgba, svg::AlphaType alphaType) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(1, 1);
  bitmap.rowBytes = 4;
  bitmap.pixels = std::move(rgba);
  bitmap.alphaType = alphaType;
  return bitmap;
}

TEST(DocumentPixelSamplerTest, ConvertsPremultipliedRgbaToSvgColor) {
  const svg::RendererBitmap bitmap = OnePixel({64, 32, 0, 128}, svg::AlphaType::Premultiplied);
  const std::optional<css::RGBA> color = ReadDocumentPixel(bitmap, Vector2i(0, 0));
  ASSERT_THAT(color, ::testing::Optional(::testing::_));
  EXPECT_THAT(color->toHexString(), Eq("#80400080"));
}

TEST(DocumentPixelSamplerTest, PreservesStraightAlphaAndCanonicalizesZeroAlpha) {
  const svg::RendererBitmap straight =
      OnePixel({51, 102, 153, 128}, svg::AlphaType::Unpremultiplied);
  const svg::RendererBitmap transparent = OnePixel({255, 10, 50, 0}, svg::AlphaType::Premultiplied);
  const std::optional<css::RGBA> straightColor = ReadDocumentPixel(straight, Vector2i(0, 0));
  const std::optional<css::RGBA> transparentColor = ReadDocumentPixel(transparent, Vector2i(0, 0));
  ASSERT_THAT(straightColor, ::testing::Optional(::testing::_));
  ASSERT_THAT(transparentColor, ::testing::Optional(::testing::_));
  EXPECT_THAT(straightColor->toHexString(), Eq("#33669980"));
  EXPECT_THAT(transparentColor->toHexString(), Eq("#00000000"));
}

TEST(DocumentPixelSamplerTest, RejectsIncompleteRowsAndOutOfRangePixels) {
  svg::RendererBitmap bitmap = OnePixel({1, 2, 3}, svg::AlphaType::Premultiplied);
  EXPECT_THAT(IsValidDocumentPixelBitmap(bitmap), Eq(false));
  EXPECT_THAT(ReadDocumentPixel(bitmap, Vector2i(0, 0)), Eq(std::nullopt));
  bitmap.pixels.push_back(4);
  bitmap.rowBytes = 3;
  EXPECT_THAT(IsValidDocumentPixelBitmap(bitmap), Eq(false));
  bitmap.rowBytes = 4;
  EXPECT_THAT(ReadDocumentPixel(bitmap, Vector2i(1, 0)), Eq(std::nullopt));
  EXPECT_THAT(ReadDocumentPixel(bitmap, Vector2i(-1, 0)), Eq(std::nullopt));
}

TEST(DocumentPixelSamplerTest, AppliesGpuRowPaddingToProvisionalCaptureCap) {
  EXPECT_THAT(CanCaptureDocumentPixelSize(Vector2i(8192, 8192)), Eq(true));
  EXPECT_THAT(CanCaptureDocumentPixelSize(Vector2i(8192, 8193)), Eq(false));
  EXPECT_THAT(CanCaptureDocumentPixelSize(Vector2i(8191, 8192)), Eq(true));
  EXPECT_THAT(CanCaptureDocumentPixelSize(Vector2i(0, 4)), Eq(false));
  EXPECT_THAT(CanCaptureDocumentPixelSize(Vector2i(std::numeric_limits<int>::max(), 1)), Eq(false));
}

TEST(DocumentPixelSamplerTest, MapsPannedHighZoomViewBoxAtDoubleDeviceScale) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(100.0, 50.0);
  viewport.paneSize = Vector2d(200.0, 200.0);
  viewport.documentViewBox = Box2d::FromXYWH(20.0, 30.0, 100.0, 100.0);
  viewport.devicePixelRatio = 2.0;
  viewport.zoom = 20.0;
  viewport.panDocPoint = Vector2d(60.0, 80.0);
  viewport.panScreenPoint = Vector2d(150.0, 100.0);
  const EditorRasterViewport raster = viewport.rasterViewport();
  EXPECT_THAT(raster.viewportBounded, Eq(true));
  EXPECT_THAT(DocumentPixelIndexAtScreenPoint(viewport, raster, Vector2d(150.0, 100.0)),
              ::testing::Optional(Eq(Vector2i(356, 356))));
  EXPECT_THAT(DocumentPixelIndexAtScreenPoint(viewport, raster, Vector2d(200.0, 120.0)),
              ::testing::Optional(Eq(Vector2i(456, 396))));
  EXPECT_THAT(DocumentPixelIndexAtScreenPoint(
                  viewport, raster, Vector2d(std::numeric_limits<double>::infinity(), 100.0)),
              Eq(std::nullopt));
}

TEST(DocumentPixelSamplerTest, MapsOnlyPaneAndDocumentPointsIntoRasterPixels) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(10.0, 10.0);
  viewport.paneSize = Vector2d(40.0, 40.0);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 4.0, 4.0);
  viewport.zoom = 10.0;
  viewport.panDocPoint = Vector2d::Zero();
  viewport.panScreenPoint = viewport.paneOrigin;
  const EditorRasterViewport raster = viewport.rasterViewport();
  EXPECT_THAT(DocumentPixelIndexAtScreenPoint(viewport, raster, Vector2d(25.0, 25.0)),
              ::testing::Optional(Eq(Vector2i(15, 15))));
  EXPECT_THAT(DocumentPixelIndexAtScreenPoint(viewport, raster, Vector2d(10.0, 10.0)),
              ::testing::Optional(Eq(Vector2i(0, 0))));
  EXPECT_THAT(DocumentPixelIndexAtScreenPoint(viewport, raster, Vector2d(50.0, 25.0)),
              Eq(std::nullopt));
  EXPECT_THAT(DocumentPixelIndexAtScreenPoint(viewport, raster, Vector2d(9.0, 25.0)),
              Eq(std::nullopt));
}

}  // namespace
}  // namespace donner::editor
