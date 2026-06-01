/// @file
/// Unit tests for the single-element rasterization API
/// (`svg::RenderElementToBitmap`), which the Layers panel uses to produce real
/// Donner-rendered row thumbnails instead of ImGui-synthesized vector
/// silhouettes (CLAUDE.md "No Rendering Vector Graphics With ImGui").

#include "donner/svg/renderer/RenderElementToBitmap.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>

#include "donner/editor/EditorApp.h"
#include "donner/editor/LayerTreeModel.h"
#include "donner/svg/SVGElement.h"

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
    <rect x="0" y="0" width="50" height="100" fill="rgb(0,0,210)"/>
    <rect x="50" y="0" width="50" height="100" fill="rgb(210,210,0)"/>
  </g>
</svg>)SVG";

// Read the RGBA pixel at (x, y) from a row-bytes-aware renderer bitmap.
std::array<int, 4> PixelAt(const RendererBitmap& bitmap, int x, int y) {
  const std::size_t index =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[index + 0], bitmap.pixels[index + 1], bitmap.pixels[index + 2],
          bitmap.pixels[index + 3]};
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

TEST(RenderElementToBitmapTest, RedRectFillsCenterRed) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRedRectSvg));

  const std::optional<SVGElement> rect = ElementById(app, "r");
  ASSERT_TRUE(rect.has_value());

  const RendererBitmap bitmap = RenderElementToBitmap(*rect, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());
  EXPECT_EQ(bitmap.dimensions, Vector2i(24, 24));

  // The square fills the cell, so the center pixel is the rect's opaque fill.
  EXPECT_THAT(PixelAt(bitmap, 12, 12), RgbaNear(220, 0, 0, 255, 4));
}

TEST(RenderElementToBitmapTest, GreenCircleCenterGreenCornerTransparent) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kGreenCircleSvg));

  const std::optional<SVGElement> circle = ElementById(app, "c");
  ASSERT_TRUE(circle.has_value());

  const RendererBitmap bitmap = RenderElementToBitmap(*circle, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());

  // Center is inside the circle: green and opaque.
  EXPECT_THAT(PixelAt(bitmap, 12, 12), RgbaNear(0, 200, 0, 255, 6));
  // A corner is outside the circle's bounds: fully transparent (the renderer
  // produced a transparent-background thumbnail, no ImGui backdrop).
  EXPECT_THAT(PixelAt(bitmap, 0, 0), RgbaNear(0, 0, 0, 0, 2));
}

TEST(RenderElementToBitmapTest, GroupComposesDescendants) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTwoHalvesGroupSvg));

  const std::optional<SVGElement> group = ElementById(app, "g");
  ASSERT_TRUE(group.has_value());

  const RendererBitmap bitmap = RenderElementToBitmap(*group, Vector2i(24, 24));
  ASSERT_FALSE(bitmap.empty());

  // The group's two halves compose into one preview: left blue, right yellow.
  EXPECT_THAT(PixelAt(bitmap, 6, 12), RgbaNear(0, 0, 210, 255, 6));
  EXPECT_THAT(PixelAt(bitmap, 17, 12), RgbaNear(210, 210, 0, 255, 6));
}

TEST(RenderElementToBitmapTest, ZeroSizeReturnsEmpty) {
  editor::EditorApp app;
  ASSERT_TRUE(app.loadFromString(kRedRectSvg));
  const std::optional<SVGElement> rect = ElementById(app, "r");
  ASSERT_TRUE(rect.has_value());

  EXPECT_TRUE(RenderElementToBitmap(*rect, Vector2i(0, 24)).empty());
  EXPECT_TRUE(RenderElementToBitmap(*rect, Vector2i(24, -3)).empty());
}

}  // namespace
}  // namespace donner::svg
