/// @file
/// Unit tests for the "Convert Text to Outlines" conversion: outline
/// generation, error handling, performance, and undo/selection behavior.
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

/// Apply a prepared conversion through the structured DOM-editing APIs, the
/// same insert-group/insert-paths/delete-text sequence the editor shell queues
/// as `EditorCommand`s. Returns the post-conversion document source.
std::string ApplyConversion(svg::SVGDocument& document, svg::SVGElement text,
                            ConvertTextToOutlinesResult& result) {
  std::optional<svg::SVGElement> parent = text.parentElement();
  EXPECT_TRUE(parent.has_value());
  EXPECT_TRUE(result.outlineGroup.has_value());
  xml::ApplySourceEditResult groupInsert = document.insertElement(*parent, *result.outlineGroup, text);
  EXPECT_FALSE(groupInsert.diagnostic.has_value())
      << groupInsert.diagnostic.value_or(ParseDiagnostic()).reason;
  for (svg::SVGElement& path : result.outlinePaths) {
    xml::ApplySourceEditResult pathInsert = document.insertElement(*result.outlineGroup, path);
    EXPECT_FALSE(pathInsert.diagnostic.has_value())
        << pathInsert.diagnostic.value_or(ParseDiagnostic()).reason;
  }
  xml::ApplySourceEditResult removed = document.removeElement(text);
  EXPECT_FALSE(removed.diagnostic.has_value())
      << removed.diagnostic.value_or(ParseDiagnostic()).reason;
  return std::string(document.source());
}

}  // namespace

// Converts `SVG` text to path geometry: the converted source contains `<path>`
// elements and the document has no live `<text>` element.
TEST(TextToOutlines, ConvertsToPathGeometryWithNoLiveText) {
  svg::SVGDocument document = Parse(kTextSvg);
  svg::SVGElement text = TextElement(document);

  ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.outlineGroupId, "logo_outlines");

  const std::string mergedSource = ApplyConversion(document, text, result);
  EXPECT_THAT(mergedSource, HasSubstr("<path"));
  EXPECT_THAT(mergedSource, HasSubstr("data-donner-converted-from=\"text\""));
  EXPECT_THAT(mergedSource, HasSubstr("id=\"logo_outlines\""));

  // The live document reflects the structural edit without a reparse, and the
  // emitted source round-trips.
  EXPECT_FALSE(document.querySelector("text").has_value())
      << "converted document must contain no live <text>";
  EXPECT_TRUE(document.querySelector("#logo_outlines").has_value());
  EXPECT_TRUE(document.querySelector("#logo_outlines_0").has_value());

  svg::SVGDocument reparsed = Parse(mergedSource);
  EXPECT_FALSE(reparsed.querySelector("text").has_value());
  EXPECT_TRUE(reparsed.querySelector("#logo_outlines").has_value());
}

// Three "SVG" glyphs produce three path elements (one per glyph).
TEST(TextToOutlines, EmitsOnePathPerGlyph) {
  svg::SVGDocument document = Parse(kTextSvg);
  svg::SVGElement text = TextElement(document);

  const ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  ASSERT_TRUE(result.ok) << result.error;

  // "SVG" → three glyphs, none of which are whitespace, so three `<path>`s.
  EXPECT_EQ(result.outlinePaths.size(), 3u);
}

// Pixel-compare: rendering the document BEFORE conversion and AFTER conversion
// must match exactly (zero differing pixels). Both render the same placed glyph
// outlines via the same `TextEngine::computedGlyphPaths()` geometry.
TEST(TextToOutlines, PixelCompareBeforeAndAfterMatches) {
  svg::SVGDocument converted = Parse(kTextSvg);
  svg::SVGElement text = TextElement(converted);

  ConvertTextToOutlinesResult result = convertTextToOutlines(converted, text);
  ASSERT_TRUE(result.ok) << result.error;
  const std::string mergedSource = ApplyConversion(converted, text, result);

  // Parse the baseline from scratch so the conversion's lazy text geometry
  // cache does not perturb the baseline render; render the converted result
  // from its emitted source so the comparison also covers the round-trip.
  svg::SVGDocument beforeFresh = Parse(kTextSvg);
  svg::SVGDocument after = Parse(mergedSource);

  const svg::RendererBitmap beforeBitmap = RenderToBitmap(beforeFresh);
  const svg::RendererBitmap afterBitmap = RenderToBitmap(after);

  // Zero-diff identity expectation: both renders fill the same placed glyph
  // outlines, so any differing pixel is a real geometry/coordinate bug.
  // `CompareBitmapToBitmap` adds gtest failures and writes
  // actual_/expected_/diff_ PNGs to $TEST_UNDECLARED_OUTPUTS_DIR on mismatch.
  tests::CompareBitmapToBitmap(afterBitmap, beforeBitmap, "text_to_outlines_before_after",
                               tests::PixelmatchIdentityParams());
}

// Text authored by the editor's text tool: box text with per-line <tspan>
// children (x + dy line advance), the box-size data attributes, and bold
// styling. Conversion must produce path geometry that renders pixel-identical
// to the live text.
TEST(TextToOutlines, ConvertsTextToolBoxTextPixelIdentical) {
  constexpr std::string_view kToolAuthoredSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
<text x="10" y="52" font-family="sans-serif" font-size="32" fill="black" font-weight="bold" data-donner-text-box-width="180" data-donner-text-box-height="100"><tspan x="10">Hello</tspan><tspan x="10" dy="38.4">world</tspan></text>
</svg>)";
  svg::SVGDocument before = Parse(kToolAuthoredSvg);
  svg::SVGElement text = TextElement(before);

  const ConvertTextToOutlinesResult result = convertTextToOutlines(before, text);
  ASSERT_TRUE(result.ok) << result.error;

  svg::SVGDocument converted = Parse(result.mergedSource);
  EXPECT_FALSE(converted.querySelector("text").has_value())
      << "converted document must contain no live <text>";
  EXPECT_FALSE(converted.querySelector("tspan").has_value())
      << "converted document must contain no live <tspan>";

  svg::SVGDocument beforeFresh = Parse(kToolAuthoredSvg);
  svg::SVGDocument after = Parse(result.mergedSource);
  const svg::RendererBitmap beforeBitmap = RenderToBitmap(beforeFresh);
  const svg::RendererBitmap afterBitmap = RenderToBitmap(after);
  tests::CompareBitmapToBitmap(afterBitmap, beforeBitmap, "text_to_outlines_tool_box_text",
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

  ConvertTextToOutlinesResult result = convertTextToOutlines(document, text);
  ASSERT_TRUE(result.ok) << result.error;
  const std::string mergedSource = ApplyConversion(document, text, result);

  // Style is carried onto the `<g>` group.
  EXPECT_THAT(mergedSource, HasSubstr("fill=\"#0033aa\""));
  EXPECT_THAT(mergedSource, HasSubstr("fill-rule=\"evenodd\""));
  EXPECT_THAT(mergedSource, HasSubstr("opacity=\"0.5\""));
  EXPECT_THAT(mergedSource, HasSubstr("stroke=\"black\""));
  EXPECT_THAT(mergedSource, HasSubstr("stroke-width=\"2\""));
  EXPECT_THAT(mergedSource, HasSubstr("transform=\"translate(5,5)\""));

  // Paint order: the group sits between #under and #over, exactly where the
  // original `<text>` was.
  const size_t underPos = mergedSource.find("id=\"under\"");
  const size_t groupPos = mergedSource.find("id=\"t_outlines\"");
  const size_t overPos = mergedSource.find("id=\"over\"");
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

  AsyncSVGDocument async;
  async.setDocument(Parse(originalSource));
  ASSERT_TRUE(async.hasDocument());

  svg::SVGElement text = TextElement(async.document());
  ConvertTextToOutlinesResult result = convertTextToOutlines(async.document(), text);
  ASSERT_TRUE(result.ok) << result.error;

  // Apply the conversion the way the editor shell does: structural DOM edit
  // commands through the mutation queue (insert group + paths, delete text).
  std::optional<svg::SVGElement> parent = text.parentElement();
  ASSERT_TRUE(parent.has_value());
  async.queue().push(EditorCommand::InsertElementCommand(*parent, *result.outlineGroup, text));
  for (svg::SVGElement& path : result.outlinePaths) {
    async.queue().push(EditorCommand::InsertElementCommand(*result.outlineGroup, path));
  }
  async.queue().push(EditorCommand::DeleteElementCommand(text));
  ASSERT_TRUE(async.flushFrame());
  EXPECT_FALSE(async.document().querySelector("text").has_value());
  EXPECT_TRUE(async.document().querySelector("#logo_outlines").has_value());
  EXPECT_NE(std::string(async.document().source()).find("logo_outlines"), std::string::npos)
      << "the DOM edits must reflect into the source";

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
  EXPECT_FALSE(result.outlineGroup.has_value());
  EXPECT_TRUE(result.outlinePaths.empty());
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
  EXPECT_FALSE(result.outlineGroup.has_value());
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
