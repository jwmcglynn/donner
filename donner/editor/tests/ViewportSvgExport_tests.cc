#include "donner/editor/ViewportSvgExport.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "donner/base/Box.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {
namespace {

using svg::SVGDocument;
using svg::parser::SVGParser;

/// A small self-contained source document with no external references.
constexpr std::string_view kSelfContainedSvg =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<svg width=\"600\" height=\"600\" viewBox=\"0 0 600 600\" "
    "xmlns=\"http://www.w3.org/2000/svg\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n"
    "  <rect x=\"10\" y=\"20\" width=\"100\" height=\"50\" fill=\"red\"/>\n"
    "  <circle cx=\"300\" cy=\"300\" r=\"40\" fill=\"blue\"/>\n"
    "</svg>\n";

/// A document referencing an external resource over http:// — must be refused.
constexpr std::string_view kExternalResourceSvg =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<svg width=\"600\" height=\"600\" viewBox=\"0 0 600 600\" "
    "xmlns=\"http://www.w3.org/2000/svg\" "
    "xmlns:xlink=\"http://www.w3.org/1999/xlink\">\n"
    "  <image xlink:href=\"http://example.com/x.png\" x=\"0\" y=\"0\" "
    "width=\"100\" height=\"100\"/>\n"
    "</svg>\n";

SVGDocument ParseOrDie(std::string_view source) {
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> parseResult = SVGParser::ParseSVG(source, warningSink);
  EXPECT_FALSE(parseResult.hasError())
      << "Parse error: " << (parseResult.hasError() ? parseResult.error() : ParseDiagnostic{});
  return std::move(parseResult).result();
}

/// Build a viewport with a known screen↔document mapping for deterministic
/// `screenToDocument` results.
ViewportState MakeViewport(double zoom, const Vector2d& panDocPoint, const Vector2d& panScreenPoint,
                           const Vector2d& paneOrigin, const Vector2d& paneSize) {
  ViewportState viewport;
  viewport.zoom = zoom;
  viewport.panDocPoint = panDocPoint;
  viewport.panScreenPoint = panScreenPoint;
  viewport.paneOrigin = paneOrigin;
  viewport.paneSize = paneSize;
  return viewport;
}

bool Contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

TEST(ViewportSvgExportTest, ViewBoxMatchesScreenToDocumentOfRenderPaneRect) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);

  // zoom=2, panScreen=(0,0), panDoc=(10,20):
  //   screenToDocument(p) = (10,20) + p/2.
  const ViewportState viewport =
      MakeViewport(/*zoom=*/2.0, /*panDocPoint=*/Vector2d(10.0, 20.0),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(100.0, 100.0), /*paneSize=*/Vector2d(400.0, 300.0));
  const Recti renderPaneRect(Vector2i(100, 100), Vector2i(500, 400));

  // topLeft -> (10,20) + (100,100)/2 = (60,70)
  // bottomRight -> (10,20) + (500,400)/2 = (260,220)
  // viewBox = "60 70 200 150".
  const Box2d expectedDocBox =
      viewport.screenToDocument(Box2d(Vector2d(100.0, 100.0), Vector2d(500.0, 400.0)));
  EXPECT_EQ(expectedDocBox.topLeft, Vector2d(60.0, 70.0));
  EXPECT_EQ(expectedDocBox.bottomRight, Vector2d(260.0, 220.0));

  ViewportExportOptions options;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  EXPECT_TRUE(Contains(result.value, "viewBox=\"60 70 200 150\"")) << result.value;
  // Output dimensions are the render-pane size in CSS px.
  EXPECT_TRUE(Contains(result.value, "width=\"400\"")) << result.value;
  EXPECT_TRUE(Contains(result.value, "height=\"300\"")) << result.value;
}

TEST(ViewportSvgExportTest, ContentIsClippedToViewportRect) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport =
      MakeViewport(/*zoom=*/1.0, /*panDocPoint=*/Vector2d(0.0, 0.0),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(400.0, 300.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  // A clipPath with the document-space viewport rect must exist.
  EXPECT_TRUE(Contains(result.value, "<clipPath id=\"donner-viewport-clip\">")) << result.value;
  EXPECT_TRUE(
      Contains(result.value, "<rect x=\"0\" y=\"0\" width=\"400\" height=\"300\"/></clipPath>"))
      << result.value;
  // Content is wrapped in a group referencing that clip path.
  EXPECT_TRUE(Contains(result.value, "<g clip-path=\"url(#donner-viewport-clip)\">"))
      << result.value;
  // Source children are present (verbatim, vector-first — no <image> snapshot).
  EXPECT_TRUE(Contains(result.value, "<rect x=\"10\" y=\"20\" width=\"100\" height=\"50\""))
      << result.value;
  EXPECT_TRUE(Contains(result.value, "<circle cx=\"300\" cy=\"300\" r=\"40\"")) << result.value;
}

TEST(ViewportSvgExportTest, OverlayGroupAbsentByDefault) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = MakeViewport(1.0, Vector2d(0.0, 0.0), Vector2d(0.0, 0.0),
                                              Vector2d(0.0, 0.0), Vector2d(400.0, 300.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;  // includeSelectionOverlay defaults to false.
  ASSERT_FALSE(options.includeSelectionOverlay);
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  EXPECT_FALSE(Contains(result.value, "donner-editor-overlay")) << result.value;
}

TEST(ViewportSvgExportTest, OverlayGroupPlaceholderEmittedWhenRequested) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = MakeViewport(1.0, Vector2d(0.0, 0.0), Vector2d(0.0, 0.0),
                                              Vector2d(0.0, 0.0), Vector2d(400.0, 300.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  // The overlay group exists with the documented id but is EMPTY in M6.
  EXPECT_TRUE(Contains(result.value, "<g id=\"donner-editor-overlay\"")) << result.value;
  EXPECT_TRUE(Contains(result.value, "data-donner-export-role=\"editor-overlay\"")) << result.value;
  // No overlay primitives yet; the group must close immediately.
  const std::size_t overlayPos = result.value.find("<g id=\"donner-editor-overlay\"");
  ASSERT_NE(overlayPos, std::string::npos);
  const std::size_t closePos = result.value.find("</g>", overlayPos);
  ASSERT_NE(closePos, std::string::npos);
  const std::size_t openTagEnd = result.value.find('>', overlayPos);
  ASSERT_NE(openTagEnd, std::string::npos);
  // Between the overlay group's open tag and its close tag there is no nested
  // element — only whitespace/comment placeholder.
  const std::string between = result.value.substr(openTagEnd + 1, closePos - (openTagEnd + 1));
  EXPECT_EQ(between.find('<'), std::string::npos)
      << "overlay group must be empty in M6: " << between;
}

TEST(ViewportSvgExportTest, ExportDoesNotMutateSourceDocument) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const std::string sourceBefore(doc.source());

  const ViewportState viewport = MakeViewport(1.5, Vector2d(5.0, 5.0), Vector2d(0.0, 0.0),
                                              Vector2d(0.0, 0.0), Vector2d(300.0, 300.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(300, 300));

  ViewportExportOptions options;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  const std::string sourceAfter(doc.source());
  EXPECT_EQ(sourceBefore, sourceAfter);
}

TEST(ViewportSvgExportTest, SelfContainedExportRoundTripsWithMatchingViewBox) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport =
      MakeViewport(/*zoom=*/1.0, /*panDocPoint=*/Vector2d(0.0, 0.0),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(250.0, 200.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(250, 200));

  ViewportExportOptions options;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  // The exported SVG must re-parse cleanly.
  ParseWarningSink reparseWarningSink = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> reparsed = SVGParser::ParseSVG(result.value, reparseWarningSink);
  ASSERT_FALSE(reparsed.hasError()) << "Re-parse error: " << reparsed.error();
  SVGDocument exportedDoc = std::move(reparsed).result();

  const std::optional<Box2d> viewBox = exportedDoc.svgElement().viewBox();
  ASSERT_TRUE(viewBox.has_value());
  EXPECT_EQ(viewBox->topLeft, Vector2d(0.0, 0.0));
  EXPECT_EQ(viewBox->width(), 250.0);
  EXPECT_EQ(viewBox->height(), 200.0);
}

TEST(ViewportSvgExportTest, ExternalResourceReferenceIsRefused) {
  const SVGDocument doc = ParseOrDie(kExternalResourceSvg);
  const ViewportState viewport = MakeViewport(1.0, Vector2d(0.0, 0.0), Vector2d(0.0, 0.0),
                                              Vector2d(0.0, 0.0), Vector2d(400.0, 300.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  EXPECT_FALSE(result.ok());
  // The error must be useful: name the offending external reference.
  EXPECT_TRUE(Contains(result.error, "http://example.com/x.png")) << result.error;
  EXPECT_TRUE(Contains(result.error, "external")) << result.error;
}

TEST(ViewportSvgExportTest, NonTransparentBackgroundPrependsCoveringRect) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = MakeViewport(1.0, Vector2d(0.0, 0.0), Vector2d(0.0, 0.0),
                                              Vector2d(0.0, 0.0), Vector2d(400.0, 300.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.transparentBackground = false;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  EXPECT_TRUE(Contains(result.value,
                       "<rect x=\"0\" y=\"0\" width=\"400\" height=\"300\" fill=\"#ffffff\"/>"))
      << result.value;
}

// --- Milestone 7: overlay serialization ---------------------------------

/// Return the substring of @p haystack spanning the `<g id="donner-editor-
/// overlay" ...>` open tag through its matching `</g>`. Empty if not found.
std::string OverlayGroupSpan(const std::string& haystack) {
  const std::size_t open = haystack.find("<g id=\"donner-editor-overlay\"");
  if (open == std::string::npos) {
    return std::string();
  }
  const std::size_t close = haystack.find("</g>", open);
  if (close == std::string::npos) {
    return std::string();
  }
  return haystack.substr(open, (close + 4) - open);
}

/// Parse the double value of attribute @p attr (e.g. `x`) from the first
/// occurrence of `attr="<number>"` at or after @p from in @p text.
double ParseAttr(const std::string& text, std::string_view attr, std::size_t from) {
  const std::string needle = std::string(attr) + "=\"";
  const std::size_t at = text.find(needle, from);
  EXPECT_NE(at, std::string::npos) << "attribute " << attr << " not found";
  const std::size_t valueStart = at + needle.size();
  const std::size_t valueEnd = text.find('"', valueStart);
  EXPECT_NE(valueEnd, std::string::npos);
  return std::stod(text.substr(valueStart, valueEnd - valueStart));
}

/// A viewport whose `screenToDocument` is the identity over the render pane, so
/// the exported viewBox is "0 0 400 300" and document coords map 1:1.
ViewportState IdentityViewport() {
  return MakeViewport(/*zoom=*/1.0, /*panDocPoint=*/Vector2d(0.0, 0.0),
                      /*panScreenPoint=*/Vector2d(0.0, 0.0),
                      /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(400.0, 300.0));
}

/// A snapshot with a single AABB at (50,60)-(150,160) and one resize handle.
SelectionChromeSnapshot MakeAabbSnapshot() {
  SelectionChromeSnapshot snapshot;
  snapshot.aabbsDoc.push_back(Box2d(Vector2d(50.0, 60.0), Vector2d(150.0, 160.0)));
  snapshot.handleBoxesDoc.push_back(Box2d(Vector2d(48.0, 58.0), Vector2d(52.0, 62.0)));
  return snapshot;
}

TEST(ViewportSvgExportTest, OverlayGroupPopulatedFromSnapshot) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  const std::string overlay = OverlayGroupSpan(result.value);
  ASSERT_FALSE(overlay.empty()) << result.value;
  // The overlay group must now be NON-empty: it contains primitives.
  EXPECT_NE(overlay.find("<rect"), std::string::npos) << overlay;

  // The AABB rect is emitted first (paths -> aabbs -> handles), so the first
  // `<rect` is the AABB. Its coordinates must match the snapshot AABB.
  const std::size_t aabbRect = overlay.find("<rect");
  ASSERT_NE(aabbRect, std::string::npos);
  EXPECT_NEAR(ParseAttr(overlay, "x", aabbRect), 50.0, 1e-3) << overlay;
  EXPECT_NEAR(ParseAttr(overlay, "y", aabbRect), 60.0, 1e-3) << overlay;
  EXPECT_NEAR(ParseAttr(overlay, "width", aabbRect), 100.0, 1e-3) << overlay;
  EXPECT_NEAR(ParseAttr(overlay, "height", aabbRect), 100.0, 1e-3) << overlay;
}

TEST(ViewportSvgExportTest, OverlayHandlesRendered) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  const std::string overlay = OverlayGroupSpan(result.value);
  ASSERT_FALSE(overlay.empty()) << result.value;
  // The resize handle is a white-filled rect.
  EXPECT_NE(overlay.find("fill=\"#ffffff\""), std::string::npos) << overlay;
}

TEST(ViewportSvgExportTest, OverlayGroupCarriesClipPath) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  // The overlay group reuses the M6 content clipPath; it is not a new one.
  const std::size_t open = result.value.find("<g id=\"donner-editor-overlay\"");
  ASSERT_NE(open, std::string::npos) << result.value;
  const std::size_t openTagEnd = result.value.find('>', open);
  ASSERT_NE(openTagEnd, std::string::npos);
  const std::string openTag = result.value.substr(open, openTagEnd - open);
  EXPECT_NE(openTag.find("clip-path=\"url(#donner-viewport-clip)\""), std::string::npos) << openTag;
}

TEST(ViewportSvgExportTest, OverlayStyleIsDeterministic) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  const std::string overlay = OverlayGroupSpan(result.value);
  ASSERT_FALSE(overlay.empty()) << result.value;
  // Theme-independent stroke color.
  EXPECT_NE(overlay.find("stroke=\"#1ea7fd\""), std::string::npos) << overlay;
}

TEST(ViewportSvgExportTest, OverlayExportDoesNotMutateSourceDocument) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const std::string sourceBefore(doc.source());

  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  const std::string sourceAfter(doc.source());
  EXPECT_EQ(sourceBefore, sourceAfter);
}

}  // namespace
}  // namespace donner::editor
