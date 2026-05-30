#include "donner/editor/ViewportSvgExport.h"

#include <gtest/gtest.h>

#include <string>

#include "donner/base/Box.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Vector2.h"
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

}  // namespace
}  // namespace donner::editor
