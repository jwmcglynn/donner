/// @file
/// Unit tests for the single-element rasterization API
/// (`svg::Renderer::renderElementToBitmap`), which the Layers panel uses to
/// produce real Donner-rendered row thumbnails instead of ImGui-synthesized
/// vector silhouettes (CLAUDE.md "No Rendering Vector Graphics With ImGui").

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/LayerTreeModel.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::svg {
namespace {

constexpr std::string_view kRedRectSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="r" x="10" y="10" width="80" height="80" fill="rgb(220,0,0)"/>
</svg>)SVG";

constexpr std::string_view kGreenCircleSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <circle id="c" cx="50" cy="50" r="40" fill="rgb(0,200,0)"/>
</svg>)SVG";

constexpr std::string_view kTwoHalvesGroupSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <g id="g">
    <rect x="0" y="0" width="50" height="100" fill="rgb(80,200,255)"/>
    <rect x="50" y="0" width="50" height="100" fill="rgb(210,210,0)"/>
  </g>
</svg>)SVG";

constexpr std::string_view kWideBlackBackgroundSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="175" height="100" viewBox="0 0 175 100">
  <g id="Background">
    <rect x="0" y="0" width="175" height="100" fill="rgb(0,0,0)"/>
  </g>
</svg>)SVG";

constexpr std::string_view kRootGroupWithOffCanvasSiblingSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <g id="outer">
    <rect x="0" y="0" width="100" height="100" fill="rgb(0,0,0)"/>
    <rect x="0" y="-300" width="100" height="100" fill="rgb(220,0,0)"/>
  </g>
</svg>)SVG";

constexpr std::string_view kForegroundOverBackgroundSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <rect id="Background" x="0" y="0" width="100" height="100" fill="rgb(16,24,32)"/>
  <circle id="dot" cx="50" cy="50" r="40" fill="rgb(250,225,0)"/>
</svg>)SVG";

constexpr std::string_view kStrokeOnlyLineSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <line id="strokeOnly" x1="10" y1="50" x2="90" y2="50" fill="none" stroke="rgb(250,40,20)" stroke-width="12"/>
</svg>)SVG";

constexpr std::string_view kWideSeparatedDotsSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <g id="wide">
    <circle cx="5" cy="50" r="5" fill="rgb(230,0,0)"/>
    <circle cx="95" cy="50" r="5" fill="rgb(0,0,230)"/>
  </g>
</svg>)SVG";

constexpr std::string_view kTransparentGeometryBiasSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <g id="biased">
    <rect x="0" y="0" width="45" height="100" fill="black" opacity="0"/>
    <circle cx="78" cy="50" r="12" fill="rgb(240,190,0)"/>
  </g>
</svg>)SVG";

// Read the RGBA pixel at (x, y) from a row-bytes-aware renderer bitmap.
std::array<int, 4> PixelAt(const RendererBitmap& bitmap, int x, int y) {
  const std::size_t index =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[index + 0], bitmap.pixels[index + 1], bitmap.pixels[index + 2],
          bitmap.pixels[index + 3]};
}

template <typename Predicate>
int CountPixelsMatching(const RendererBitmap& bitmap, Predicate predicate) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (predicate(PixelAt(bitmap, x, y))) {
        ++count;
      }
    }
  }
  return count;
}

template <typename Predicate>
double AverageXOfPixelsMatching(const RendererBitmap& bitmap, Predicate predicate) {
  double sumX = 0.0;
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      if (predicate(PixelAt(bitmap, x, y))) {
        sumX += static_cast<double>(x);
        ++count;
      }
    }
  }
  return count == 0 ? 0.0 : sumX / static_cast<double>(count);
}

bool IsBrightWarmPixel(const std::array<int, 4>& px) {
  return px[3] > 180 && px[0] > 170 && px[1] > 110 && px[2] < 130;
}

bool IsDarkOpaquePixel(const std::array<int, 4>& px) {
  return px[3] > 220 && px[0] < 45 && px[1] < 60 && px[2] < 80;
}

bool IsRedPixel(const std::array<int, 4>& px) {
  return px[3] > 180 && px[0] > 170 && px[1] < 80 && px[2] < 80;
}

bool IsBluePixel(const std::array<int, 4>& px) {
  return px[3] > 180 && px[0] < 80 && px[1] < 80 && px[2] > 170;
}

// gmock matcher: the 4-channel pixel is within `tol` of (r, g, b, a) on every
// channel. Prints the full actual RGBA on failure (ToTT-style diagnostic).
MATCHER_P5(RgbaNear, r, g, b, a, tol, "") {
  const auto& px = arg;
  const bool ok = std::abs(px[0] - r) <= tol && std::abs(px[1] - g) <= tol &&
                  std::abs(px[2] - b) <= tol && std::abs(px[3] - a) <= tol;
  if (!ok) {
    *result_listener << "actual RGBA = (" << px[0] << ", " << px[1] << ", " << px[2] << ", "
                     << px[3] << "), expected near (" << r << ", " << g << ", " << b << ", " << a
                     << ") +/- " << tol;
  }
  return ok;
}

// Fetch the element whose display name matches `id`, via the LayerTreeModel row
// list (the same DOM elements the Layers panel renders thumbnails for).
std::optional<SVGElement> ElementById(editor::EditorApp& app, std::string_view id) {
  editor::LayerTreeModel model;
  model.refresh(app);
  for (const editor::LayerTreeRow& row : model.rows()) {
    if (std::string_view(row.displayName) == id) {
      return row.element;
    }
  }
  return std::nullopt;
}

std::string ReadFixture(std::string_view path) {
  std::ifstream input{donner::Runfiles::instance().Rlocation(std::string(path))};
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

TEST(RenderElementToBitmapTest, RedRectFillsCenterRed) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRedRectSvg));

  const std::optional<SVGElement> rect = ElementById(app, "r");
  ASSERT_TRUE(rect.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*rect, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());
  EXPECT_EQ(bitmap.dimensions, Vector2i(24, 24));

  // The square fills the cell, so the center pixel is the rect's opaque fill.
  EXPECT_THAT(PixelAt(bitmap, 12, 12), RgbaNear(220, 0, 0, 255, 4));
  EXPECT_THAT(PixelAt(bitmap, 0, 12), RgbaNear(220, 0, 0, 255, 4))
      << "thumbnail crop should not leave left padding";
  EXPECT_THAT(PixelAt(bitmap, 23, 12), RgbaNear(220, 0, 0, 255, 4))
      << "thumbnail crop should not leave right padding";
  EXPECT_THAT(PixelAt(bitmap, 12, 0), RgbaNear(220, 0, 0, 255, 4))
      << "thumbnail crop should not leave top padding";
  EXPECT_THAT(PixelAt(bitmap, 12, 23), RgbaNear(220, 0, 0, 255, 4))
      << "thumbnail crop should not leave bottom padding";
}

TEST(RenderElementToBitmapTest, GreenCircleCenterGreenCornerTransparent) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kGreenCircleSvg));

  const std::optional<SVGElement> circle = ElementById(app, "c");
  ASSERT_TRUE(circle.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*circle, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());

  // Center is inside the circle: green and opaque.
  EXPECT_THAT(PixelAt(bitmap, 12, 12), RgbaNear(0, 200, 0, 255, 6));
  // A corner is outside the circle's bounds: fully transparent (the renderer
  // produced a transparent-background thumbnail, no ImGui backdrop).
  EXPECT_THAT(PixelAt(bitmap, 0, 0), RgbaNear(0, 0, 0, 0, 2));
}

TEST(RenderElementToBitmapTest, ForegroundThumbnailDoesNotIncludeBackgroundSibling) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kForegroundOverBackgroundSvg));

  const std::optional<SVGElement> dot = ElementById(app, "dot");
  ASSERT_TRUE(dot.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*dot, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());

  EXPECT_THAT(PixelAt(bitmap, 12, 12), RgbaNear(250, 225, 0, 255, 6));
  EXPECT_THAT(PixelAt(bitmap, 0, 0), RgbaNear(0, 0, 0, 0, 2))
      << "a layer thumbnail must keep transparent corners transparent instead of compositing "
         "the document background sibling";
}

TEST(RenderElementToBitmapTest, DonnerSplashLayerThumbnailDoesNotIncludeBackgroundLayer) {
  editor::EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  const std::optional<SVGElement> donner = app.document().document().querySelector("#Donner");
  ASSERT_TRUE(donner.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*donner, Vector2i(32, 32));
  ASSERT_FALSE(bitmap.empty());

  EXPECT_THAT(PixelAt(bitmap, 0, 0), RgbaNear(0, 0, 0, 0, 2))
      << "the Donner layer thumbnail should keep empty crop corners transparent instead of "
         "showing the document background layer";
}

TEST(RenderElementToBitmapTest, DonnerSplashDonnerThumbnailShowsLetterFill) {
  editor::EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  const std::optional<SVGElement> donner = app.document().document().querySelector("#Donner");
  ASSERT_TRUE(donner.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*donner, Vector2i(32, 32));
  ASSERT_FALSE(bitmap.empty());

  EXPECT_THAT(PixelAt(bitmap, 0, 0), RgbaNear(0, 0, 0, 0, 2))
      << "the Donner layer itself does not include the document background or sticker silhouette";
  EXPECT_GT(CountPixelsMatching(bitmap, IsBrightWarmPixel), 80)
      << "the Donner thumbnail should show the yellow/white letter fills, not a dark silhouette";
  EXPECT_NEAR(AverageXOfPixelsMatching(bitmap, IsBrightWarmPixel), 15.5, 5.0)
      << "the Donner letters should be centered instead of cropped to one edge";
  EXPECT_LT(CountPixelsMatching(bitmap, IsDarkOpaquePixel), 500)
      << "opaque dark pixels dominating the thumbnail means the row is rendering background-like "
         "content instead of the Donner letters";
}

TEST(RenderElementToBitmapTest, DonnerSplashSunburstThumbnailCentersBrightContent) {
  editor::EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  const std::optional<SVGElement> sunburst = app.document().document().querySelector("#Sunburst");
  ASSERT_TRUE(sunburst.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*sunburst, Vector2i(32, 32));
  ASSERT_FALSE(bitmap.empty());

  const int warmPixels = CountPixelsMatching(bitmap, IsBrightWarmPixel);
  ASSERT_GT(warmPixels, 20) << "the Sunburst thumbnail should include the sun, not only shine";
  EXPECT_NEAR(AverageXOfPixelsMatching(bitmap, IsBrightWarmPixel), 15.5, 5.0)
      << "the sun should be centered in the thumbnail instead of clipped to an edge";
}

TEST(RenderElementToBitmapTest, DonnerSplashSunburstThumbnailUsesFullElementBounds) {
  editor::EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  const std::optional<SVGElement> sunburst = app.document().document().querySelector("#Sunburst");
  ASSERT_TRUE(sunburst.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*sunburst, Vector2i(42, 24));
  ASSERT_FALSE(bitmap.empty());

  // #Sunburst's full canvas-visible element bounds are the shine ellipse
  // (455.4x272.32 after clipping the off-canvas top to the root viewBox), so
  // fitting into the layer-panel max thumbnail produces a 41x24 bitmap.
  EXPECT_EQ(bitmap.dimensions.x, 41);
  EXPECT_EQ(bitmap.dimensions.y, 24);
}

TEST(RenderElementToBitmapTest, DonnerSplashBlueCenterBurstThumbnailUsesFullElementBounds) {
  editor::EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  const std::optional<SVGElement> burst =
      app.document().document().querySelector("#Blue_center_burst");
  ASSERT_TRUE(burst.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*burst, Vector2i(42, 24));
  ASSERT_FALSE(bitmap.empty());

  // #Blue_center_burst is a 365.32x420.64 ellipse, clipped by the root viewBox
  // to 365.32x419.06. Fitting the whole element into 42x24 produces 21x24.
  EXPECT_EQ(bitmap.dimensions.x, 21);
  EXPECT_EQ(bitmap.dimensions.y, 24);
}

TEST(RenderElementToBitmapTest, DonnerSplashLayerThumbnailBoundsStayStableAfterCanvasResize) {
  editor::EditorApp app;
  const std::string source = ReadFixture("donner_splash.svg");
  ASSERT_FALSE(source.empty());
  ASSERT_TRUE(app.loadFromString(source));

  const std::optional<SVGElement> sunburst = app.document().document().querySelector("#Sunburst");
  const std::optional<SVGElement> backgroundSticker =
      app.document().document().querySelector("#Background_sticker");
  const std::optional<SVGElement> blueCenterBurst =
      app.document().document().querySelector("#Blue_center_burst");
  ASSERT_TRUE(sunburst.has_value());
  ASSERT_TRUE(backgroundSticker.has_value());
  ASSERT_TRUE(blueCenterBurst.has_value());

  Renderer renderer;
  const RendererBitmap initialSunburst =
      renderer.renderElementToBitmap(*sunburst, Vector2i(42, 24));
  const RendererBitmap initialBackgroundSticker =
      renderer.renderElementToBitmap(*backgroundSticker, Vector2i(42, 24));
  const RendererBitmap initialBlueCenterBurst =
      renderer.renderElementToBitmap(*blueCenterBurst, Vector2i(42, 24));
  ASSERT_EQ(initialSunburst.dimensions, Vector2i(41, 24));
  ASSERT_EQ(initialBackgroundSticker.dimensions, Vector2i(27, 24));
  ASSERT_EQ(initialBlueCenterBurst.dimensions, Vector2i(21, 24));

  app.document().document().setCanvasSize(1784, 1024);

  EXPECT_EQ(renderer.renderElementToBitmap(*sunburst, Vector2i(42, 24)).dimensions,
            initialSunburst.dimensions)
      << "the layer thumbnail crop must stay in the same coordinate space after the editor "
         "resizes the live document canvas for a rerender";
  EXPECT_EQ(renderer.renderElementToBitmap(*backgroundSticker, Vector2i(42, 24)).dimensions,
            initialBackgroundSticker.dimensions);
  EXPECT_EQ(renderer.renderElementToBitmap(*blueCenterBurst, Vector2i(42, 24)).dimensions,
            initialBlueCenterBurst.dimensions);
}

TEST(RenderElementToBitmapTest, WideLayerThumbnailFitsFullContentInRectangularBitmap) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kWideSeparatedDotsSvg));

  const std::optional<SVGElement> group = ElementById(app, "wide");
  ASSERT_TRUE(group.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*group, Vector2i(42, 24));
  ASSERT_FALSE(bitmap.empty());

  EXPECT_EQ(bitmap.dimensions.x, 42);
  EXPECT_GE(bitmap.dimensions.y, 3);
  EXPECT_LE(bitmap.dimensions.y, 6)
      << "a very wide layer should return a short bitmap that follows its content aspect ratio";
  EXPECT_GT(CountPixelsMatching(bitmap, IsRedPixel), 2)
      << "the left endpoint of a wide layer should stay visible";
  EXPECT_GT(CountPixelsMatching(bitmap, IsBluePixel), 2)
      << "the right endpoint of a wide layer should stay visible";
  EXPECT_LT(AverageXOfPixelsMatching(bitmap, IsRedPixel), 6.0);
  EXPECT_GT(AverageXOfPixelsMatching(bitmap, IsBluePixel), 35.0);
  EXPECT_THAT(PixelAt(bitmap, 21, bitmap.dimensions.y / 2), RgbaNear(0, 0, 0, 0, 2))
      << "fitting the full wide layer should not crop the transparent center gap";
}

TEST(RenderElementToBitmapTest, TransparentGeometryDoesNotBiasTightCrop) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTransparentGeometryBiasSvg));

  const std::optional<SVGElement> group = ElementById(app, "biased");
  ASSERT_TRUE(group.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*group, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());

  EXPECT_GT(CountPixelsMatching(bitmap, IsBrightWarmPixel), 250)
      << "the visible circle should fill the thumbnail after alpha-tight cropping";
  EXPECT_NEAR(AverageXOfPixelsMatching(bitmap, IsBrightWarmPixel), 11.5, 1.5)
      << "transparent geometry in the same group must not push visible pixels to the edge";
}

TEST(RenderElementToBitmapTest, StrokeOnlyLayerThumbnailUsesStrokeContentBounds) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kStrokeOnlyLineSvg));

  const std::optional<SVGElement> line = ElementById(app, "strokeOnly");
  ASSERT_TRUE(line.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*line, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty())
      << "stroke-only geometry is visible layer content and must not fall back to a fill swatch";

  EXPECT_EQ(bitmap.dimensions.x, 24);
  EXPECT_LT(bitmap.dimensions.y, 8)
      << "a horizontal stroke thumbnail should keep its content aspect ratio";
  EXPECT_THAT(PixelAt(bitmap, bitmap.dimensions.x / 2, bitmap.dimensions.y / 2),
              RgbaNear(250, 40, 20, 255, 8));
}

TEST(RenderElementToBitmapTest, GroupComposesDescendants) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoHalvesGroupSvg));

  const std::optional<SVGElement> group = ElementById(app, "g");
  ASSERT_TRUE(group.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*group, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());

  // The group's two halves compose into one preview: left blue, right yellow.
  EXPECT_THAT(PixelAt(bitmap, 6, 12), RgbaNear(80, 200, 255, 255, 6));
  EXPECT_THAT(PixelAt(bitmap, 17, 12), RgbaNear(210, 210, 0, 255, 6));
}

TEST(RenderElementToBitmapTest, WideBackgroundFillsMatchingRectangularThumbnail) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kWideBlackBackgroundSvg));

  const std::optional<SVGElement> background = ElementById(app, "Background");
  ASSERT_TRUE(background.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*background, Vector2i(42, 24));
  ASSERT_FALSE(bitmap.empty());

  EXPECT_EQ(bitmap.dimensions, Vector2i(42, 24));
  EXPECT_THAT(PixelAt(bitmap, 21, 0), RgbaNear(0, 0, 0, 255, 4))
      << "a full-canvas background thumbnail should not expose checkerboard letterbox rows";
  EXPECT_THAT(PixelAt(bitmap, 21, 12), RgbaNear(0, 0, 0, 255, 4));
  EXPECT_THAT(PixelAt(bitmap, 21, 23), RgbaNear(0, 0, 0, 255, 4))
      << "a full-canvas background thumbnail should not expose checkerboard letterbox rows";
}

TEST(RenderElementToBitmapTest, RootGroupThumbnailIsClippedToSvgViewBox) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRootGroupWithOffCanvasSiblingSvg));

  const std::optional<SVGElement> group = ElementById(app, "outer");
  ASSERT_TRUE(group.has_value());

  Renderer renderer;
  const RendererBitmap bitmap = renderer.renderElementToBitmap(*group, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());

  EXPECT_THAT(PixelAt(bitmap, 12, 0), RgbaNear(0, 0, 0, 255, 4))
      << "off-canvas descendants must not expand the root group thumbnail crop";
  EXPECT_THAT(PixelAt(bitmap, 12, 12), RgbaNear(0, 0, 0, 255, 4));
  EXPECT_THAT(PixelAt(bitmap, 12, 23), RgbaNear(0, 0, 0, 255, 4))
      << "the visible SVG canvas should fill the root group thumbnail";
}

TEST(RenderElementToBitmapTest, ZeroSizeReturnsEmpty) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRedRectSvg));
  const std::optional<SVGElement> rect = ElementById(app, "r");
  ASSERT_TRUE(rect.has_value());

  Renderer renderer;
  EXPECT_TRUE(renderer.renderElementToBitmap(*rect, Vector2i(0, 24)).empty());
  EXPECT_TRUE(renderer.renderElementToBitmap(*rect, Vector2i(24, -3)).empty());
}

}  // namespace
}  // namespace donner::svg
