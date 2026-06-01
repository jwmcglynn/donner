/// @file
/// Unit tests for the Milestone 5 "Convert Text to Outlines" conversion
/// (`docs/design_docs/0047-v0_8_showcase.md` §"Convert Text to Outlines",
/// §"Error Handling", §"Performance", and §"Testing and Validation" →
/// `text_outline_tests`).
///
/// These run in the DEFAULT basic-text (stb_truetype) configuration: the test
/// `<text>` uses the engine's embedded fallback font (Public Sans), so glyph
/// outlines are produced hermetically with no checked-in font and no
/// `--config=text-full` requirement.

#include "donner/editor/TextToOutlines.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

namespace {

using ::testing::HasSubstr;

constexpr int kCanvas = 200;

/// Parse \p svg into a source-backed SVGDocument, asserting success. Source
/// backing is required so `convertTextToOutlines` can splice the `<text>` node's
/// source range.
svg::SVGDocument Parse(std::string_view svg) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svg, sink);
  EXPECT_FALSE(result.hasError()) << "parse failed for: " << svg;
  return result.result();
}

/// Resolve the first `<text>` element in \p document.
svg::SVGElement TextElement(svg::SVGDocument& document) {
  auto element = document.querySelector("text");
  EXPECT_TRUE(element.has_value());
  return *element;
}

/// Render \p document to a `RendererBitmap` at the canvas size, using the same
/// `svg::Renderer::draw` + `takeSnapshot` path as `DocumentSave_tests`.
svg::RendererBitmap RenderToBitmap(svg::SVGDocument& document) {
  document.setCanvasSize(kCanvas, kCanvas);
  svg::Renderer renderer;
  renderer.draw(document);
  return renderer.takeSnapshot();
}

/// SVG with a single `<text>SVG</text>` rendered at a large size so the glyph
/// outlines cover many pixels. Uses the default fallback font (no @font-face).
constexpr std::string_view kTextSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
<text id="logo" x="10" y="120" font-size="64" fill="#0033aa">SVG</text>
</svg>)";

}  // namespace

// Converts `SVG` text to path geometry: the merged source contains `<path>`
// elements and the reparsed document has no live `<text>` element.
TEST(TextToOutlines, ConvertsToPathGeometryWithNoLiveText) {
  svg::SVGDocument document = Parse(kTextSvg);
  svg::SVGElement text = TextElement(document);

  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  ASSERT_TRUE(result.ok) << result.error;

  EXPECT_THAT(result.mergedSource, HasSubstr("<path"));
  EXPECT_THAT(result.mergedSource, HasSubstr("data-donner-converted-from=\"text\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("id=\"logo_outlines\""));
  EXPECT_EQ(result.outlineGroupId, "logo_outlines");

  svg::SVGDocument converted = Parse(result.mergedSource);
  EXPECT_FALSE(converted.querySelector("text").has_value())
      << "converted document must contain no live <text>";
  EXPECT_TRUE(converted.querySelector("#logo_outlines").has_value());
  EXPECT_TRUE(converted.querySelector("#logo_outlines_0").has_value());
}

// Three "SVG" glyphs produce three path elements (one per glyph).
TEST(TextToOutlines, EmitsOnePathPerGlyph) {
  svg::SVGDocument document = Parse(kTextSvg);
  svg::SVGElement text = TextElement(document);

  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  ASSERT_TRUE(result.ok) << result.error;

  // "SVG" → three glyphs, none of which are whitespace, so three `<path>`s.
  size_t pathCount = 0;
  for (size_t pos = 0; (pos = result.mergedSource.find("<path", pos)) != std::string::npos;
       pos += 5) {
    ++pathCount;
  }
  EXPECT_EQ(pathCount, 3u);
}

// Pixel-compare: rendering the document BEFORE conversion and AFTER conversion
// must match exactly (zero differing pixels). Both render the same placed glyph
// outlines via the same `TextEngine::computedGlyphPaths()` geometry.
TEST(TextToOutlines, PixelCompareBeforeAndAfterMatches) {
  svg::SVGDocument before = Parse(kTextSvg);
  svg::SVGElement text = TextElement(before);

  const ConvertTextToOutlinesResult result = convertTextToOutlines(before, text);
  ASSERT_TRUE(result.ok) << result.error;

  // Re-parse `before` from scratch so the conversion's lazy text geometry cache
  // does not perturb the baseline render.
  svg::SVGDocument beforeFresh = Parse(kTextSvg);
  svg::SVGDocument after = Parse(result.mergedSource);

  const svg::RendererBitmap beforeBitmap = RenderToBitmap(beforeFresh);
  const svg::RendererBitmap afterBitmap = RenderToBitmap(after);

  // Zero-diff identity expectation: both renders fill the same placed glyph
  // outlines, so any differing pixel is a real geometry/coordinate bug.
  // `CompareBitmapToBitmap` adds gtest failures and writes
  // actual_/expected_/diff_ PNGs to $TEST_UNDECLARED_OUTPUTS_DIR on mismatch.
  tests::CompareBitmapToBitmap(afterBitmap, beforeBitmap, "text_to_outlines_before_after",
                               tests::PixelmatchIdentityParams());
}

// Preserves fill, opacity, transform, fill-rule, stroke, and paint order: the
// authored presentation attributes on `<text>` are carried onto the outline
// group, and the group replaces the text in situ (same source position).
TEST(TextToOutlines, PreservesStyleAndPaintOrder) {
  // Custom raw-string delimiter `SVG(...)SVG`: the `transform="translate(5,5)"`
  // attribute contains the byte sequence `)"`, which would prematurely close the
  // default `R"(...)"` delimiter.
  constexpr std::string_view kStyledSvg =
      R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
<rect id="under" x="0" y="0" width="200" height="200" fill="white"/>
<text id="t" x="10" y="120" font-size="64" fill="#0033aa" fill-rule="evenodd" opacity="0.5" stroke="black" stroke-width="2" transform="translate(5,5)">SVG</text>
<rect id="over" x="0" y="0" width="5" height="5" fill="red"/>
</svg>)SVG";
  svg::SVGDocument document = Parse(kStyledSvg);
  svg::SVGElement text = TextElement(document);

  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  ASSERT_TRUE(result.ok) << result.error;

  // Style is carried onto the `<g>` group.
  EXPECT_THAT(result.mergedSource, HasSubstr("fill=\"#0033aa\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("fill-rule=\"evenodd\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("opacity=\"0.5\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("stroke=\"black\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("stroke-width=\"2\""));
  EXPECT_THAT(result.mergedSource, HasSubstr("transform=\"translate(5,5)\""));

  // Paint order: the group sits between #under and #over, exactly where the
  // original `<text>` was.
  const size_t underPos = result.mergedSource.find("id=\"under\"");
  const size_t groupPos = result.mergedSource.find("id=\"t_outlines\"");
  const size_t overPos = result.mergedSource.find("id=\"over\"");
  ASSERT_NE(underPos, std::string::npos);
  ASSERT_NE(groupPos, std::string::npos);
  ASSERT_NE(overPos, std::string::npos);
  EXPECT_LT(underPos, groupPos);
  EXPECT_LT(groupPos, overPos);
}

// Apply path through AsyncSVGDocument: a ConvertTextToOutlines command swaps the
// document to the outlined source, and a subsequent ReplaceDocument with the
// original bytes (what undo replays) restores the live `<text>` element.
TEST(TextToOutlines, AppliesThroughAsyncDocumentAndUndoRestoresText) {
  const std::string originalSource(kTextSvg);
  svg::SVGDocument document = Parse(originalSource);
  svg::SVGElement text = TextElement(document);

  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  ASSERT_TRUE(result.ok) << result.error;

  AsyncSVGDocument async;
  async.setDocument(Parse(originalSource));
  ASSERT_TRUE(async.hasDocument());

  // Apply the conversion as a structural replace.
  async.queue().push(EditorCommand::ConvertTextToOutlinesCommand(result.mergedSource));
  ASSERT_TRUE(async.flushFrame());
  EXPECT_FALSE(async.document().querySelector("text").has_value());
  EXPECT_TRUE(async.document().querySelector("#logo_outlines").has_value());

  // Undo replays the pre-conversion source. Use the same preserve-undo reparse
  // path the undo timeline uses.
  async.queue().push(EditorCommand::ReplaceDocumentCommand(originalSource,
                                                           /*preserveUndoOnReparse=*/true));
  ASSERT_TRUE(async.flushFrame());
  EXPECT_TRUE(async.document().querySelector("text").has_value())
      << "undo must restore the live <text> element";
  EXPECT_FALSE(async.document().querySelector("#logo_outlines").has_value());
}

// Error handling: a `<text>` that produces no glyph outlines (empty content)
// fails the conversion and leaves the document completely unchanged.
TEST(TextToOutlines, EmptyOutlineFailureLeavesDocumentUnchanged) {
  constexpr std::string_view kEmptyTextSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
<text id="empty" x="10" y="120" font-size="64"></text>
</svg>)";
  svg::SVGDocument document = Parse(kEmptyTextSvg);
  svg::SVGElement text = TextElement(document);

  const std::string sourceBefore(document.source());
  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  EXPECT_FALSE(result.ok);
  EXPECT_THAT(result.error, HasSubstr("outline"));
  EXPECT_TRUE(result.mergedSource.empty());
  // The document source is untouched (convertTextToOutlines never mutates).
  EXPECT_EQ(std::string(document.source()), sourceBefore);
  EXPECT_TRUE(document.querySelector("text").has_value());
}

// Error handling: a non-`<text>` selection is rejected without mutation.
TEST(TextToOutlines, NonTextElementRejected) {
  constexpr std::string_view kRectSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
<rect id="r" x="0" y="0" width="10" height="10"/>
</svg>)";
  svg::SVGDocument document = Parse(kRectSvg);
  auto rect = document.querySelector("#r");
  ASSERT_TRUE(rect.has_value());

  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, *rect);
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.mergedSource.empty());
}

// Performance: converting a three-glyph "SVG" string completes well within
// budget. Design doc §"Performance" targets <=100 ms for three glyphs on an
// M-series Mac; the assertion uses a generous 500 ms ceiling for CI variance.
TEST(TextToOutlines, ThreeGlyphConversionWithinBudget) {
  svg::SVGDocument document = Parse(kTextSvg);
  svg::SVGElement text = TextElement(document);

  const auto start = std::chrono::steady_clock::now();
  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();

  ASSERT_TRUE(result.ok) << result.error;
  // Design-doc target is 100 ms; CI ceiling is intentionally generous.
  EXPECT_LT(elapsedMs, 500) << "three-glyph conversion took " << elapsedMs << " ms";
}

}  // namespace donner::editor
