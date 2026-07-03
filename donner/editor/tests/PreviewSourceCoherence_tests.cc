/// @file
/// Preview-vs-source save/reload coherence tests.
///
/// Invariant under test: after any *committed* editor operation, the editor's
/// live preview render (rendering `AsyncSVGDocument::document()` — the same DOM
/// the on-screen canvas composites) is pixel-identical to rendering the
/// document's saved *source* after a save -> reload (reparse) roundtrip
/// (`SVGParser::ParseSVG(document().source())`). In plain terms: "what you see
/// in the editor" must equal "what you get when you save the SVG and reopen it",
/// and this must hold after EACH kind of edit, not only at initial load.
///
/// This is the COMMITTED-state invariant. It deliberately does NOT exercise
/// mid-drag transient preview (the drag fast-path uses a transient compose
/// offset before the source is updated — a separate invariant fixed on another
/// branch). Every operation here is pushed onto the `CommandQueue` and committed
/// via `AsyncSVGDocument::flushFrame()`, exactly as the editor's per-frame main
/// loop does.
///
/// Each committed operation gets its own a/b/c coherence check:
///   a. Render the live document state (the preview) to an RGBA bitmap.
///   b. Take `document().source()` bytes, reparse into a fresh `SVGDocument`
///      (the "reload"), and render THAT at the same canvas size.
///   c. Assert the two bitmaps are pixel-identical via `CompareBitmapToBitmap`
///      with `PixelmatchIdentityParams()` (zero diff, no percentage threshold).
///      On mismatch the helper dumps actual_/expected_/diff_ PNGs to
///      `$TEST_UNDECLARED_OUTPUTS_DIR`.
///
/// Runs in the DEFAULT basic-text (stb_truetype) configuration: the fixture
/// `<text>` uses the engine's embedded fallback font, so the text/outline paths
/// are exercised hermetically with no checked-in font.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/AsyncSVGDocument.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/TextToOutlines.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

namespace {

constexpr int kCanvas = 240;

/// Parse \p svg into a source-backed SVGDocument, asserting success. Source
/// backing is required so the committed ops write back into the XML source store
/// and `document().source()` reflects every mutation.
svg::SVGDocument Parse(std::string_view svg) {
  ParseWarningSink sink;
  auto result = svg::parser::SVGParser::ParseSVG(svg, sink);
  EXPECT_FALSE(result.hasError()) << "parse failed for: " << svg;
  return result.result();
}

/// Render \p document to a `RendererBitmap` at the fixed canvas size, via the
/// same `svg::Renderer::draw` + `takeSnapshot` path the other editor pixel-diff
/// suites use (`TextToOutlines_tests`, `DocumentSave_tests`).
svg::RendererBitmap Render(svg::SVGDocument& document) {
  document.setCanvasSize(kCanvas, kCanvas);
  svg::Renderer renderer;
  renderer.draw(document);
  return renderer.takeSnapshot();
}

/// Render the editor's LIVE preview: the document the on-screen canvas
/// composites is `AsyncSVGDocument::document()`. This is the "what you see"
/// side of the invariant.
svg::RendererBitmap RenderLivePreview(AsyncSVGDocument& async) {
  return Render(async.document());
}

/// Render the SAVE -> RELOAD side: take the current document source bytes (as if
/// "Save" were pressed right now), reparse them into a fresh document (the
/// "reopen"), and render that. This is the "what you get when you save and
/// reopen" side of the invariant.
svg::RendererBitmap RenderReloadedFromSource(AsyncSVGDocument& async) {
  const std::string savedSource(async.document().source());
  svg::SVGDocument reloaded = Parse(savedSource);
  return Render(reloaded);
}

/// The full a/b/c coherence assertion for one committed editor state.
/// `op` names the operation so a failure localizes the offending edit without a
/// rerun (ToTT). On mismatch `CompareBitmapToBitmap` writes
/// actual_<op>/expected_<op>/diff_<op> PNGs for inspection.
void ExpectPreviewMatchesReloadedSource(AsyncSVGDocument& async, std::string_view op) {
  SCOPED_TRACE(testing::Message() << "preview != reloaded-source after committed op: " << op);
  const svg::RendererBitmap preview = RenderLivePreview(async);
  const svg::RendererBitmap reloaded = RenderReloadedFromSource(async);
  const std::string label = std::string("preview_source_coherence_") + std::string(op);
  // Zero-diff identity expectation: the live preview and a save->reload of the
  // current source render the SAME committed document, so any differing pixel is
  // a real coherence bug (wrong attribute writeback, stale source, transform not
  // serialized, etc.) — never anti-aliasing (pixelmatch already excludes AA).
  tests::CompareBitmapToBitmap(preview, reloaded, label, tests::PixelmatchIdentityParams());
}

/// Resolve a required element by CSS selector, asserting it exists.
svg::SVGElement Require(AsyncSVGDocument& async, std::string_view selector) {
  auto element = async.document().querySelector(selector);
  EXPECT_TRUE(element.has_value()) << "missing element for selector: " << selector;
  return *element;
}

/// Small but non-trivial fixture: two top-level shapes (rect, circle), a group
/// `<g>` wrapping a third shape, and a `<text>` so the text/outline path is
/// exercised. Custom raw-string delimiter `SVG(...)SVG` so attribute values
/// containing `)"` (none here, but kept for safety/consistency) never close the
/// literal early.
constexpr std::string_view kFixture =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 240">
  <rect id="bg" x="0" y="0" width="240" height="240" fill="#ffffff"/>
  <rect id="box" x="20" y="20" width="80" height="60" fill="#3366cc"/>
  <circle id="dot" cx="170" cy="60" r="35" fill="#cc3366"/>
  <g id="grp" transform="translate(10,120)">
    <rect id="inner" x="0" y="0" width="70" height="50" fill="#33aa66"/>
  </g>
  <text id="label" x="20" y="220" font-size="40" fill="#222288">SVG</text>
</svg>)SVG";

}  // namespace

// Sanity: at initial load (before any edit), the live preview equals a
// save->reload of the source. If this fails, the harness itself (viewport/size
// mismatch between the two render paths) is wrong, not the product.
// Regression: text inserted programmatically (SVGTextElement::Create +
// insertElement + setAttribute positioning) must render in the LIVE document,
// not just after a save->reload. `setAttribute("x"/"y")` on text elements
// previously stored the raw attribute without updating
// `TextPositioningComponent` (the XML-parse path was the only writer), so the
// live layout placed glyphs at the origin while a reload rendered them at the
// authored position.
TEST(PreviewSourceCoherence, InsertedTextRendersLiveAtAuthoredPosition) {
  constexpr std::string_view kBlank =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 240">
  <rect id="bg" x="0" y="0" width="240" height="240" fill="#ffffff"/>
</svg>)";

  const auto countOrange = [](const svg::RendererBitmap& bitmap) {
    int orange = 0;
    for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4) {
      if (bitmap.pixels[i] > 120 && bitmap.pixels[i + 1] > 50 && bitmap.pixels[i + 1] < 140 &&
          bitmap.pixels[i + 2] < 60) {
        ++orange;
      }
    }
    return orange;
  };

  svg::SVGDocument document = Parse(kBlank);
  // Prime the render tree, mirroring an editor that has already presented.
  (void)Render(document);

  svg::SVGTextElement text = svg::SVGTextElement::Create(document);
  text.setAttribute("x", "120");
  text.setAttribute("y", "200");
  text.setAttribute("font-size", "36");
  text.setAttribute("fill", "#aa5500");
  (void)document.insertElement(document.svgElement(), text);
  text.setTextContent("Hi");

  const svg::RendererBitmap bitmap = Render(document);
  EXPECT_GT(countOrange(bitmap), 50) << "programmatically inserted text must render live";
}

TEST(PreviewSourceCoherence, InitialLoadMatches) {
  AsyncSVGDocument async;
  async.setDocument(Parse(kFixture));
  ASSERT_TRUE(async.hasDocument());

  ExpectPreviewMatchesReloadedSource(async, "initial_load");
}

// A full representative sequence of committed editor operations. After EACH
// committed op the preview-vs-reloaded-source invariant is re-checked, so a
// regression names the exact edit that broke coherence.
TEST(PreviewSourceCoherence, MatchesAfterEachCommittedOperation) {
  AsyncSVGDocument async;
  async.setDocument(Parse(kFixture));
  ASSERT_TRUE(async.hasDocument());

  // (a) Initial load.
  ExpectPreviewMatchesReloadedSource(async, "initial_load");

  // (b) Move a shape. The editor commits a move as an attribute writeback into
  // the XML source store (the live drag fast-path's transform is folded back to
  // the `transform` attribute at commit time). We drive that committed form
  // directly via SetAttribute, which `AsyncSVGDocument::applyOne` routes through
  // `document_->setElementAttribute(...)` — mutating BOTH the live DOM and the
  // source. (Pure `SetTransformCommand` is the *mid-drag* form and is covered by
  // the drag-coherence suite on another branch; here we test committed state.)
  {
    const svg::SVGElement box = Require(async, "#box");
    async.queue().push(EditorCommand::SetAttributeCommand(box, "transform", "translate(30,18)"));
    ASSERT_TRUE(async.flushFrame());
    ExpectPreviewMatchesReloadedSource(async, "move_box");
  }

  // (c) Recolor a shape (committed attribute edit).
  {
    const svg::SVGElement dot = Require(async, "#dot");
    async.queue().push(EditorCommand::SetAttributeCommand(dot, "fill", "#22aa44"));
    ASSERT_TRUE(async.flushFrame());
    ExpectPreviewMatchesReloadedSource(async, "recolor_dot");
  }

  // (d1) Insert text via the Text-tool command path. `InsertTextCommand` creates
  // the live `<text>` node and writes it into the source store, exactly as the
  // TextTool authoring flow does at commit. We give it position/size/fill via
  // committed SetAttribute edits so it covers real pixels.
  {
    svg::SVGElement parent = async.document().svgElement();
    svg::SVGTextElement created = svg::SVGTextElement::Create(async.document());
    svg::SVGElement inserted = created;
    async.queue().push(
        EditorCommand::InsertTextCommand(parent, inserted, "Hi", /*reference=*/std::nullopt));
    ASSERT_TRUE(async.flushFrame());

    // The inserted handle stays valid after the structural insert (it is the
    // same element, not a fresh query). Newly-created text has no geometry
    // attributes yet; set them as committed attribute edits so it renders and
    // the coherence check covers real pixels.
    async.queue().push(EditorCommand::SetAttributeCommand(inserted, "x", "120"));
    async.queue().push(EditorCommand::SetAttributeCommand(inserted, "y", "200"));
    async.queue().push(EditorCommand::SetAttributeCommand(inserted, "font-size", "36"));
    async.queue().push(EditorCommand::SetAttributeCommand(inserted, "fill", "#aa5500"));
    ASSERT_TRUE(async.flushFrame());
    ExpectPreviewMatchesReloadedSource(async, "insert_text");
  }

  // (d2) Convert Text to Outlines on the original fixture `<text>`. This is a
  // set of structural DOM edits (insert group + paths before the <text>, then
  // delete it) whose source deltas mirror into the source text. Verifies the
  // preview still equals a save->reload after a structural edit burst.
  {
    const svg::SVGElement label = Require(async, "#label");
    ConvertTextToOutlinesResult result = convertTextToOutlines(async.document(), label);
    ASSERT_TRUE(result.ok) << result.error;
    std::optional<svg::SVGElement> labelParent = label.parentElement();
    ASSERT_TRUE(labelParent.has_value());
    async.queue().push(EditorCommand::InsertElementCommand(*labelParent, *result.outlineGroup, label));
    for (svg::SVGElement& path : result.outlinePaths) {
      async.queue().push(EditorCommand::InsertElementCommand(*result.outlineGroup, path));
    }
    async.queue().push(EditorCommand::DeleteElementCommand(label));
    ASSERT_TRUE(async.flushFrame());
    EXPECT_FALSE(async.document().querySelector("#label").has_value())
        << "converted <text> should no longer be live";
    ExpectPreviewMatchesReloadedSource(async, "convert_text_to_outlines");
  }

  // (e) Delete a shape (committed structural edit). `applyOne` routes this
  // through `document_->removeElement(...)`, mutating both the live DOM and the
  // source.
  {
    const svg::SVGElement dot = Require(async, "#dot");
    async.queue().push(EditorCommand::DeleteElementCommand(dot));
    ASSERT_TRUE(async.flushFrame());
    EXPECT_FALSE(async.document().querySelector("#dot").has_value())
        << "deleted #dot should no longer be live";
    ExpectPreviewMatchesReloadedSource(async, "delete_dot");
  }

  // NOTE ON COVERAGE: The shape-clipboard Paste/Cut path
  // (`donner/editor/ShapeClipboardCommands.h` -> `PasteShapesCommand`) is NOT
  // driven here. Its commit mechanism is the structural-reparse path
  // (`AsyncSVGDocument::applyOne` handles CutShapes/PasteShapes by reparsing
  // `bytes` via `setDocumentMaybeStructural`). Driving the clipboard helper
  // itself requires selection-id plumbing that belongs to
  // `shape_clipboard_tests`.
}

}  // namespace donner::editor
