/// @file
/// `text-decoration-style` geometry and `text-decoration-color` paint, compared exactly against
/// independently authored scenes. The proportions follow the documented Donner rules on
/// \ref donner::svg::TextDecorationStyle.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/renderer/tests/DecorationTestScene.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg {
namespace {

using tests::DecorationDocument;
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

/// Emit one origin's underline for a single-glyph run and return the emitted paths' point counts.
std::vector<std::size_t> EmitUnderline(TextDecorationStyle style, double x, double advance,
                                       double thickness,
                                       const Transform2d& deviceFromLocal = Transform2d()) {
  auto document = DecorationDocument("");
  const auto& engine = document.registry().ctx().get<TextEngine>();
  TextRun run;
  TextGlyph glyph;
  glyph.glyphIndex = 3;
  glyph.xPosition = x;
  glyph.xAdvance = advance;
  run.glyphs.push_back(glyph);
  components::TextDecorationOrigin origin;
  origin.lines = TextDecoration::Underline;
  origin.style = style;
  origin.thickness = {TextDecorationThickness::Kind::Length, Lengthd(thickness)};
  std::size_t remaining = Path::kMaximumGeometryQueryWork;
  std::vector<std::size_t> points;
  EmitTextDecorationPaths(run, origin, origin.lines, 1, 20, engine, deviceFromLocal, remaining,
                          [&](const Path& path) { points.push_back(path.points().size()); });
  return points;
}

TEST(TextDecorationStyle, StraightSegmentsRespectTheExactSharedAllowance) {
  auto document = DecorationDocument("");
  const auto& engine = document.registry().ctx().get<TextEngine>();
  TextRun run;
  for (int index = 0; index < 2; ++index) {
    TextGlyph glyph;
    glyph.glyphIndex = 3;
    glyph.xPosition = index * 10;
    glyph.yPosition = index;
    glyph.xAdvance = 10;
    run.glyphs.push_back(glyph);
  }
  components::TextDecorationOrigin origin;
  origin.lines = TextDecoration::Underline;
  origin.style = TextDecorationStyle::Solid;
  origin.thickness = {TextDecorationThickness::Kind::Length, Lengthd(2)};
  for (const std::size_t allowance : {15u, 16u}) {
    SCOPED_TRACE(allowance);
    std::size_t remaining = allowance;
    std::vector<std::size_t> points;
    EmitTextDecorationPaths(run, origin, origin.lines, 1, 20, engine, Transform2d(), remaining,
                            [&](const Path& path) { points.push_back(path.points().size()); });
    EXPECT_EQ(remaining, 0u);
    if (allowance == 16) {
      EXPECT_THAT(points, testing::ElementsAre(8u));
    } else {
      EXPECT_THAT(points, testing::IsEmpty());
    }
  }
}

TEST(TextDecorationStyle, DashesCoverAnExactMultipleOfThePeriod) {
  // 20 units at a 2 unit thickness is exactly two 10 unit periods: two dashes, four points each.
  EXPECT_THAT(EmitUnderline(TextDecorationStyle::Dashed, 10, 20, 2), testing::ElementsAre(8u));
}

TEST(TextDecorationStyle, DashesTerminateWhereThePeriodIsBelowCoordinatePrecision) {
  // Near 1e20 adjacent doubles are 16384 apart, so adding the 8 unit dash period to a position
  // cannot advance it. Element counts come from the segment length (16384 / 3.2 = 5120 dots or
  // half-waves); every dash collapses to zero width and is rejected.
  EXPECT_THAT(EmitUnderline(TextDecorationStyle::Dashed, 1e20, 16384, 1.6), testing::IsEmpty());
  EXPECT_THAT(EmitUnderline(TextDecorationStyle::Dotted, 1e20, 16384, 1.6),
              testing::ElementsAre(13u * 5120u));
  EXPECT_THAT(EmitUnderline(TextDecorationStyle::Wavy, 1e20, 16384, 1.6),
              testing::ElementsAre(2u + 4u * 5120u));
}

TEST(TextDecorationStyle, LongThinStyledLinesStillDraw) {
  // Ten thousand dots on one baseline: bounded by the per-draw work allowance, not dropped.
  EXPECT_THAT(EmitUnderline(TextDecorationStyle::Dotted, 0, 20000, 1),
              testing::ElementsAre(13u * 10000u));
  EXPECT_THAT(EmitUnderline(TextDecorationStyle::Wavy, 0, 20000, 1),
              testing::ElementsAre(2u + 4u * 10000u));
}

TEST(TextDecorationStyle, SingularTransformsSuppressEveryStyle) {
  Transform2d singular;
  singular.data[0] = singular.data[1] = singular.data[2] = singular.data[3] = 0;
  for (const auto style :
       {TextDecorationStyle::Solid, TextDecorationStyle::Double, TextDecorationStyle::Dotted,
        TextDecorationStyle::Dashed, TextDecorationStyle::Wavy}) {
    SCOPED_TRACE(testing::PrintToString(style));
    EXPECT_THAT(EmitUnderline(style, 10, 20, 0, singular), testing::IsEmpty());
  }
}

TEST(TextDecorationStyle, DecorationWorkIsSharedAcrossTheFrame) {
  // Five offscreen texts each ask for a 1,800,002 point wavy line: 45 glyphs of 10 units at a
  // 0.0005 unit thickness, scaled 4000x so the one device pixel minimum does not widen it. That
  // exceeds the per-draw allowance, so each is refused before any geometry is built and spends the
  // rest of that draw's allowance. Four such refusals use up the frame's 4 Mi work units, so the
  // visible underline drawn after them gets no allowance, although no geometry was ever drawn.
  std::string scene = "<g transform='scale(4000)'>";
  for (int i = 0; i < 5; ++i) {
    scene +=
        "<text x='-1000' y='-1000' font-size='20' fill='red' style='text-decoration:"
        "underline wavy;text-decoration-thickness:0.0005px'>" +
        std::string(45, 'X') + "</text>";
  }
  scene += "</g>";
  ExpectScene(scene + DecoratedText("underline"), std::string(kRedGlyphs),
              "decoration_frame_allowance");
  // Control: without the offscreen texts, the same underline draws.
  ExpectScene(DecoratedText("underline"),
              "<rect x='10' y='41' width='20' height='2' fill='red'/>" + std::string(kRedGlyphs),
              "decoration_frame_allowance_control");
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
