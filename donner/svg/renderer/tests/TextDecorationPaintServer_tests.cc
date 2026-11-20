/// @file
/// Paint servers on text decorations. SVG 2 (section 11.11) paints a decoration with the fill and
/// stroke of the element that declares it, so a gradient there paints the lines as it paints that
/// element's glyphs: an objectBoundingBox gradient maps onto the text element's bounding box, not
/// onto the line's own box. Each scene is compared with a scene of `<rect>` bands painted with the
/// equivalent userSpaceOnUse gradient, on the same renderer.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/tests/DecorationTestScene.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

using tests::RenderDecorationScene;

// Every scene sets its text at x=10 and baseline y=40 with font-size 20 and an 8px decoration
// thickness, so with the decoration test backend the glyph cells span y from 24 to 44, each
// glyph advances 10px and draws a 1x2 rectangle at its origin, and the bands below cover whole
// pixels.
constexpr std::string_view kDecorated = "font-size='20' style='text-decoration-thickness:8px";

/// The top of each decoration band at baseline 40.
constexpr int kUnderlineTop = 38;
constexpr int kOverlineTop = 20;
constexpr int kLineThroughTop = 30;

/// An objectBoundingBox gradient `#box` and a userSpaceOnUse gradient `#user` with the same stops;
/// \p boxAttributes and \p userAttributes position each one.
std::string Gradients(std::string_view boxAttributes, std::string_view userAttributes) {
  constexpr std::string_view kStops =
      "<stop stop-color='#e02020'/><stop offset='1' stop-color='#2040e0'/>";
  return "<defs><linearGradient id='box' " + std::string(boxAttributes) + ">" +
         std::string(kStops) + "</linearGradient><linearGradient id='user' " +
         "gradientUnits='userSpaceOnUse' " + std::string(userAttributes) + ">" +
         std::string(kStops) + "</linearGradient></defs>";
}

/// A horizontal gradient across the text box from x=10 to \p textRight.
std::string HorizontalGradients(int textRight) {
  return Gradients("", "x1='10' x2='" + std::to_string(textRight) + "' y1='0' y2='0'");
}

/// A decoration band of the 8px thickness, \p width wide, at \p x and \p top.
std::string Band(int x, int top, int width, std::string_view paint) {
  return "<rect x='" + std::to_string(x) + "' y='" + std::to_string(top) + "' width='" +
         std::to_string(width) + "' height='8' " + std::string(paint) + "/>";
}

/// The glyph rectangles of \p count glyphs starting at \p x.
std::string Glyphs(int x, int count, std::string_view paint) {
  std::string result;
  for (int index = 0; index < count; ++index) {
    result += "<rect x='" + std::to_string(x + 10 * index) + "' y='38' width='1' height='2' " +
              std::string(paint) + "/>";
  }
  return result;
}

/// Fails unless \p bitmap has a visible pixel, so a comparison of two empty renders cannot pass.
void ExpectVisible(const RendererBitmap& bitmap, std::string_view label) {
  EXPECT_THAT(test::CountNonTransparentPixels(bitmap), testing::Gt(0u)) << label;
}

/// Renders \p actual and \p expected and requires them to be identical and visible.
void ExpectSceneMatches(std::string_view actual, std::string_view expected,
                        std::string_view label) {
  const RendererBitmap expectedBitmap = RenderDecorationScene(expected);
  ExpectVisible(expectedBitmap, label);
  editor::tests::CompareBitmapToBitmap(RenderDecorationScene(actual), expectedBitmap, label,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST(TextDecorationPaintServer, GradientFillPaintsEachLineAcrossTheTextBox) {
  struct Line {
    std::string_view name;
    int top;
    bool beneathText;
  };
  constexpr Line kLines[] = {
      {"underline", kUnderlineTop, true},
      {"overline", kOverlineTop, true},
      {"line-through", kLineThroughTop, false},
  };
  constexpr std::string_view kPaint = "fill='url(#user)'";
  for (const Line& line : kLines) {
    SCOPED_TRACE(line.name);
    const std::string band = Band(10, line.top, 20, kPaint);
    const std::string glyphs = Glyphs(10, 2, kPaint);
    ExpectSceneMatches(HorizontalGradients(30) + "<text x='10' y='40' fill='url(#box)' " +
                           std::string(kDecorated) +
                           ";text-decoration-line:" + std::string(line.name) + "'>XX</text>",
                       HorizontalGradients(30) + (line.beneathText ? band + glyphs : glyphs + band),
                       line.name);
  }
}

TEST(TextDecorationPaintServer, GradientFillPaintsAllThreeLines) {
  // Each line comes from its own element, which keeps every band at its own position, and each
  // element paints with the gradient its fill inherits.
  constexpr std::string_view kPaint = "fill='url(#user)'";
  constexpr std::string_view kThickness = "text-decoration-thickness:8px";
  ExpectSceneMatches(
      HorizontalGradients(30) + "<text x='10' y='40' fill='url(#box)' " + std::string(kDecorated) +
          ";text-decoration-line:underline'><tspan style='text-decoration-line:overline;" +
          std::string(kThickness) + "'><tspan style='text-decoration-line:line-through;" +
          std::string(kThickness) + "'>XX</tspan></tspan></text>",
      HorizontalGradients(30) + Band(10, kUnderlineTop, 20, kPaint) +
          Band(10, kOverlineTop, 20, kPaint) + Glyphs(10, 2, kPaint) +
          Band(10, kLineThroughTop, 20, kPaint),
      "all_lines");
}

TEST(TextDecorationPaintServer, GradientStrokeOutlinesTheLine) {
  constexpr std::string_view kPaint = "fill='none' stroke='url(#user)' stroke-width='2'";
  ExpectSceneMatches(
      HorizontalGradients(30) +
          "<text x='10' y='40' fill='none' stroke='url(#box)' stroke-width='2' " +
          std::string(kDecorated) + ";text-decoration-line:underline'>XX</text>",
      HorizontalGradients(30) + Band(10, kUnderlineTop, 20, kPaint) + Glyphs(10, 2, kPaint),
      "stroke");
}

TEST(TextDecorationPaintServer, UserSpaceGradientKeepsItsCoordinates) {
  const std::string gradient =
      "<defs><linearGradient id='g' gradientUnits='userSpaceOnUse' x1='0' x2='100' y1='0' "
      "y2='0'><stop stop-color='#20a040'/><stop offset='1' stop-color='#a020a0'/>"
      "</linearGradient></defs>";
  constexpr std::string_view kPaint = "fill='url(#g)' stroke='url(#g)' stroke-width='2'";
  ExpectSceneMatches(gradient + "<text x='10' y='40' " + std::string(kPaint) + " " +
                         std::string(kDecorated) + ";text-decoration-line:underline'>XX</text>",
                     gradient + Band(10, kUnderlineTop, 20, kPaint) + Glyphs(10, 2, kPaint),
                     "user_space");
}

TEST(TextDecorationPaintServer, GradientTransformAppliesInTheTextBox) {
  // The text box is 20x20 at (10, 24), so its unit square maps through matrix(20 0 0 20 10 24).
  const std::string gradients =
      Gradients("gradientTransform='rotate(30 0.5 0.5)'",
                "x1='0' x2='1' y1='0' y2='0' gradientTransform='matrix(20 0 0 20 10 24) "
                "rotate(30 0.5 0.5)'");
  constexpr std::string_view kPaint = "fill='url(#user)'";
  ExpectSceneMatches(gradients + "<text x='10' y='40' fill='url(#box)' " + std::string(kDecorated) +
                         ";text-decoration-line:underline'>XX</text>",
                     gradients + Band(10, kUnderlineTop, 20, kPaint) + Glyphs(10, 2, kPaint),
                     "gradient_transform");
}

TEST(TextDecorationPaintServer, SpanDecorationMapsOntoTheTextBoxNotItsLine) {
  // The span's underline covers x from 30 to 50, while the text box spans x from 10 to 50 and y
  // from 24 to 44, so a gradient mapped onto the line's own box paints different colors, for a
  // fill and for a stroke.
  struct Case {
    std::string_view label;
    std::string gradients;
    std::string_view spanPaint;
    std::string_view referencePaint;
  };
  const Case kCases[] = {
      {"horizontal", HorizontalGradients(50), "fill='url(#box)'", "fill='url(#user)'"},
      {"vertical", Gradients("x2='0' y2='1'", "x1='0' x2='0' y1='24' y2='44'"), "fill='url(#box)'",
       "fill='url(#user)'"},
      {"horizontal_stroke", HorizontalGradients(50),
       "fill='none' stroke='url(#box)' stroke-width='2'",
       "fill='none' stroke='url(#user)' stroke-width='2'"},
  };
  for (const Case& testCase : kCases) {
    SCOPED_TRACE(testCase.label);
    ExpectSceneMatches(testCase.gradients +
                           "<text x='10' y='40' font-size='20' fill='black'>XX<tspan " +
                           std::string(testCase.spanPaint) +
                           " style='text-decoration:underline;"
                           "text-decoration-thickness:8px'>XX</tspan></text>",
                       testCase.gradients + Glyphs(10, 2, "fill='black'") +
                           Band(30, kUnderlineTop, 20, testCase.referencePaint) +
                           Glyphs(30, 2, testCase.referencePaint),
                       testCase.label);
  }
}

TEST(TextDecorationPaintServer, PaintAndSpanOpacityApply) {
  struct Case {
    std::string_view label;
    std::string_view textPaint;
    std::string_view content;
    std::string_view referencePaint;
    std::string_view referenceGroup;
  };
  // A span's `opacity` multiplies each paint drawn over the span, its lines included, so the
  // reference repeats it on each band and glyph; the text element's own `opacity` is a group.
  constexpr Case kCases[] = {
      {"fill_opacity", "fill='url(#box)' fill-opacity='0.5'", "XX",
       "fill='url(#user)' fill-opacity='0.5'", ""},
      {"stroke_opacity", "fill='none' stroke='url(#box)' stroke-width='2' stroke-opacity='0.5'",
       "XX", "fill='none' stroke='url(#user)' stroke-width='2' stroke-opacity='0.5'", ""},
      {"span_opacity", "fill='url(#box)'", "<tspan opacity='0.5'>XX</tspan>",
       "fill='url(#user)' fill-opacity='0.5'", ""},
      {"text_opacity", "fill='url(#box)' opacity='0.5'", "XX", "fill='url(#user)'",
       "opacity='0.5'"},
  };
  for (const Case& testCase : kCases) {
    SCOPED_TRACE(testCase.label);
    ExpectSceneMatches(HorizontalGradients(30) + "<text x='10' y='40' " +
                           std::string(testCase.textPaint) + " " + std::string(kDecorated) +
                           ";text-decoration-line:underline'>" + std::string(testCase.content) +
                           "</text>",
                       HorizontalGradients(30) + "<g " + std::string(testCase.referenceGroup) +
                           ">" + Band(10, kUnderlineTop, 20, testCase.referencePaint) +
                           Glyphs(10, 2, testCase.referencePaint) + "</g>",
                       testCase.label);
  }
}

TEST(TextDecorationPaintServer, GradientWithFallbackColorPaintsTheGradient) {
  // A fallback color paints only where its gradient cannot, so a drawable gradient paints the
  // lines as it paints the glyphs.
  struct Case {
    std::string_view label;
    std::string_view textPaint;
    std::string_view referencePaint;
  };
  constexpr Case kCases[] = {
      {"fill", "fill='url(#box) #20a040'", "fill='url(#user)'"},
      {"stroke", "fill='none' stroke='url(#box) #20a040' stroke-width='2'",
       "fill='none' stroke='url(#user)' stroke-width='2'"},
  };
  for (const Case& testCase : kCases) {
    SCOPED_TRACE(testCase.label);
    ExpectSceneMatches(
        HorizontalGradients(30) + "<text x='10' y='40' " + std::string(testCase.textPaint) + " " +
            std::string(kDecorated) + ";text-decoration-line:underline'>XX</text>",
        HorizontalGradients(30) + Band(10, kUnderlineTop, 20, testCase.referencePaint) +
            Glyphs(10, 2, testCase.referencePaint),
        testCase.label);
  }
}

TEST(TextDecorationPaintServer, DeclaringElementPaintsTheLineOverATransparentSpan) {
  // The text element declares the underline, so the line keeps the element's gradient where the
  // span's transparent fill paints no glyphs.
  ExpectSceneMatches(HorizontalGradients(30) + "<text x='10' y='40' fill='url(#box)' " +
                         std::string(kDecorated) +
                         ";text-decoration-line:underline'><tspan fill='transparent'>XX</tspan>"
                         "</text>",
                     HorizontalGradients(30) + Band(10, kUnderlineTop, 20, "fill='url(#user)'"),
                     "transparent_span");
}

TEST(TextDecorationPaintServer, TextPathAndTransformedTextMapOntoTheTextBox) {
  constexpr std::string_view kPaint = "fill='url(#user)'";
  // A straight path along the baseline places the glyphs where plain text places them.
  ExpectSceneMatches(
      HorizontalGradients(30) +
          "<path id='baseline' d='M 10 40 H 90' fill='none'/><text fill='url(#box)' " +
          std::string(kDecorated) +
          ";text-decoration-line:underline'><textPath href='#baseline'>XX</textPath>"
          "</text>",
      HorizontalGradients(30) + Band(10, kUnderlineTop, 20, kPaint) + Glyphs(10, 2, kPaint),
      "text_path");
  ExpectSceneMatches(HorizontalGradients(30) +
                         "<text x='10' y='40' transform='scale(2) translate(-5 -15)' "
                         "fill='url(#box)' " +
                         std::string(kDecorated) + ";text-decoration-line:underline'>XX</text>",
                     HorizontalGradients(30) + "<g transform='scale(2) translate(-5 -15)'>" +
                         Band(10, kUnderlineTop, 20, kPaint) + Glyphs(10, 2, kPaint) + "</g>",
                     "scaled");
}

}  // namespace
}  // namespace donner::svg
