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
#include "donner/svg/parser/SVGParser.h"
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
  // Output dimensions are the render-pane size in CSS px.
  EXPECT_THAT(result.value, HasSubstr("width=\"400\""));
  EXPECT_THAT(result.value, HasSubstr("height=\"300\""));
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
  EXPECT_THAT(result.value,
              HasSubstr("clip-path=\"url(#donner-viewport-clip)\"/>"));
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

  // The overlay group exists with the documented id but is EMPTY in M6.
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
      "width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<rect width=\"10\" height=\"10\"/>"
      "</svg>");
  const ViewportState viewport = IdentityViewport();
  const Recti renderPaneRect(Vector2i(0, 0), Vector2i(100, 100));

  const Result<std::string, std::string> result =
      ExportViewportAsSvg(doc, viewport, renderPaneRect, ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("source: root&amp;amp;source"));
  EXPECT_THAT(result.value, HasSubstr("id=\"root&amp;amp;source\""));
  EXPECT_THAT(result.value, HasSubstr("data-title=\"&quot;quoted&quot;\""));
  EXPECT_THAT(result.value, HasSubstr("data-owner=\"Bob&apos;s\""));
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
    EXPECT_THAT(result.error, HasSubstr("root <svg>"));
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
  EXPECT_THAT(result.error, HasSubstr("root <svg>"));
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
  EXPECT_THAT(result.error, HasSubstr("root <svg>"));
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
  EXPECT_THAT(result.error, HasSubstr("root <svg>"));
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
  EXPECT_THAT(result.error, HasSubstr("root <svg>"));
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

TEST(ViewportSvgExportTest, RootScannerSkipsSvgPrefixedElementNamesBeforeRealRoot) {
  SVGDocument doc =
      ParseOrDie("<svg width=\"100\" height=\"100\" xmlns=\"http://www.w3.org/2000/svg\"/>");

  const xml::ApplySourceEditResult edit = doc.applySourceEdit(xml::XMLEditIntent{
      .range = SourceRange{FileOffset::Offset(0), FileOffset::Offset(0)},
      .replacement = "<svgx id=\"not-root\"/>",
      .sourceVersion = doc.sourceVersion(),
  });
  ASSERT_TRUE(edit.applied);

  const Result<std::string, std::string> result = ExportViewportAsSvg(
      doc, IdentityViewport(), Recti(Vector2i(0, 0), Vector2i(100, 100)), ViewportExportOptions{});

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, Not(HasSubstr("not-root")));
  EXPECT_THAT(result.value, HasSubstr("width=\"100\""));
}

TEST(ViewportSvgExportTest, MissingRootCloseTagUsesSourceRemainderAsBody) {
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

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("id=\"kept\""));
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

TEST(ViewportSvgExportTest, HrefScannerIgnoresMalformedRawSuffixes) {
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

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_THAT(result.value, HasSubstr("id=\"shape\""));
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

/// A snapshot with a single AABB at (50,60)-(150,160) and one resize handle.
SelectionChromeSnapshot MakeAabbSnapshot() {
  SelectionChromeSnapshot snapshot;
  snapshot.aabbsDoc.push_back(Box2d(Vector2d(50.0, 60.0), Vector2d(150.0, 160.0)));
  snapshot.handleBoxesDoc.push_back(Box2d(Vector2d(48.0, 58.0), Vector2d(52.0, 62.0)));
  return snapshot;
}

SelectionChromeSnapshot MakePathPointSnapshot() {
  SelectionChromeSnapshot snapshot;
  snapshot.pathControlLinesDoc.push_back(SelectionChromeSnapshot::PathControlLine{
      .anchorDoc = Vector2d(10.0, 20.0),
      .controlDoc = Vector2d(20.0, 10.0),
  });
  snapshot.pathControlPointBoxesDoc.push_back(Box2d(Vector2d(18.0, 8.0), Vector2d(22.0, 12.0)));
  snapshot.pathAnchorBoxesDoc.push_back(Box2d(Vector2d(8.0, 18.0), Vector2d(12.0, 22.0)));
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
  EXPECT_THAT(overlay,
              HasSubstr("<rect x=\"18\" y=\"8\" width=\"4\" height=\"4\" fill=\"#1ea7fd\""));
  EXPECT_THAT(overlay,
              HasSubstr("<rect x=\"8\" y=\"18\" width=\"4\" height=\"4\" fill=\"#1ea7fd\""));
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

  // The overlay group reuses the M6 content clipPath; it is not a new one.
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

}  // namespace
}  // namespace donner::editor
