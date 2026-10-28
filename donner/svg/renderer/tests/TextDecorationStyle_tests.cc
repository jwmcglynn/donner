/// @file
/// `text-decoration-style` geometry and `text-decoration-color` paint, compared exactly against
/// independently authored scenes. The proportions follow the documented Donner rules on
/// \ref donner::svg::TextDecorationStyle.

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/DecorationTestScene.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

namespace donner::svg {
namespace {

using tests::RenderDecorationScene;

constexpr std::string_view kRedGlyphs =
    "<rect x='10' y='38' width='1' height='2' fill='red'/>"
    "<rect x='20' y='38' width='1' height='2' fill='red'/>";

/// A two-glyph run from x=10 to x=30 at baseline 40 with a 2px decoration of the given style.
std::string DecoratedText(std::string_view decoration, std::string_view extraAttributes = "") {
  return "<text x='10' y='40' font-size='20' fill='red' " + std::string(extraAttributes) +
         " style='text-decoration:" + std::string(decoration) +
         ";text-decoration-thickness:2px'>XX</text>";
}

void ExpectScene(const std::string& actualSvg, const std::string& expectedSvg,
                 std::string_view label) {
  editor::tests::CompareBitmapToBitmap(RenderDecorationScene(actualSvg),
                                       RenderDecorationScene(expectedSvg), label,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationStyle, SolidIsOneBand) {
  ExpectScene(DecoratedText("underline solid"),
              "<rect x='10' y='41' width='20' height='2' fill='red'/>" + std::string(kRedGlyphs),
              "decoration_style_solid");
}

TEST(TextDecorationStyle, DoubleAddsASecondBandAwayFromTheText) {
  struct Case {
    const char* line;
    int first;
    int second;
    bool beneathText;
  };
  // Underline grows down, overline grows up, and line-through straddles its solid position.
  for (const Case& c : {Case{"underline", 41, 45, true}, Case{"overline", 23, 19, true},
                        Case{"line-through", 31, 35, false}}) {
    SCOPED_TRACE(c.line);
    const std::string bands = "<rect x='10' y='" + std::to_string(c.first) +
                              "' width='20' height='2' fill='red'/><rect x='10' y='" +
                              std::to_string(c.second) + "' width='20' height='2' fill='red'/>";
    ExpectScene(DecoratedText(std::string(c.line) + " double"),
                c.beneathText ? bands + std::string(kRedGlyphs) : std::string(kRedGlyphs) + bands,
                c.line);
  }
}

TEST(TextDecorationStyle, DashedRepeatsThreeThicknessDashesEveryFive) {
  ExpectScene(DecoratedText("underline dashed"),
              "<rect x='10' y='41' width='6' height='2' fill='red'/>"
              "<rect x='20' y='41' width='6' height='2' fill='red'/>" +
                  std::string(kRedGlyphs),
              "decoration_style_dashed");
}

/// Path data for a unit-radius dot: four cubic quarter-circles with the standard 0.5522847498
/// control distance. A `<circle>` element may use a backend's own circle primitive instead, so the
/// scene spells out the curve.
std::string DotPathData(double cx, double cy) {
  constexpr double k = 0.5522847498;
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                "M%.10g %.10g C%.10g %.10g %.10g %.10g %.10g %.10g "
                "C%.10g %.10g %.10g %.10g %.10g %.10g C%.10g %.10g %.10g %.10g %.10g %.10g "
                "C%.10g %.10g %.10g %.10g %.10g %.10g Z ",
                cx + 1, cy, cx + 1, cy + k, cx + k, cy + 1, cx, cy + 1, cx - k, cy + 1, cx - 1,
                cy + k, cx - 1, cy, cx - 1, cy - k, cx - k, cy - 1, cx, cy - 1, cx + k, cy - 1,
                cx + 1, cy - k, cx + 1, cy);
  return buffer;
}

TEST(TextDecorationStyle, DottedRepeatsRoundDotsEveryTwoThicknesses) {
  // One shape holds every dot of the line, as one decoration line is one painted shape.
  std::string dots = "<path fill='red' d='";
  for (const int cx : {11, 15, 19, 23, 27}) {
    dots += DotPathData(cx, 42);
  }
  dots += "'/>";
  ExpectScene(DecoratedText("underline dotted"), dots + std::string(kRedGlyphs),
              "decoration_style_dotted");
}

TEST(TextDecorationStyle, WavyFollowsWholeHalfWavesAcrossTheSegment) {
  // Five half-waves of 4px (the 20px run divided into the count nearest 2t), amplitude 2px,
  // forming a 2px band between the top and bottom wave edges.
  constexpr std::string_view kWave =
      "<path fill='red' d='M10 41 Q12 37 14 41 Q16 45 18 41 Q20 37 22 41 Q24 45 26 41 Q28 37 30 41 "
      "L30 43 Q28 39 26 43 Q24 47 22 43 Q20 39 18 43 Q16 47 14 43 Q12 39 10 43 Z'/>";
  ExpectScene(DecoratedText("underline wavy"), std::string(kWave) + std::string(kRedGlyphs),
              "decoration_style_wavy");
}

TEST(TextDecorationStyle, StyleAppliesPerOrigin) {
  // The span's dotted line-through does not restyle the ancestor's solid underline.
  ExpectScene(
      "<text x='10' y='40' font-size='20' fill='red' "
      "style='text-decoration:underline;text-decoration-thickness:2px'><tspan "
      "style='text-decoration:line-through dashed;text-decoration-thickness:2px'>XX</tspan></text>",
      "<rect x='10' y='41' width='20' height='2' fill='red'/>" + std::string(kRedGlyphs) +
          "<rect x='10' y='33' width='6' height='2' fill='red'/>"
          "<rect x='20' y='33' width='6' height='2' fill='red'/>",
      "decoration_style_per_origin");
}

TEST(TextDecorationColor, ReplacesTheDeclaringFill) {
  ExpectScene(DecoratedText("underline blue"),
              "<rect x='10' y='41' width='20' height='2' fill='blue'/>" + std::string(kRedGlyphs),
              "decoration_color_fill");
}

TEST(TextDecorationColor, PaintsEvenWhenTheDeclaringFillIsNone) {
  ExpectScene(
      "<text x='10' y='40' font-size='20' fill='none' style='text-decoration:underline blue;"
      "text-decoration-thickness:2px'>XX</text>",
      "<rect x='10' y='41' width='20' height='2' fill='blue'/>", "decoration_color_fill_none");
}

TEST(TextDecorationColor, KeepsTheDeclaringStroke) {
  constexpr std::string_view kStroke = "stroke='lime' stroke-width='1'";
  ExpectScene(DecoratedText("underline blue", kStroke),
              "<rect x='10' y='41' width='20' height='2' fill='blue' " + std::string(kStroke) +
                  "/><rect x='10' y='38' width='1' height='2' fill='red' " + std::string(kStroke) +
                  "/><rect x='20' y='38' width='1' height='2' fill='red' " + std::string(kStroke) +
                  "/>",
              "decoration_color_stroke");
}

TEST(TextDecorationColor, CurrentColorUsesTheDeclaringElementColor) {
  ExpectScene(
      "<text x='10' y='40' font-size='20' fill='red' color='green'><tspan color='blue' "
      "style='text-decoration:underline;text-decoration-color:currentcolor;"
      "text-decoration-thickness:2px'>XX</tspan></text>",
      "<rect x='10' y='41' width='20' height='2' fill='blue'/>" + std::string(kRedGlyphs),
      "decoration_color_currentcolor");
}

TEST(TextDecorationColor, UnsetKeepsTheSvgFillRule) {
  // Without text-decoration-color, SVG 2 section 11.11 paints the decoration with the declaring
  // element's fill, not the span's.
  ExpectScene(
      "<text x='10' y='40' font-size='20' fill='green' style='text-decoration:underline;"
      "text-decoration-thickness:2px'><tspan fill='red'>XX</tspan></text>",
      "<rect x='10' y='41' width='20' height='2' fill='green'/>" + std::string(kRedGlyphs),
      "decoration_color_unset");
}

}  // namespace
}  // namespace donner::svg
