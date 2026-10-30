/// @file
/// Painting order of text decorations relative to glyphs, and of each decoration's own fill and
/// stroke. CSS Text Decoration 3 paints underlines and overlines beneath the text and
/// line-throughs above it; SVG 2 (section 11.11) adopts that order and takes the decoration's
/// fill/stroke order from `paint-order` where the decoration is declared.

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/DecorationTestScene.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

namespace donner::svg {
namespace {

using tests::RenderDecorationScene;

/// Glyphs 20px tall, so every decoration line overlaps them.
constexpr double kTallGlyph = 20.0;

struct DecorationLine {
  const char* name;
  int top;  ///< Band top for an 8px thickness at baseline 40, font-size 20.
  bool beneathText;
};

constexpr DecorationLine kLines[] = {
    {"underline", 38, true},
    {"overline", 20, true},
    {"line-through", 30, false},
};

std::string Band(const DecorationLine& line, std::string_view paint) {
  return "<rect x='10' y='" + std::to_string(line.top) + "' width='20' height='8' " +
         std::string(paint) + "/>";
}

std::string GlyphRects(int y, int height, std::string_view paint) {
  std::string result;
  for (const int x : {10, 20}) {
    result += "<rect x='" + std::to_string(x) + "' y='" + std::to_string(y) +
              "' width='1' height='" + std::to_string(height) + "' " + std::string(paint) + "/>";
  }
  return result;
}

TEST(TextDecorationPaintOrder, UnderlineAndOverlineBeneathGlyphsLineThroughAbove) {
  for (const DecorationLine& line : kLines) {
    SCOPED_TRACE(line.name);
    const auto actual = RenderDecorationScene(
        "<text x='10' y='40' font-size='20' fill='red' style='text-decoration:" +
            std::string(line.name) +
            ";text-decoration-thickness:8px'><tspan fill='blue'>XX</tspan></text>",
        true, kTallGlyph);
    const std::string glyphs = GlyphRects(20, 20, "fill='blue'");
    const std::string band = Band(line, "fill='red'");
    const auto expected =
        RenderDecorationScene(line.beneathText ? band + glyphs : glyphs + band, true, kTallGlyph);
    editor::tests::CompareBitmapToBitmap(actual, expected, line.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST(TextDecorationPaintOrder, AncestorLinesKeepOrderAroundDescendantGlyphs) {
  // Two origins on the same run: the ancestor's underline and the span's line-through. Both keep
  // their line's position relative to the glyphs, whichever element declared them.
  const auto actual = RenderDecorationScene(
      R"svg(<text x="10" y="40" font-size="20" fill="red"
                  style="text-decoration:underline;text-decoration-thickness:8px"><tspan fill="blue"
                  style="text-decoration:line-through;text-decoration-thickness:8px">XX</tspan></text>)svg",
      true, kTallGlyph);
  const auto expected =
      RenderDecorationScene(Band(kLines[0], "fill='red'") + GlyphRects(20, 20, "fill='blue'") +
                                Band(kLines[2], "fill='blue'"),
                            true, kTallGlyph);
  editor::tests::CompareBitmapToBitmap(actual, expected, "decoration_origin_order",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationPaintOrder, DecorationFillAndStrokeFollowDeclaringPaintOrder) {
  // The glyphs share the declaring element's paint, so the scene repeats the same stroke-first
  // paint on the glyph rectangles in their own position in the order.
  constexpr std::string_view kPaint =
      "fill='red' stroke='blue' stroke-width='4' paint-order='stroke'";
  for (const DecorationLine& line : kLines) {
    SCOPED_TRACE(line.name);
    const auto actual =
        RenderDecorationScene("<text x='10' y='40' font-size='20' " + std::string(kPaint) +
                              " style='text-decoration:" + std::string(line.name) +
                              ";text-decoration-thickness:8px'>XX</text>");
    const std::string glyphs = GlyphRects(38, 2, kPaint);
    const std::string band = Band(line, kPaint);
    const auto expected = RenderDecorationScene(line.beneathText ? band + glyphs : glyphs + band);
    editor::tests::CompareBitmapToBitmap(actual, expected, line.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

}  // namespace
}  // namespace donner::svg
