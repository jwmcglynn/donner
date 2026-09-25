#include "donner/editor/ViewportSvgExport.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Path.h"
#include "donner/base/Vector2.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/ViewportState.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

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

/// A document referencing an external resource over http:// - must be refused.
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

/// A viewport whose `screenToDocument` is the identity over the render pane, so
/// the exported viewBox is "0 0 400 300" and document coords map 1:1.
ViewportState IdentityViewport() {
  return MakeViewport(/*zoom=*/1.0, /*panDocPoint=*/Vector2d(0.0, 0.0),
                      /*panScreenPoint=*/Vector2d(0.0, 0.0),
                      /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(400.0, 300.0));
}

/// Whether exported SVG text re-parses cleanly: the deterministic form of the round-trip
/// fuzzer oracle. Returns an assertion result so call sites decide between EXPECT and ASSERT;
/// a fatal assertion inside a void helper would not stop the calling test.
testing::AssertionResult ReparsesCleanly(std::string_view exported) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  ParseResult<SVGDocument> reparsed = SVGParser::ParseSVG(exported, sink);
  if (reparsed.hasError()) {
    return testing::AssertionFailure() << "Re-parse error: " << reparsed.error();
  }
  return testing::AssertionSuccess();
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

  EXPECT_THAT(result.value, HasSubstr("viewBox=\"60 70 200 150\""));
  // Output dimensions follow the cropped portion of the original document.
  EXPECT_THAT(result.value, HasSubstr("<svg width=\"200\" height=\"150\""));
}

TEST(ViewportSvgExportTest, FullyVisibleDocumentKeepsOriginalBoundsAndDimensions) {
  const SVGDocument doc = ParseOrDie(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"800\" height=\"400\" "
      "viewBox=\"10 20 400 200\"><rect x=\"10\" y=\"20\" width=\"400\" height=\"200\"/>"
      "</svg>");
  const ViewportState viewport =
      MakeViewport(/*zoom=*/0.5, /*panDocPoint=*/Vector2d(0.0, 0.0),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(600.0, 400.0));

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(600, 400)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("width=\"800\" height=\"400\" viewBox=\"10 20 400 200\""));
  EXPECT_THAT(result.value,
              HasSubstr("<rect x=\"10\" y=\"20\" width=\"400\" height=\"200\"/></clipPath>"));
}

TEST(ViewportSvgExportTest, PartialViewportOnlyExportsOriginalDocumentIntersection) {
  const SVGDocument doc = ParseOrDie(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"800\" height=\"400\" "
      "viewBox=\"10 20 400 200\"><rect x=\"10\" y=\"20\" width=\"400\" height=\"200\"/>"
      "</svg>");
  const ViewportState viewport =
      MakeViewport(/*zoom=*/1.0, /*panDocPoint=*/Vector2d(-50.0, -30.0),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(200.0, 150.0));

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(200, 150)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("width=\"280\" height=\"200\" viewBox=\"10 20 140 100\""));
  EXPECT_THAT(result.value,
              HasSubstr("<rect x=\"10\" y=\"20\" width=\"140\" height=\"100\"/></clipPath>"));
}

TEST(ViewportSvgExportTest, ViewBoxlessRootUsesCurrentIntrinsicBoundsOverStaleViewportCache) {
  const SVGDocument doc = ParseOrDie(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
      "<rect width=\"200\" height=\"100\" fill=\"red\"/></svg>");
  ViewportState viewport = IdentityViewport();
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 100.0, 100.0);

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(200, 100)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" "
                                      "height=\"100\" viewBox=\"0 0 200 100\""));
}

TEST(ViewportSvgExportTest, TinyPositiveCropRetainsNonzeroDimensions) {
  const SVGDocument doc = ParseOrDie(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\" "
      "viewBox=\"0 0 1000000000 1000000000\"/>");
  const ViewportState viewport =
      MakeViewport(/*zoom=*/32.0, /*panDocPoint=*/Vector2d(0.0, 0.0),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(1.0, 1.0));

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(1, 1)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, Not(HasSubstr("width=\"0\"")));
  EXPECT_THAT(result.value, Not(HasSubstr("height=\"0\"")));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

TEST(ViewportSvgExportTest, ViewportOutsideDocumentHasNoExportableRegion) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport =
      MakeViewport(/*zoom=*/1.0, /*panDocPoint=*/Vector2d(700.0, 700.0),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(100.0, 100.0));

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("does not overlap"));
}

TEST(ViewportSvgExportTest, RootIdWithDoubleHyphenSanitizedInComment) {
  // Regression: an untrusted root id containing "--" must not appear verbatim in
  // the provenance XML comment body, which XML forbids and which makes the
  // exported document invalid for conformant consumers.
  constexpr std::string_view kDoubleHyphenIdSvg =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<svg id=\"a--b\" width=\"600\" height=\"600\" viewBox=\"0 0 600 600\" "
      "xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <rect x=\"10\" y=\"20\" width=\"100\" height=\"50\" fill=\"red\"/>\n"
      "</svg>\n";
  const SVGDocument doc = ParseOrDie(kDoubleHyphenIdSvg);

  ViewportExportOptions options;
  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(400, 300)), options);
  ASSERT_TRUE(result.ok()) << result.error;

  // The comment body (between "<!--" and "-->") must not contain "--".
  const std::string& out = result.value;
  const size_t commentStart = out.find("<!--");
  const size_t commentEnd = out.find("-->", commentStart);
  ASSERT_NE(commentStart, std::string::npos);
  ASSERT_NE(commentEnd, std::string::npos);
  const std::string commentBody = out.substr(commentStart + 4, commentEnd - (commentStart + 4));
  EXPECT_THAT(commentBody, Not(HasSubstr("--")));
  // The id is preserved in a sanitized, readable form.
  EXPECT_THAT(commentBody, HasSubstr("source: a- -b;"));
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
  EXPECT_THAT(result.value, HasSubstr("<clipPath id=\"donner-viewport-clip\">"));
  EXPECT_THAT(result.value,
              HasSubstr("<rect x=\"0\" y=\"0\" width=\"400\" height=\"300\"/></clipPath>"));
  // Content is wrapped in a group referencing that clip path.
  EXPECT_THAT(result.value, HasSubstr("<g clip-path=\"url(#donner-viewport-clip)\">"));
  // Source children are present (verbatim, vector-first - no <image> snapshot).
  EXPECT_THAT(result.value, HasSubstr("<rect x=\"10\" y=\"20\" width=\"100\" height=\"50\""));
  EXPECT_THAT(result.value, HasSubstr("<circle cx=\"300\" cy=\"300\" r=\"40\""));
}

TEST(ViewportSvgExportTest, InjectedClipPathIdIsUniquifiedAgainstSourceIds) {
  // Source document that already declares the preferred injected clip id.
  constexpr std::string_view kClipIdCollisionSvg =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<svg width=\"600\" height=\"600\" viewBox=\"0 0 600 600\" "
      "xmlns=\"http://www.w3.org/2000/svg\">\n"
      "  <clipPath id=\"donner-viewport-clip\">"
      "<rect x=\"0\" y=\"0\" width=\"50\" height=\"50\"/></clipPath>\n"
      "  <rect x=\"10\" y=\"20\" width=\"100\" height=\"50\" fill=\"red\" "
      "clip-path=\"url(#donner-viewport-clip)\"/>\n"
      "</svg>\n";

  const SVGDocument doc = ParseOrDie(kClipIdCollisionSvg);
  const ViewportState viewport = MakeViewport(1.0, Vector2d(0.0, 0.0), Vector2d(0.0, 0.0),
                                              Vector2d(0.0, 0.0), Vector2d(400.0, 300.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  // The injected clip path picks a suffixed id instead of colliding with the
  // source-declared one, and every injected reference uses the suffixed id.
  EXPECT_THAT(result.value, HasSubstr("<defs><clipPath id=\"donner-viewport-clip-2\">"));
  EXPECT_THAT(result.value, HasSubstr("<g clip-path=\"url(#donner-viewport-clip-2)\">"));
  EXPECT_THAT(result.value,
              HasSubstr("pointer-events=\"none\" clip-path=\"url(#donner-viewport-clip-2)\">"));

  // The source-declared clip path and its reference are preserved verbatim.
  EXPECT_THAT(result.value, HasSubstr("<clipPath id=\"donner-viewport-clip\">"
                                      "<rect x=\"0\" y=\"0\" width=\"50\" height=\"50\"/>"));
  EXPECT_THAT(result.value, HasSubstr("clip-path=\"url(#donner-viewport-clip)\"/>"));
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

  EXPECT_THAT(result.value, Not(HasSubstr("donner-editor-overlay")));
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

  // The overlay group exists with the documented id but is EMPTY without a snapshot.
  EXPECT_THAT(result.value, HasSubstr("<g id=\"donner-editor-overlay\""));
  EXPECT_THAT(result.value, HasSubstr("data-donner-export-role=\"editor-overlay\""));
  // No overlay primitives yet; the group must close immediately.
  const std::size_t overlayPos = result.value.find("<g id=\"donner-editor-overlay\"");
  ASSERT_NE(overlayPos, std::string::npos);
  const std::size_t closePos = result.value.find("</g>", overlayPos);
  ASSERT_NE(closePos, std::string::npos);
  const std::size_t openTagEnd = result.value.find('>', overlayPos);
  ASSERT_NE(openTagEnd, std::string::npos);
  // Between the overlay group's open tag and its close tag there is no nested
  // element - only whitespace/comment placeholder.
  const std::string between = result.value.substr(openTagEnd + 1, closePos - (openTagEnd + 1));
  EXPECT_THAT(between, Not(HasSubstr("<"))) << "overlay group must be empty in M6";
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
  EXPECT_THAT(result.error, HasSubstr("http://example.com/x.png"));
  EXPECT_THAT(result.error, HasSubstr("external"));
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

  EXPECT_THAT(result.value,
              HasSubstr("<rect x=\"0\" y=\"0\" width=\"400\" height=\"300\" fill=\"#ffffff\"/>"));
}

TEST(ViewportSvgExportTest, ProgrammaticDocumentWithoutSourceStoreIsRefused) {
  const SVGDocument doc;
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("source store"));
}

TEST(ViewportSvgExportTest, ExternalReferenceScannerSkipsHrefLikeAttributes) {
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\" "
      "xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
      "<defs><rect id=\"shape\" hrefish=\"https://not-a-reference.example/image.png\" "
      "width=\"10\" height=\"10\"/></defs>"
      "<use xlink:href=\"#shape\"/>"
      "<image href = ' \tFILE://tmp/image.png' width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("FILE://tmp/image.png"));
  EXPECT_THAT(result.error, Not(HasSubstr("not-a-reference.example")));
}

TEST(ViewportSvgExportTest, InternalFragmentReferencesAreAllowed) {
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\" "
      "xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
      "<defs><rect id=\"shape\" width=\"10\" height=\"10\"/></defs>"
      "<use href=\"#shape\" x=\"5\"/>"
      "<use xlink:href=\"#shape\" x=\"20\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("<use href=\"#shape\" x=\"5\"/>"));
  EXPECT_THAT(result.value, HasSubstr("<use xlink:href=\"#shape\" x=\"20\"/>"));
}

TEST(ViewportSvgExportTest, RootAttributeEscapingIsDeterministic) {
  const SVGDocument doc = ParseOrDie(
      "<svg id=\"root&amp;source\" data-title='\"quoted\"' data-owner=\"Bob's\" "
      "data-lines=\"first&#10;second\" "
      "width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  // Root attribute values are carried as decoded values and escaped exactly once, so the
  // source spelling round-trips rather than gaining an escaping level per export.
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("source: root&amp;source"));
  EXPECT_THAT(result.value, HasSubstr("id=\"root&amp;source\""));
  EXPECT_THAT(result.value, HasSubstr("data-title=\"&quot;quoted&quot;\""));
  EXPECT_THAT(result.value, HasSubstr("data-owner=\"Bob&apos;s\""));
  // A literal newline in an attribute value is normalized to a space when the document is
  // re-parsed, so it has to be written as a numeric reference for the value to survive.
  EXPECT_THAT(result.value, HasSubstr("data-lines=\"first&#10;second\""));
  EXPECT_TRUE(ReparsesCleanly(result.value));

  // Re-exporting the export is a fixed point: the id does not grow another `amp;`, and the
  // newline is still a newline rather than a space.
  const SVGDocument reexportedDoc = ParseOrDie(result.value);
  const Result<std::string, std::string> reexported =
      ExportViewportAsSvg(reexportedDoc, viewport, renderPaneRect, ViewportExportOptions{});
  ASSERT_TRUE(reexported.ok()) << reexported.error;
  EXPECT_THAT(reexported.value, HasSubstr("id=\"root&amp;source\""));
  EXPECT_THAT(reexported.value, HasSubstr("data-lines=\"first&#10;second\""));
}

TEST(ViewportSvgExportTest, RawLessThanInRootAttributeIsEscapedAfterSourceEdit) {
  SVGDocument doc = ParseOrDie(
      "<svg id=\"root\" width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect width=\"10\" height=\"10\"/>"
      "</svg>");
  const std::size_t rootOffset = doc.source().find("root");
  ASSERT_NE(rootOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(rootOffset), FileOffset::Offset(rootOffset + 4)},
      .replacement = "a<b",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("source: a&lt;b"));
  EXPECT_THAT(result.value, HasSubstr("id=\"a&lt;b\""));
}

TEST(ViewportSvgExportTest, RootAttributeGreaterThanAndMissingDimensionsAreNormalized) {
  const SVGDocument doc = ParseOrDie(
      "<svg id=\"root\" data-arrow=\"a>b\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("data-arrow=\"a&gt;b\""));
  EXPECT_THAT(result.value, HasSubstr("width=\"100\""));
  EXPECT_THAT(result.value, HasSubstr("height=\"100\""));
  EXPECT_THAT(result.value, HasSubstr("viewBox=\"0 0 100 100\""));
}

TEST(ViewportSvgExportTest, RootScannerSkipsPrologCommentsDoctypeAndProcessingInstructions) {
  const SVGDocument doc = ParseOrDie(
      "<?xml version=\"1.0\"?>"
      "<!-- <svg-not-root/> -->"
      "<?editor instruction?>"
      "<!DOCTYPE svg>"
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("real-root-child"));
  EXPECT_THAT(result.value, Not(HasSubstr("svg-not-root")));
}

// Each shape below once desynchronized a raw-source scan from the tokenizer, letting the
// exporter pick the wrong root, body start, or body end. Markup boundaries now come from
// parsed-tree source locations, so these documents are only interesting as regressions.

TEST(ViewportSvgExportTest, RootScannerSkipsProcessingInstructionContainingMarkup) {
  // A processing instruction whose content contains `>` followed by an `<svg>`
  // element is not an element node, so the PI-embedded element is never
  // mistaken for the document root.
  const SVGDocument doc = ParseOrDie(
      "<?editor data=\"a>b\"><svg id=\"not-the-root\" width=\"0\"/>?>"
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("real-root-child"));
  EXPECT_THAT(result.value, Not(HasSubstr("not-the-root")));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

TEST(ViewportSvgExportTest, RootScannerSkipsDeclarationWithQuotedTerminator) {
  // A `?>` inside a quoted declaration value does not end the declaration
  // (the parser skips quoted spans), so the markup that follows it is not
  // treated as the document root.
  const SVGDocument doc = ParseOrDie(
      "<?xml version=\"1.0\" data='a?><svg id=\"not-the-root\" width=\"0\"/>'?>"
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("real-root-child"));
  EXPECT_THAT(result.value, Not(HasSubstr("not-the-root")));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

TEST(ViewportSvgExportTest, DoctypeInternalSubsetIsRefused) {
  // The parser expands entity references and drops the DOCTYPE node, so the export cannot
  // reproduce the declarations. Refuse rather than emit a body whose entity references no
  // longer resolve.
  const SVGDocument doc = ParseOrDie(
      "<!DOCTYPE svg [<!ENTITY data \"a> <svg id='not-the-root' width='0'/>\">]>"
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("DOCTYPE internal subset"));
}

TEST(ViewportSvgExportTest, DoctypeEntityReferencedFromBodyIsRefused) {
  // The sharp case: the body actually uses the declared entity, so an export carrying the
  // body verbatim without the declaration would reference an undeclared entity.
  const SVGDocument doc = ParseOrDie(
      "<!DOCTYPE svg [<!ENTITY shapeFill \"red\">]>"
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\" fill=\"&shapeFill;\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("DOCTYPE internal subset"));
}

TEST(ViewportSvgExportTest, RootScannerFindsRootCloseBeforeTrailingComment) {
  // A `</svg` sequence after the root inside a trailing comment is comment text,
  // and the body end comes from the root's own closing-tag location, so the
  // comment cannot be mistaken for the root's closing tag.
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>"
      "<!-- trailing </svg> -->");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("real-root-child"));
  EXPECT_THAT(result.value, Not(HasSubstr("trailing")));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

TEST(ViewportSvgExportTest, DoctypeShapedCommentInPrologStillExports) {
  // The refusal comes from what the parser resolved, not from a scan for `<!DOCTYPE` in the
  // prolog bytes: a comment or processing instruction that merely looks like a DOCTYPE
  // declares nothing and must not cost the user their export.
  const SVGDocument doc = ParseOrDie(
      "<!-- <!DOCTYPE svg [<!ENTITY data \"unused\">]> -->"
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("real-root-child"));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

TEST(ViewportSvgExportTest, DoctypeEntityValueWithBracketsIsRefused) {
  // Brackets inside a quoted `<!ENTITY>` value do not affect internal-subset nesting, so the
  // markup-shaped entity value is part of the declaration, and the declaration refuses the
  // export rather than leaking `<svg id="not-the-root">` into the output.
  const SVGDocument doc = ParseOrDie(
      "<!DOCTYPE svg [<!ENTITY data ']><svg id=\"not-the-root\">'>]>"
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("DOCTYPE internal subset"));
  EXPECT_THAT(result.error, Not(HasSubstr("not-the-root")));
}

TEST(ViewportSvgExportTest, DoctypeInsideTheBodyIsRefused) {
  // The internal-subset refusal is document level, because the parser expands the declarations
  // away wherever they appeared. A doctype inside the body is refused for the same reason as
  // one in the prolog, rather than on where its markup-like entity value happens to sit.
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<!DOCTYPE d [<!ENTITY y ']><svg id=\"faux\">'>]>"
      "<rect id=\"real-root-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("DOCTYPE internal subset"));
  EXPECT_THAT(result.error, Not(HasSubstr("faux")));
}

TEST(ViewportSvgExportTest, NestedSvgCloseDoesNotEndRootBody) {
  // Nested `<svg>` elements are tracked, so an inner close cannot be mistaken
  // for the root's: the body extends past it to the root's own close tag.
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<svg id=\"inner\" width=\"10\" height=\"10\"><rect width=\"5\" height=\"5\"/></svg>"
      "<rect id=\"outer-child\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("id=\"inner\""));
  EXPECT_THAT(result.value, HasSubstr("id=\"outer-child\""));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

TEST(ViewportSvgExportTest, NestedSvgWithMissingRootCloseIsRefused) {
  // The tree is stale while the source has errors: export refuses instead of
  // slicing the remainder.
  SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<svg id=\"inner\" width=\"10\" height=\"10\"></svg>"
      "<rect id=\"outer-child\" width=\"10\" height=\"10\"/></svg>");
  const std::size_t closeOffset = doc.source().rfind("</svg>");
  ASSERT_NE(closeOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(closeOffset), FileOffset::Offset(closeOffset + 6)},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, RootScannerRejectsUnterminatedPrologMarkup) {
  for (std::string_view prefix : {"<!--", "<?editor"}) {
    SCOPED_TRACE(prefix);
    SVGDocument doc =
        ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");

    const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
        .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)},
        .replacement = std::string(prefix),
        .sourceVersion = doc.sourceVersion(),
    });
    ASSERT_TRUE(edit.applied);
    ASSERT_TRUE(edit.diagnostic.has_value());

    const Result<std::string, std::string> result =
        ExportViewportAsSvg(doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)),
                            ViewportExportOptions{});

    EXPECT_FALSE(result.ok());
    EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
  }
}

TEST(ViewportSvgExportTest, RootScannerAcceptsSvgNameTerminators) {
  const std::vector<std::string> sources = {
      "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect id=\"plain\" width=\"10\" "
      "height=\"10\"/></svg>",
      "<svg\twidth='100'\nheight = \"80\"\r\nviewBox = '0 0 100 80' "
      "xmlns='http://www.w3.org/2000/svg'><rect id='tabbed' width='10' "
      "height='10'/></svg>",
      "<svg\nxmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"80\">"
      "<rect id=\"newline\" width=\"10\" height=\"10\"/></svg>",
      "<svg\rxmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"80\">"
      "<rect id=\"carriage\" width=\"10\" height=\"10\"/></svg>",
      "<svg xmlns=\"http://www.w3.org/2000/svg\"/>",
  };

  for (const std::string& source : sources) {
    SCOPED_TRACE(source);
    const SVGDocument doc = ParseOrDie(source);
    const Result<std::string, std::string> result = ExportViewportAsSvg(
        doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 80)), ViewportExportOptions{});

    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_THAT(result.value, HasSubstr("<svg"));
    EXPECT_THAT(result.value, HasSubstr("width=\"100\""));
    EXPECT_THAT(result.value, HasSubstr("height=\"80\""));
  }
}

TEST(ViewportSvgExportTest, RootScannerAcceptsRawSlashTerminatedSvgStart) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");
  const std::size_t attributeOffset = doc.source().find(" width=");
  ASSERT_NE(attributeOffset, std::string_view::npos);
  const std::size_t slashOffset = doc.source().find("/>");
  ASSERT_NE(slashOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(attributeOffset), FileOffset::Offset(slashOffset)},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 80)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("<svg width=\"100\" height=\"80\""));
}

TEST(ViewportSvgExportTest, SelfClosingRootExportsEmptyContentGroup) {
  const SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("<g clip-path=\"url(#donner-viewport-clip)\"></g>"));
}

TEST(ViewportSvgExportTest, NonFiniteViewportValuesFormatAsZero) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  ViewportState viewport = IdentityViewport();
  const double infinity = std::numeric_limits<double>::infinity();
  viewport.panDocPoint = Vector2d(infinity, -infinity);
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("viewBox=\"0 0 0 0\""));
}

TEST(ViewportSvgExportTest, NegativeZeroViewportValuesFormatAsZero) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  ViewportState viewport = IdentityViewport();
  viewport.panDocPoint = Vector2d(-0.0, -0.0);
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(0, 0));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("viewBox=\"0 0 0 0\""));
  EXPECT_THAT(result.value, Not(HasSubstr("-0")));
}

TEST(ViewportSvgExportTest, RoundedNegativeZeroViewportValuesFormatAsZero) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  ViewportState viewport = IdentityViewport();
  viewport.panDocPoint = Vector2d(-0.0000001, -0.0000001);
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(0, 0));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("viewBox=\"0 0 0 0\""));
  EXPECT_THAT(result.value, Not(HasSubstr("-0")));
}

TEST(ViewportSvgExportTest, FractionalViewportValuesTrimTrailingZeros) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport =
      MakeViewport(/*zoom=*/3.0, /*panDocPoint=*/Vector2d(0.5, 0.25),
                   /*panScreenPoint=*/Vector2d(0.0, 0.0),
                   /*paneOrigin=*/Vector2d(0.0, 0.0), /*paneSize=*/Vector2d(1.0, 2.0));
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(1, 2));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("viewBox=\"0.5 0.25 0.333333 0.666667\""));
}

TEST(ViewportSvgExportTest, SourceWithoutRootSvgIsRejected) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");
  const std::size_t nameOffset = doc.source().find("svg");
  ASSERT_NE(nameOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(nameOffset), FileOffset::Offset(nameOffset + 3)},
      .replacement = "g",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, MalformedRootOpenTagWithoutTerminatorIsRejected) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");
  const std::size_t selfCloseOffset = doc.source().rfind("/>");
  ASSERT_NE(selfCloseOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(selfCloseOffset), FileOffset::Offset(doc.source().size())},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, MalformedRootOpenTagEndingInWhitespaceIsRejected) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");
  const std::size_t selfCloseOffset = doc.source().rfind("/>");
  ASSERT_NE(selfCloseOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range =
          SourceRange{FileOffset::Offset(selfCloseOffset), FileOffset::Offset(doc.source().size())},
      .replacement = " ",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, MalformedSelfClosingRootWithoutCloseAngleIsRejected) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");
  const std::size_t closeAngleOffset = doc.source().rfind('>');
  ASSERT_NE(closeAngleOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(closeAngleOffset),
                           FileOffset::Offset(closeAngleOffset + 1)},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, EmptyRootIdFallsBackToUntitledProvenance) {
  const SVGDocument doc = ParseOrDie(
      "<svg id=\"\" width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect width=\"10\" height=\"10\"/>"
      "</svg>");

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("source: untitled;"));
  EXPECT_THAT(result.value, HasSubstr("id=\"\""));
}

TEST(ViewportSvgExportTest, SvgPrefixedElementInsertionIsRefused) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)},
      .replacement = "<svgx id=\"not-root\"/>",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, MissingRootCloseTagIsRefused) {
  SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"kept\" width=\"10\" height=\"10\"/></svg>");
  const std::size_t closeOffset = doc.source().find("</svg>");
  ASSERT_NE(closeOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(closeOffset), FileOffset::Offset(closeOffset + 6)},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, HrefLikeTextWithoutQuotedValueIsIgnored) {
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<text>href = https://example.com/not-an-attribute.png</text>"
      "<text>href</text>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("not-an-attribute.png"));
}

TEST(ViewportSvgExportTest, MalformedTrailingSuffixIsRefused) {
  SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<defs><rect id=\"shape\" width=\"10\" height=\"10\"/></defs>"
      "</svg>");

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(doc.source().size()),
                           FileOffset::Offset(doc.source().size())},
      .replacement = " href href= href=\"\" href=\"#shape",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, ExternalReferenceScannerHandlesWhitespaceAndUppercaseSchemes) {
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<image href \n = \r\n \" \tHTTPS://example.com/image.png\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("HTTPS://example.com/image.png"));
}

TEST(ViewportSvgExportTest, ExternalReferenceInCommentIsIgnored) {
  // Only real parsed attributes are inspected: an `href` inside a comment is
  // inert, so it must not refuse the export.
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<!-- <image href=\"https://example.com/commented-out.png\"/> -->"
      "<rect id=\"content\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("id=\"content\""));
}

TEST(ViewportSvgExportTest, DataHrefAttributeIsRefused) {
  // Attribute names ending in "href" keep the historical conservative match.
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect data-href=\"https://example.com/data.png\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("https://example.com/data.png"));
}

TEST(ViewportSvgExportTest, UnterminatedHrefValueIsRefused) {
  // An `href` attribute whose value never terminates (mid-edit) leaves the tree stale,
  // so the staleness gate refuses: failing closed preserves the historical verdict for
  // unparseable values.
  SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<image href=\"https://example.com/x\" width=\"10\" height=\"10\"/>"
      "</svg>");
  const std::size_t valueStart = doc.source().find("https://example.com/x\"");
  ASSERT_NE(valueStart, std::string_view::npos);
  const std::size_t deleteFrom = valueStart + std::string_view("https://example.com/x").size();

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(deleteFrom), FileOffset::Offset(doc.source().size())},
      .replacement = "",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
}

TEST(ViewportSvgExportTest, StaleTreeRefusalNamesUnderlyingReason) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");
  const std::size_t nameOffset = doc.source().find("svg");
  ASSERT_NE(nameOffset, std::string_view::npos);

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(nameOffset), FileOffset::Offset(nameOffset + 3)},
      .replacement = "g",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);
  ASSERT_TRUE(edit.diagnostic.has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("parses cleanly"));
  EXPECT_THAT(result.error, HasSubstr("rename"));
}

TEST(ViewportSvgExportTest, RootAttributesKeepSourceOrderFromTree) {
  // Attribute names deliberately out of alphabetical order: the tree stores them by
  // name, so the exporter must restore source order from their locations.
  const SVGDocument doc = ParseOrDie(
      "<svg zebra=\"1\" apple=\"2\" mango=\"3\" width=\"100\" height=\"100\" "
      "xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect width=\"10\" height=\"10\"/>"
      "</svg>");

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  const std::size_t zebra = result.value.find("zebra=\"1\"");
  const std::size_t apple = result.value.find("apple=\"2\"");
  const std::size_t mango = result.value.find("mango=\"3\"");
  ASSERT_NE(zebra, std::string::npos);
  ASSERT_NE(apple, std::string::npos);
  ASSERT_NE(mango, std::string::npos);
  EXPECT_LT(zebra, apple);
  EXPECT_LT(apple, mango);
}

TEST(ViewportSvgExportTest, AnchorlessRootAttributeFallsBackToDecodedValue) {
  // Inline parsing injects `xmlns` without a source location; the exporter emits
  // its decoded value instead of dropping it.
  SVGParser::Options options;
  options.parseAsInlineSVG = true;
  ParseWarningSink warnings;
  ParseResult<SVGDocument> parsed = SVGParser::ParseSVG(
      "<svg width=\"100\" height=\"100\"><rect width=\"10\" height=\"10\"/></svg>", warnings,
      options);
  ASSERT_FALSE(parsed.hasError()) << parsed.error();
  const SVGDocument doc = std::move(parsed).result();

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("xmlns=\"http://www.w3.org/2000/svg\""));
}

TEST(ViewportSvgExportTest, ExternalHrefInsideForeignNamespaceIsRefused) {
  // Foreign-namespace subtrees are retained in the tree, so references hidden in
  // them are still inspected: both a nested SVG image and an href directly on the
  // foreign element refuse.
  for (std::string_view body :
       {"<other:group xmlns:other=\"http://example.test/other\">"
        "<image href=\"http://example.com/nested.png\" width=\"10\" height=\"10\"/>"
        "</other:group>",
        "<other:group xmlns:other=\"http://example.test/other\" "
        "href=\"http://example.com/direct.png\"/>"}) {
    SCOPED_TRACE(body);
    const SVGDocument doc =
        ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">" +
                   std::string(body) + "</svg>");

    const Result<std::string, std::string> result =
        ExportViewportAsSvg(doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)),
                            ViewportExportOptions{});

    EXPECT_FALSE(result.ok());
    EXPECT_THAT(result.error, HasSubstr("external"));
    EXPECT_THAT(result.error, HasSubstr("http://example.com/"));
  }
}

TEST(ViewportSvgExportTest, EntityEncodedSchemeIsRefused) {
  // Scheme matching runs on decoded values, so `https&#58;//` cannot evade it.
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<image href=\"https&#58;//example.com/x.png\" width=\"10\" height=\"10\"/>"
      "</svg>");

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.error, HasSubstr("external"));
  EXPECT_THAT(result.error, HasSubstr("https://example.com/x.png"));
}

TEST(ViewportSvgExportTest, DuplicateRootAttributeKeepsLastValue) {
  // Duplicate attributes are invalid XML; the parser keeps the last occurrence and
  // the exporter emits it once.
  const SVGDocument doc = ParseOrDie(
      "<svg data-x=\"1\" data-x=\"2\" width=\"100\" height=\"100\" "
      "xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect width=\"10\" height=\"10\"/>"
      "</svg>");

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, Not(HasSubstr("data-x=\"1\"")));
  EXPECT_THAT(result.value, HasSubstr("data-x=\"2\""));
}

TEST(ViewportSvgExportTest, CleanEditBeforeBodyKeepsBoundsCorrect) {
  SVGDocument doc = ParseOrDie(
      "<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect id=\"kept\" width=\"10\" height=\"10\"/>"
      "</svg>");

  const xml::ApplySourceEditResult edit = doc.setElementAttribute(doc.svgElement(), "id", "grown");
  ASSERT_TRUE(edit.applied);
  EXPECT_FALSE(doc.xmlDocument().sourceDiagnostic().has_value());

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("id=\"grown\""));
  EXPECT_THAT(result.value, HasSubstr("id=\"kept\""));
}

// --- Overlay serialization -----------------------------------------------

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

/// A snapshot with a single AABB at (50,60)-(150,160) and one resize handle.
SelectionChromeSnapshot MakeAabbSnapshot() {
  SelectionChromeSnapshot snapshot;
  snapshot.aabbsDoc.push_back(Box2d(Vector2d(50.0, 60.0), Vector2d(150.0, 160.0)));
  snapshot.handleAnchorsDoc.push_back(Vector2d(50.0, 60.0));
  return snapshot;
}

SelectionChromeSnapshot MakePathPointSnapshot() {
  SelectionChromeSnapshot snapshot;
  snapshot.pathControlLinesDoc.push_back(SelectionChromeSnapshot::PathControlLine{
      .anchorDoc = Vector2d(10.0, 20.0),
      .controlDoc = Vector2d(20.0, 10.0),
  });
  snapshot.pathControlPointsDoc.push_back(Vector2d(20.0, 10.0));
  snapshot.pathAnchorPointsDoc.push_back(Vector2d(10.0, 20.0));
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
  ASSERT_THAT(overlay, Not(IsEmpty())) << result.value;
  // The overlay group must now be NON-empty: it contains primitives.
  EXPECT_THAT(overlay, HasSubstr("<rect"));

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
  ASSERT_THAT(overlay, Not(IsEmpty())) << result.value;
  // The resize handle is a white-filled rect.
  EXPECT_THAT(overlay, HasSubstr("fill=\"#ffffff\""));
}

TEST(ViewportSvgExportTest, OverlayPathPointChromeRendered) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakePathPointSnapshot();

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  const std::string overlay = OverlayGroupSpan(result.value);
  ASSERT_THAT(overlay, Not(IsEmpty())) << result.value;
  EXPECT_THAT(overlay, HasSubstr("d=\"M 10 20 L 20 10\""));
  // Squares are sized by the export from the snapshot's own transform, exactly
  // as the editor draws them: 4 logical px for a control point, 5 for an anchor.
  EXPECT_THAT(overlay,
              HasSubstr("<rect x=\"18\" y=\"8\" width=\"4\" height=\"4\" fill=\"#1ea7fd\""));
  EXPECT_THAT(overlay,
              HasSubstr("<rect x=\"7.5\" y=\"17.5\" width=\"5\" height=\"5\" fill=\"#1ea7fd\""));
  EXPECT_THAT(overlay, HasSubstr("stroke=\"none\" stroke-width=\"0\""));
}

TEST(ViewportSvgExportTest, OverlaySelectedPathAndOrientedBoundsRendered) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  SelectionChromeSnapshot snapshot;
  snapshot.paths.push_back(SelectionChromeSnapshot::PathItem{
      .pathDoc = PathBuilder().moveTo(Vector2d(5.0, 6.0)).lineTo(Vector2d(7.0, 8.0)).build(),
  });
  snapshot.paths.push_back(SelectionChromeSnapshot::PathItem{});  // Empty paths are skipped.
  snapshot.orientedBoundsDoc = SelectionChromeSnapshot::OrientedBox{
      .cornersDoc =
          {
              Vector2d(10.0, 20.0),
              Vector2d(30.0, 20.0),
              Vector2d(35.0, 45.0),
              Vector2d(12.0, 50.0),
          },
  };
  snapshot.marqueeDoc = Box2d(Vector2d(1.0, 2.0), Vector2d(3.0, 4.0));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  const std::string overlay = OverlayGroupSpan(result.value);
  ASSERT_THAT(overlay, Not(IsEmpty())) << result.value;
  EXPECT_THAT(overlay, HasSubstr("d=\"M 5 6 L 7 8\""));
  EXPECT_THAT(overlay, Not(HasSubstr("d=\"\"")));
  EXPECT_THAT(overlay, HasSubstr("d=\"M 10 20 L 30 20 L 35 45 L 12 50 Z\""));
  EXPECT_THAT(overlay, HasSubstr("<rect x=\"1\" y=\"2\" width=\"2\" height=\"2\" fill=\"none\""));
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

  // The overlay group reuses the content clipPath; it is not a new one.
  const std::size_t open = result.value.find("<g id=\"donner-editor-overlay\"");
  ASSERT_NE(open, std::string::npos) << result.value;
  const std::size_t openTagEnd = result.value.find('>', open);
  ASSERT_NE(openTagEnd, std::string::npos);
  const std::string openTag = result.value.substr(open, openTagEnd - open);
  EXPECT_THAT(openTag, HasSubstr("clip-path=\"url(#donner-viewport-clip)\""));
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
  ASSERT_THAT(overlay, Not(IsEmpty())) << result.value;
  // Theme-independent stroke color.
  EXPECT_THAT(overlay, HasSubstr("stroke=\"#1ea7fd\""));
}

TEST(ViewportSvgExportTest, OverlayPaintResistsSourceStylesheet) {
  const SVGDocument doc = ParseOrDie(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <style>* { fill: red !important; stroke: none !important; stroke-width: 0 !important; }</style>
</svg>
)svg");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  SVGDocument exported = ParseOrDie(result.value);
  const std::optional<svg::SVGElement> overlayRect =
      exported.querySelector("#donner-editor-overlay rect");
  ASSERT_TRUE(overlayRect.has_value()) << result.value;
  const auto& style = overlayRect->getComputedStyle();
  EXPECT_THAT(style.fill.get(), ::testing::Optional(svg::PaintServer(svg::PaintServer::None{})));
  EXPECT_THAT(style.stroke.get(), ::testing::Optional(svg::PaintServer(svg::PaintServer::Solid(
                                      css::Color(css::RGBA(0x1e, 0xa7, 0xfd, 0xff))))));
  ASSERT_THAT(style.strokeWidth.get(), ::testing::Optional(::testing::_));
  EXPECT_DOUBLE_EQ(style.strokeWidth.get()->value, 1.0);
}

TEST(ViewportSvgExportTest, OverlayScopeIsUniquifiedAgainstSourceIds) {
  const SVGDocument doc = ParseOrDie(R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100" viewBox="0 0 100 100">
  <g id = "donner-editor-overla&#x79;">
    <rect id="source-collision" class="donner-editor-overlay-line"
          x="0" y="0" width="10" height="10" fill="red" stroke="green" stroke-width="7"/>
  </g>
</svg>
)svg");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  ViewportExportOptions options;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  SVGDocument exported = ParseOrDie(result.value);
  const std::optional<svg::SVGElement> sourceRect = exported.querySelector("#source-collision");
  ASSERT_TRUE(sourceRect.has_value()) << result.value;
  const auto& sourceStyle = sourceRect->getComputedStyle();
  EXPECT_THAT(sourceStyle.fill.get(), ::testing::Optional(svg::PaintServer(svg::PaintServer::Solid(
                                          css::Color(css::RGBA(0xff, 0x00, 0x00, 0xff))))));
  EXPECT_THAT(sourceStyle.stroke.get(),
              ::testing::Optional(svg::PaintServer(
                  svg::PaintServer::Solid(css::Color(css::RGBA(0x00, 0x80, 0x00, 0xff))))));
  ASSERT_THAT(sourceStyle.strokeWidth.get(), ::testing::Optional(::testing::_));
  EXPECT_DOUBLE_EQ(sourceStyle.strokeWidth.get()->value, 7.0);

  EXPECT_THAT(result.value, HasSubstr("#donner-editor-overlay-2 .donner-editor-overlay-line"));
  const std::optional<svg::SVGElement> generatedOverlayRect =
      exported.querySelector("#donner-editor-overlay-2 rect");
  ASSERT_TRUE(generatedOverlayRect.has_value()) << result.value;
  const auto& generatedStyle = generatedOverlayRect->getComputedStyle();
  EXPECT_THAT(generatedStyle.stroke.get(),
              ::testing::Optional(svg::PaintServer(
                  svg::PaintServer::Solid(css::Color(css::RGBA(0x1e, 0xa7, 0xfd, 0xff))))));
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

/// Parse and render an SVG string to a bitmap at its intrinsic size.
svg::RendererBitmap RenderSvg(std::string_view svgSource) {
  SVGDocument doc = ParseOrDie(svgSource);
  svg::Renderer renderer;
  renderer.draw(doc);
  return renderer.takeSnapshot();
}

TEST(ViewportSvgExportTest, MeetCropUsesOriginalUniformScaleWithoutLetterbox) {
  constexpr std::string_view kArtwork =
      "<rect width=\"400\" height=\"400\" fill=\"#e6e6e6\"/>"
      "<rect x=\"25\" y=\"50\" width=\"80\" height=\"200\" fill=\"#db3131\"/>"
      "<circle cx=\"170\" cy=\"300\" r=\"20\" fill=\"#315ee0\"/>";
  const SVGDocument doc = ParseOrDie(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"800\" height=\"400\" "
      "viewBox=\"0 0 400 400\">" +
      std::string(kArtwork) + "</svg>");
  const ViewportState viewport = MakeViewport(1.0, Vector2d::Zero(), Vector2d::Zero(),
                                              Vector2d::Zero(), Vector2d(200.0, 400.0));

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(200, 400)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("width=\"200\" height=\"400\" viewBox=\"0 0 200 400\""));

  const std::string expectedSource =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"400\" "
      "viewBox=\"0 0 200 400\">" +
      std::string(kArtwork) + "</svg>";
  tests::CompareBitmapToBitmap(RenderSvg(result.value), RenderSvg(expectedSource),
                               "viewport_export_meet_crop");
}

TEST(ViewportSvgExportTest, NoneCropRetainsIndependentRootScale) {
  constexpr std::string_view kArtwork =
      "<rect width=\"400\" height=\"400\" fill=\"#e6e6e6\"/>"
      "<rect x=\"25\" y=\"50\" width=\"80\" height=\"200\" fill=\"#db3131\"/>";
  const SVGDocument doc = ParseOrDie(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"800\" height=\"400\" "
      "viewBox=\"0 0 400 400\" preserveAspectRatio=\"none\">" +
      std::string(kArtwork) + "</svg>");
  const ViewportState viewport = MakeViewport(1.0, Vector2d::Zero(), Vector2d::Zero(),
                                              Vector2d::Zero(), Vector2d(200.0, 400.0));

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(200, 400)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("width=\"400\" height=\"400\" viewBox=\"0 0 200 400\""));

  const std::string expectedSource =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"400\" "
      "viewBox=\"0 0 200 400\" preserveAspectRatio=\"none\">" +
      std::string(kArtwork) + "</svg>";
  tests::CompareBitmapToBitmap(RenderSvg(result.value), RenderSvg(expectedSource),
                               "viewport_export_none_crop");
}

TEST(ViewportSvgExportTest, SliceCropUsesOriginalUniformScale) {
  constexpr std::string_view kArtwork =
      "<rect width=\"400\" height=\"400\" fill=\"#e6e6e6\"/>"
      "<circle cx=\"100\" cy=\"200\" r=\"60\" fill=\"#315ee0\"/>";
  const SVGDocument doc = ParseOrDie(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"800\" height=\"400\" "
      "viewBox=\"0 0 400 400\" preserveAspectRatio=\"xMidYMid slice\">" +
      std::string(kArtwork) + "</svg>");
  const ViewportState viewport = MakeViewport(1.0, Vector2d(0.0, 100.0), Vector2d::Zero(),
                                              Vector2d::Zero(), Vector2d(200.0, 200.0));

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, viewport, Recti(Vector2i(0, 0), Vector2i(200, 200)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("width=\"400\" height=\"400\" viewBox=\"0 100 200 200\""));

  const std::string expectedSource =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"400\" height=\"400\" "
      "viewBox=\"0 100 200 200\" preserveAspectRatio=\"xMidYMid slice\">" +
      std::string(kArtwork) + "</svg>";
  tests::CompareBitmapToBitmap(RenderSvg(result.value), RenderSvg(expectedSource),
                               "viewport_export_slice_crop");
}

/// Count pixels in @p bmp satisfying @p pred (called with r, g, b, a in 0-255).
int CountPixels(const svg::RendererBitmap& bmp,
                const std::function<bool(int, int, int, int)>& pred) {
  int count = 0;
  for (int y = 0; y < bmp.dimensions.y; ++y) {
    for (int x = 0; x < bmp.dimensions.x; ++x) {
      const std::size_t o =
          static_cast<std::size_t>(y) * bmp.rowBytes + static_cast<std::size_t>(x) * 4u;
      if (pred(bmp.pixels[o], bmp.pixels[o + 1], bmp.pixels[o + 2], bmp.pixels[o + 3])) {
        ++count;
      }
    }
  }
  return count;
}

// The exported SVG, when rendered, must be pixel-identical to the same document
// content shown under the export's viewBox at the export's output size - i.e.
// the export is a faithful screenshot of the viewport crop. The export's
// clip-group wrapper and injected clipPath must not distort the visible region.
TEST(ViewportSvgExportTest, ExportRenderMatchesViewportCrop) {
  const SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;  // transparentBackground = true by default.
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  const svg::RendererBitmap actual = RenderSvg(result.value);

  // Independent reference: kSelfContainedSvg's children under the crop viewBox at
  // the export output size, with no export machinery.
  const svg::RendererBitmap expected = RenderSvg(
      "<svg width=\"400\" height=\"300\" viewBox=\"0 0 400 300\" "
      "xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect x=\"10\" y=\"20\" width=\"100\" height=\"50\" fill=\"red\"/>"
      "<circle cx=\"300\" cy=\"300\" r=\"40\" fill=\"blue\"/></svg>");

  tests::CompareBitmapToBitmap(actual, expected, "viewport_export_matches_crop");
}

// Content that lies outside the exported viewport crop must not appear in the
// rendered export, while content inside the crop must.
TEST(ViewportSvgExportTest, ExportRenderClampsContentOutsideViewport) {
  const SVGDocument doc = ParseOrDie(
      "<svg width=\"600\" height=\"400\" viewBox=\"0 0 600 400\" "
      "xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect x=\"50\" y=\"50\" width=\"60\" height=\"60\" fill=\"#ff0000\"/>"
      "<rect x=\"460\" y=\"50\" width=\"60\" height=\"60\" fill=\"#00ff00\"/></svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(400, 300));

  ViewportExportOptions options;
  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options);
  ASSERT_TRUE(result.ok()) << result.error;

  const svg::RendererBitmap rendered = RenderSvg(result.value);
  ASSERT_EQ(rendered.dimensions.x, 400);
  ASSERT_EQ(rendered.dimensions.y, 300);

  const auto isRed = [](int r, int g, int b, int a) {
    return a > 200 && r > 200 && g < 60 && b < 60;
  };
  const auto isGreen = [](int r, int g, int b, int a) {
    return a > 200 && g > 200 && r < 60 && b < 60;
  };
  EXPECT_GT(CountPixels(rendered, isRed), 0)
      << "the red shape inside the viewport crop must be present in the export";
  EXPECT_EQ(CountPixels(rendered, isGreen), 0)
      << "the green shape outside the viewport crop must be clamped out of the export";
}

// Rendering instantiates shadow-tree entities (for `<use>` and for gradients that inherit
// through `href`) as children of their host element in the same tree the XML facade walks.
// Those entities carry tree structure but no XML node type, so an export that walks the tree
// must skip them instead of asking every child for its node type.
TEST(ViewportSvgExportTest, ExportAfterRenderSkipsShadowTreeEntities) {
  SVGDocument doc = ParseOrDie(
      "<svg width=\"400\" height=\"300\" viewBox=\"0 0 400 300\" "
      "xmlns=\"http://www.w3.org/2000/svg\" "
      "xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
      "<defs>"
      "<linearGradient id=\"base\">"
      "<stop offset=\"0\" stop-color=\"#ff0000\"/>"
      "<stop offset=\"1\" stop-color=\"#0000ff\"/>"
      "</linearGradient>"
      "<linearGradient id=\"derived\" xlink:href=\"#base\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"0\"/>"
      "</defs>"
      "<rect id=\"src\" x=\"10\" y=\"20\" width=\"100\" height=\"50\" fill=\"url(#derived)\"/>"
      "<use xlink:href=\"#src\" x=\"120\"/>"
      "</svg>");

  svg::Renderer renderer;
  renderer.draw(doc);

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(400, 300)), ViewportExportOptions());
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("<use"));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

// The source is a projection of the DOM: appendChild reflects the new element into the source
// text, so the export carries it. The export therefore reproduces source, and a DOM mutation
// reaches the output exactly to the extent it was reflected.
TEST(ViewportSvgExportTest, ExportCarriesReflectedProgrammaticAppend) {
  SVGDocument doc = ParseOrDie(kSelfContainedSvg);
  svg::SVGRectElement added = svg::SVGRectElement::Create(doc);
  added.setAttribute("id", "programmatic");
  svg::SVGSVGElement root = doc.svgElement();
  root.appendChild(added);

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(400, 300)), ViewportExportOptions());
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(doc.source(), HasSubstr("id=\"programmatic\""));
  EXPECT_THAT(result.value, HasSubstr("id=\"programmatic\""));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

// A root spelled with a namespace prefix has no default namespace for the injected wrapper
// markup to fall into, so the export re-emits the root under its own qualified name and gives
// every injected element the same prefix. Unprefixed injections would land in no namespace and
// the clipped group would stop being an SVG group.
TEST(ViewportSvgExportTest, PrefixedRootKeepsInjectedMarkupInTheSvgNamespace) {
  const SVGDocument doc = ParseOrDie(
      "<svg:svg xmlns:svg=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" "
      "viewBox=\"0 0 100 100\">"
      "<svg:rect id=\"body-shape\" width=\"10\" height=\"10\" fill=\"#ff0000\"/>"
      "</svg:svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  ViewportExportOptions options;
  options.transparentBackground = false;
  options.includeSelectionOverlay = true;
  const SelectionChromeSnapshot snapshot = MakeAabbSnapshot();

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, options, &snapshot);
  ASSERT_TRUE(result.ok()) << result.error;

  EXPECT_THAT(result.value, HasSubstr("<svg:svg"));
  EXPECT_THAT(result.value, HasSubstr("</svg:svg>"));
  EXPECT_THAT(result.value, HasSubstr("<svg:defs><svg:clipPath"));
  EXPECT_THAT(result.value, HasSubstr("<svg:g clip-path="));
  EXPECT_THAT(result.value, HasSubstr("<svg:style>"));
  EXPECT_THAT(result.value, HasSubstr("<svg:rect x="));
  EXPECT_TRUE(ReparsesCleanly(result.value));

  // The re-parsed export is a real SVG document: the clipped wrapper resolves to a group and
  // the carried body content is still reachable.
  SVGDocument reparsed = ParseOrDie(result.value);
  EXPECT_TRUE(reparsed.querySelector("#body-shape").has_value());
  EXPECT_TRUE(reparsed.querySelector("#donner-editor-overlay").has_value());
}

// Inline-SVG parsing repairs a root whose prefix is not bound to any namespace by injecting a
// default `xmlns`, which leaves the prefix itself undeclared. Re-emitting that prefix, and
// giving it to the injected elements, would produce markup no consumer can resolve, so the
// export falls back to the unprefixed spelling that the injected `xmlns` covers.
TEST(ViewportSvgExportTest, UnboundRootPrefixIsNotCarriedIntoInjectedMarkup) {
  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  SVGParser::Options options;
  options.parseAsInlineSVG = true;
  ParseResult<SVGDocument> parseResult = SVGParser::ParseSVG(
      "<svg:svg width=\"100\" height=\"100\" viewBox=\"0 0 100 100\">"
      "<rect id=\"body-shape\" width=\"10\" height=\"10\"/>"
      "</svg:svg>",
      warningSink, options);
  ASSERT_FALSE(parseResult.hasError()) << parseResult.error();
  const SVGDocument doc = std::move(parseResult).result();

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});
  ASSERT_TRUE(result.ok()) << result.error;

  EXPECT_THAT(result.value, Not(HasSubstr("<svg:")));
  EXPECT_THAT(result.value, Not(HasSubstr("</svg:")));
  EXPECT_THAT(result.value, HasSubstr("<defs><clipPath"));
  EXPECT_TRUE(ReparsesCleanly(result.value));
}

}  // namespace
}  // namespace donner::editor
