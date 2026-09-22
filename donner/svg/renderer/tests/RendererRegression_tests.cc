#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

using Params = ImageComparisonParams;

std::filesystem::path ResvgResourceRoot() {
  return Runfiles::instance().Rlocation("third_party/resvg-test-suite/");
}

/// A 2x2 solid red PNG as a data URI.
constexpr std::string_view kRedImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5E"
    "rkJggg==";

/// A 2x2 solid blue PNG as a data URI. @see kRedImageDataUri
constexpr std::string_view kBlueImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYPgPRmAKABf2A/1+6zfzAAAAAElFTkSu"
    "QmCC";

/// Parses a complete SVG document, failing the test on a parse error.
SVGDocument ParseSvg(std::string_view svg) {
  ParseWarningSink warnings;
  auto maybeResult = parser::SVGParser::ParseSVG(svg, warnings);
  if (maybeResult.hasError()) {
    ADD_FAILURE() << "SVG parse error: " << maybeResult.error();
    return SVGDocument();
  }
  return std::move(maybeResult.result());
}

/// RGBA pixel at (x, y) in a tightly packed snapshot bitmap. Returns transparent for a pixel
/// outside the bitmap so an assertion fails cleanly instead of reading out of bounds.
std::array<uint8_t, 4> PixelAt(const RendererBitmap& bitmap, int x, int y) {
  const size_t offset = static_cast<size_t>(y) * bitmap.rowBytes + static_cast<size_t>(x) * 4u;
  if (offset + 4 > bitmap.pixels.size()) {
    return {0, 0, 0, 0};
  }
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}

/// Counts pixels with alpha above \p threshold in the given device row.
int CountOpaqueInRow(const RendererBitmap& bitmap, int y, uint8_t threshold = 128) {
  int count = 0;
  for (int x = 0; x < bitmap.dimensions.x; ++x) {
    if (PixelAt(bitmap, x, y)[3] > threshold) {
      ++count;
    }
  }
  return count;
}

/// Counts pixels with alpha above \p threshold in the given device column.
int CountOpaqueInColumn(const RendererBitmap& bitmap, int x, uint8_t threshold = 128) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    if (PixelAt(bitmap, x, y)[3] > threshold) {
      ++count;
    }
  }
  return count;
}

/// Length of the first run of pixels with alpha above \p threshold in device row \p y, at or after
/// \p startX. Leading transparent pixels are skipped; returns 0 when the row has no opaque pixel
/// at or after \p startX.
int FirstOpaqueRunLengthInRow(const RendererBitmap& bitmap, int y, int startX,
                              uint8_t threshold = 128) {
  int length = 0;
  for (int x = startX; x < bitmap.dimensions.x; ++x) {
    if (PixelAt(bitmap, x, y)[3] > threshold) {
      ++length;
    } else if (length > 0) {
      break;
    }
  }
  return length;
}

MATCHER_P2(IsWithin, expected, tolerance,
           "is within " + testing::PrintToString(tolerance) + " of " +
               testing::PrintToString(expected)) {
  return arg >= expected - tolerance && arg <= expected + tolerance;
}

/// Asserts a measured device extent, writing the bitmap that produced it as an undeclared output
/// when the measurement is off.
///
/// These measurements are geometric quantities pixelmatch cannot express (a stroke's device width,
/// a dash run length), so they are not bitmap comparisons - but a CI failure still has to ship the
/// image, which is what \ref WriteBitmapToTestOutputs is for.
///
/// A macro rather than a function so the failure names the asserting line rather than this one.
#define EXPECT_MEASURED_EXTENT(measured, expected, tolerance, bitmap, label, why) \
  EXPECT_THAT(measured, IsWithin(expected, tolerance))                            \
      << (why) << "; rendered bitmap: " << WriteBitmapToTestOutputs((bitmap), (label))

/// Markup for a single `vector-effect: non-scaling-stroke` path carrying \p transform, used to
/// compare a mutated document against a freshly parsed one.
std::string NonScalingStrokeMarkup(std::string_view transform) {
  return std::string(R"(<path id="p" d="M 10 10 L 10 26" fill="none" stroke="black")"
                     R"( stroke-width="12" vector-effect="non-scaling-stroke" transform=")") +
         std::string(transform) + R"(" />)";
}

/// A pattern-painted `non-scaling-stroke` under `scale(2, 1)` whose drawn stroke reaches device
/// columns 0..4 while its authored-width cull box ends at device x = -4. \p guardSubpath is
/// prepended to the path data; an off-canvas subpath there widens the cull box without drawing
/// anything inside the viewport, which isolates the culling decision.
std::string PatternEdgeStrokeSvg(std::string_view guardSubpath) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="60" height="60">
      <defs>
        <pattern id="pat" width="4" height="4" patternUnits="userSpaceOnUse">
          <rect width="4" height="4" fill="black"/>
        </pattern>
      </defs>
      <g transform="scale(2, 1)">
        <path d=")svg") +
         std::string(guardSubpath) +
         R"svg(M -12.5 5 L -12 55" fill="none" stroke="url(#pat)" stroke-width="40"
              vector-effect="non-scaling-stroke"/>
      </g>
    </svg>)svg";
}

/// A `non-scaling-stroke` sharp miter whose centerline bounds start at device x = 40, outside the
/// 28px canvas, but whose miter tip reaches device x = 17.6. @see PatternEdgeStrokeSvg for
/// \p guardSubpath.
std::string MiterTipStrokeSvg(std::string_view guardSubpath) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="28" height="60">
      <path d=")svg") +
         std::string(guardSubpath) +
         R"svg(M 100 0 L 40 30 L 100 60" fill="none" stroke="black" stroke-width="20"
            stroke-linejoin="miter" vector-effect="non-scaling-stroke"/>
    </svg>)svg";
}

/// A `non-scaling-stroke` diagonal whose square cap reaches into the viewport while its centerline
/// bounds, inflated by half the stroke width, sit entirely to the right of the canvas. The join is
/// round so the miter allowance cannot mask the cap's contribution. @see PatternEdgeStrokeSvg for
/// \p guardSubpath.
std::string SquareCapStrokeSvg(std::string_view guardSubpath) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="28" height="60">
      <path d=")svg") +
         std::string(guardSubpath) +
         R"svg(M 100 100 L 40 40" fill="none" stroke="black" stroke-width="20"
            stroke-linecap="square" stroke-linejoin="round"
            vector-effect="non-scaling-stroke"/>
    </svg>)svg";
}

/// One `vector-effect: non-scaling-stroke` shape drawn twice under different anisotropic CTMs,
/// either as two `<use>` copies of a single shape entity or as two independent copies of the same
/// markup. The two documents must render identically.
std::string TwoInstanceStrokeSvg(bool useShadowTrees) {
  constexpr std::string_view kShapeAttributes =
      R"svg(d="M 10 10 L 40 10 L 40 40" fill="none" stroke="black" stroke-width="8"
            vector-effect="non-scaling-stroke")svg";

  const std::string shapeAttributes(kShapeAttributes);
  const std::string instance =
      useShadowTrees ? R"svg(<use href="#shape"/>)svg"
                     : std::string(R"svg(<path )svg") + shapeAttributes + R"svg(/>)svg";
  const std::string defs = useShadowTrees ? std::string(R"svg(<defs><path id="shape" )svg") +
                                                shapeAttributes + R"svg(/></defs>)svg"
                                          : std::string();

  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="240">)svg") +
         defs + R"svg(<g transform="scale(2, 1)">)svg" + instance +
         R"svg(</g><g transform="translate(0, 60) scale(1, 3)">)svg" + instance +
         R"svg(</g></svg>)svg";
}

/// A `non-scaling-stroke` shape under `scale(2, 1)` with an opaque fill and a fully transparent
/// stroke, carrying \p paintOrder. The stroke contributes no pixels, so the two paint orders can
/// only differ in where the fill lands.
std::string PaintOrderFillPlacementSvg(std::string_view paintOrder) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
      <g transform="scale(2, 1)">
        <path d="M 10 10 L 50 10 L 50 60 L 10 60 Z" fill="green" stroke="rgba(0, 0, 0, 0)"
              stroke-width="12" vector-effect="non-scaling-stroke" style="paint-order: )svg") +
         std::string(paintOrder) + R"svg(" />
      </g>
    </svg>)svg";
}

/// A dashed `non-scaling-stroke` line with `pathLength`, mapped through \p viewBox into a fixed
/// 200x100 viewport. A viewBox of `0 0 100 50` is a uniform 2x scale; nudging its height makes the
/// same document anisotropic by one part in 5e8.
std::string PathLengthDashSvg(std::string_view viewBox) {
  return std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100"
         preserveAspectRatio="none" viewBox=")svg") +
         std::string(viewBox) + R"svg(">
      <path d="M 0 25 L 50 25" fill="none" stroke="black" stroke-width="4" pathLength="100"
            stroke-dasharray="20 20" vector-effect="non-scaling-stroke"/>
    </svg>)svg";
}

ImageComparisonParams GoldenParams() {
  Params params;
  params.enableGoldenUpdateFromEnv();
  return params;
}

/// Uses the existing pixelmatch assertion to require that two renders are NOT identical, so an
/// equivalence test cannot pass by having neither side render the thing under test.
void ExpectBitmapsDiffer(const RendererBitmap& actual, const RendererBitmap& expected,
                         std::string_view label) {
  testing::TestPartResultArray differences;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &differences);
    ExpectBitmapsIdentical(actual, expected, label);
  }
  EXPECT_THAT(differences.size(), testing::Ge(1)) << label << ": expected the renders to differ";
}

/// Uses the existing pixelmatch assertion to reject a vacuous empty-bitmap identity result.
void ExpectVisibleBitmap(const RendererBitmap& bitmap, std::string_view label) {
  RendererBitmap empty = bitmap;
  std::fill(empty.pixels.begin(), empty.pixels.end(), 0);
  testing::TestPartResultArray differences;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &differences);
    ExpectBitmapsIdentical(bitmap, empty, label);
  }
  EXPECT_THAT(differences.size(), testing::Eq(1)) << "Expected visible rendered content";
}

/// Renders \p body at 200x200 with the hermetic test fonts, through the backend this build is
/// configured around.
RendererBitmap RenderTextOpacityCase(std::string_view body) {
  SVGDocument document = instantiateSubtree(body, parser::SVGParser::Options(), Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(document, ResvgResourceRoot() / "fonts");
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

RendererBitmap RenderImageOpacityCase(bool imageOpacity, bool groupOpacity) {
  std::string body = R"svg(<rect width="64" height="64" fill="white"/>)svg";
  if (groupOpacity) {
    body += R"svg(<g opacity="0.5">)svg";
  }
  body += R"svg(<image x="16" y="16" width="32" height="32")svg";
  if (imageOpacity) {
    body += R"svg( opacity="0.5")svg";
  }
  body += R"svg( href=")svg";
  body += kRedImageDataUri;
  body += R"svg("/>)svg";
  if (groupOpacity) {
    body += "</g>";
  }
  SVGDocument document = instantiateSubtree(body, parser::SVGParser::Options(), Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

class RendererRegressionTests : public ImageComparisonTestFixture {};

TEST_F(RendererRegressionTests, FontPropertiesDoNotChangeReducedCrosshair) {
  SVGDocument reduced = instantiateSubtree(R"(
    <svg width="500" height="500" viewBox="0 0 200 200">
      <path d="M 20 100 L 180 100 M 100 20 L 100 180" stroke="gray" stroke-width="0.5"/>
    </svg>
  )",
                                           {}, Vector2i(500, 500));
  SVGDocument styled = instantiateSubtree(R"(
    <svg width="500" height="500" viewBox="0 0 200 200"
         style="font:italic bold 200px serif; font-kerning:none; font-size-adjust:0.3">
      <path d="M 20 100 L 180 100 M 100 20 L 100 180" stroke="gray" stroke-width="0.5"/>
    </svg>
  )",
                                          {}, Vector2i(500, 500));
  const auto pathOnly = RenderDocumentWithBackend(reduced, RendererBackend::TinySkia);
  const auto withFonts = RenderDocumentWithBackend(styled, RendererBackend::TinySkia);
  ASSERT_THAT(pathOnly.empty(), testing::IsFalse());
  ASSERT_THAT(pathOnly.dimensions, testing::Eq(Vector2i(500, 500)));
  ExpectVisibleBitmap(pathOnly, "crosshair_visible");
  ExpectBitmapsIdentical(withFonts, pathOnly, "font_properties_path_independence");
  std::cout << "Reduced crosshair alpha at (249,100)/(250,100): "
            << static_cast<int>(pathOnly.pixels[100 * pathOnly.rowBytes + 249 * 4 + 3]) << "/"
            << static_cast<int>(pathOnly.pixels[100 * pathOnly.rowBytes + 250 * 4 + 3]) << "\n";
  if (const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const auto straight = pathOnly.alphaType == AlphaType::Premultiplied
                              ? UnpremultiplyRgbaRows(pathOnly.pixels, 500, 500, pathOnly.rowBytes)
                              : CopyTightRgbaRows(pathOnly.pixels, 500, 500, pathOnly.rowBytes);
    const auto image = std::filesystem::path(output) / "font_crosshair_path_only.png";
    ASSERT_THAT(
        RendererImageIO::writeRgbaPixelsToPngFile(image.string().c_str(), straight, 500, 500),
        testing::IsTrue());
  }
}

TEST_F(RendererRegressionTests, FontSizeAdjustMatchesExplicitUsedSize) {
  SVGDocument adjusted = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200" font-family="Noto Sans" font-size="64">
      <text x="100" y="100" text-anchor="middle" font-size-adjust="0.3">Text</text>
    </svg>
  )",
                                            {}, Vector2i(500, 500));
  SVGDocument explicitSize = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200" font-family="Noto Sans" font-size="35.82089552238806">
      <text x="100" y="100" text-anchor="middle">Text</text>
    </svg>
  )",
                                                {}, Vector2i(500, 500));
  RegisterFontsFromDirectoryForTesting(adjusted, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(explicitSize, ResvgResourceRoot() / "fonts");
  const RendererBitmap actual = RenderDocumentWithBackend(adjusted, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(explicitSize, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(500, 500)));
  ExpectVisibleBitmap(actual, "font_size_adjust_visible");
  // Noto Sans has 1000 units per em and an OS/2 x-height of 536: 64 * 0.3 / 0.536.
  ExpectBitmapsIdentical(actual, expected, "font_size_adjust_used_size");
}

TEST_F(RendererRegressionTests, FontSizeAdjustDecorationMatchesExplicitDeclaringSize) {
  for (const std::string decoration : {"underline", "overline", "line-through"}) {
    for (bool childOverride : {false, true}) {
      SCOPED_TRACE(decoration);
      SCOPED_TRACE(childOverride);
      const std::string content =
          childOverride
              ? "<tspan font-family='MPLUS 1p' font-size='20' font-size-adjust='none'>Text</tspan>"
              : "Text";
      const std::string start =
          "<svg viewBox='0 0 200 200' font-family='Noto Sans'><text x='30' y='100' "
          "text-decoration='" +
          decoration + "' ";
      SVGDocument adjusted = instantiateSubtree(
          start + "font-size='64' font-size-adjust='0.3'>" + content + "</text></svg>", {},
          Vector2i(500, 500));
      SVGDocument explicitSize =
          instantiateSubtree(start + "font-size='35.82089552238806'>" + content + "</text></svg>",
                             {}, Vector2i(500, 500));
      RegisterFontsFromDirectoryForTesting(adjusted, ResvgResourceRoot() / "fonts");
      RegisterFontsFromDirectoryForTesting(explicitSize, ResvgResourceRoot() / "fonts");
      const RendererBitmap actual = RenderDocumentWithBackend(adjusted, ActiveRendererBackend());
      const RendererBitmap expected =
          RenderDocumentWithBackend(explicitSize, ActiveRendererBackend());
      ASSERT_THAT(actual.empty(), testing::IsFalse());
      ASSERT_THAT(expected.empty(), testing::IsFalse());
      const std::string label =
          "adjusted_decoration_" + decoration + (childOverride ? "_child" : "_self");
      ExpectVisibleBitmap(actual, label + "_visible");
      ExpectBitmapsIdentical(actual, expected, label);
    }
  }
}

TEST_F(RendererRegressionTests, ZeroAdjustedDecorationDoesNotUseDescendantSize) {
  SVGDocument decorated = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200" font-family="Noto Sans">
      <text x="30" y="100" font-size="64" font-size-adjust="0" text-decoration="underline">
        <tspan font-size="20" font-size-adjust="none">Text</tspan>
      </text>
    </svg>
  )",
                                             {}, Vector2i(500, 500));
  SVGDocument plain = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200" font-family="Noto Sans">
      <text x="30" y="100" font-size="20">Text</text>
    </svg>
  )",
                                         {}, Vector2i(500, 500));
  RegisterFontsFromDirectoryForTesting(decorated, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(plain, ResvgResourceRoot() / "fonts");
  const auto actual = RenderDocumentWithBackend(decorated, ActiveRendererBackend());
  const auto expected = RenderDocumentWithBackend(plain, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ExpectVisibleBitmap(actual, "zero_decoration_visible_text");
  ExpectBitmapsIdentical(actual, expected, "zero_declaring_size_has_no_decoration");
}

TEST_F(RendererRegressionTests, FontShorthandMatchesExpandedLonghands) {
  SVGDocument shorthand = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200">
      <g style="font: italic bold 200px serif; font-kerning: none; font-size-adjust: 0.3">
        <text x="55" y="100" style="font: 50px 'Noto Sans'">AVA</text>
      </g>
    </svg>
  )",
                                             {}, Vector2i(500, 500));
  SVGDocument expanded = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200">
      <text x="55" y="100" style="font-size:50px; font-family:'Noto Sans';
          font-style:normal; font-weight:400; font-kerning:auto; font-size-adjust:none">AVA</text>
    </svg>
  )",
                                            {}, Vector2i(500, 500));
  RegisterFontsFromDirectoryForTesting(shorthand, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(expanded, ResvgResourceRoot() / "fonts");
  const RendererBitmap actual = RenderDocumentWithBackend(shorthand, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(expanded, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(500, 500)));
  ExpectVisibleBitmap(actual, "font_shorthand_visible");
  ExpectBitmapsIdentical(actual, expected, "font_shorthand_expansion");
}

TEST_F(RendererRegressionTests, ExpandedThinCrossbarMatchesReferenceStroke) {
  const Path path = PathBuilder()
                        .moveTo({30, 10})
                        .lineTo({34, 10})
                        .lineTo({34, 24})
                        .lineTo({46, 24})
                        .lineTo({46, 28})
                        .lineTo({34, 28})
                        .lineTo({34, 55})
                        .lineTo({30, 55})
                        .lineTo({30, 28})
                        .lineTo({22, 28})
                        .lineTo({22, 24})
                        .lineTo({30, 24})
                        .closePath()
                        .build();
  const std::string header = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 80 80\">";
  const std::string source = header + "<path d=\"" + std::string(path.toSVGPathData()) +
                             "\" fill=\"none\" stroke=\"#ff00ff\" stroke-width=\"6\"/></svg>";
  const Path expanded = path.strokeToFill({.width = 6.0}, 0.1);
  const std::string outlineSource = header + "<path d=\"" + std::string(expanded.toSVGPathData()) +
                                    "\" fill=\"#ff00ff\" fill-rule=\"nonzero\"/></svg>";
  SVGDocument expectedDocument = instantiateSubtree(source, {}, {80, 80});
  SVGDocument actualDocument = instantiateSubtree(outlineSource, {}, {80, 80});
  const RendererBitmap expected =
      RenderDocumentWithBackend(expectedDocument, RendererBackend::TinySkia);
  const RendererBitmap actual =
      RenderDocumentWithBackend(actualDocument, RendererBackend::TinySkia);
  ExpectBitmapsIdentical(actual, expected, "expanded_thin_crossbar");
  if (IsRendererBackendAvailable(RendererBackend::Geode)) {
    const RendererBitmap geode =
        RenderDocumentWithBackend(expectedDocument, RendererBackend::Geode);
    ExpectBitmapsIdentical(geode, expected, "geode_thin_crossbar");
  }
}

TEST_F(RendererRegressionTests, ThickCrossbarStrokeMatchesItsUnionAtFractionalOffsets) {
  if (!IsRendererBackendAvailable(RendererBackend::Geode)) {
    GTEST_SKIP() << "Requires the Geode backend";
  }
  constexpr std::string_view kCrossbar = "M30 10H34V24H46V28H34V55H30V28H22V24H30Z";
  // A six-unit miter stroke completely covers the four-unit-wide stem and crossbar.
  // Its boundary is the union of the two expanded axis-aligned rectangles.
  constexpr std::string_view kStrokeUnion = "M27 7H37V21H49V31H37V58H27V31H19V21H27Z";
  for (const std::string_view offset : {"0", "0.25", "-0.25"}) {
    for (const std::string_view opacity : {"1", "0.5"}) {
      SCOPED_TRACE(std::string(offset) + "/" + std::string(opacity));
      const std::string header =
          "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 80 80\"><g "
          "transform=\"translate(" +
          std::string(offset) + " " + std::string(offset) + ")\">";
      const std::string strokeSource =
          header + "<path d=\"" + std::string(kCrossbar) +
          "\" fill=\"none\" stroke=\"#ff00ff\" stroke-width=\"6\" stroke-opacity=\"" +
          std::string(opacity) + "\"/></g></svg>";
      const std::string unionSource = header + "<path d=\"" + std::string(kStrokeUnion) +
                                      "\" fill=\"#ff00ff\" fill-opacity=\"" + std::string(opacity) +
                                      "\"/></g></svg>";
      SVGDocument strokeDocument = instantiateSubtree(strokeSource, {}, {80, 80});
      SVGDocument unionDocument = instantiateSubtree(unionSource, {}, {80, 80});
      const RendererBitmap actual =
          RenderDocumentWithBackend(strokeDocument, RendererBackend::Geode);
      const RendererBitmap expected =
          RenderDocumentWithBackend(unionDocument, RendererBackend::Geode);
      const std::string label =
          "crossbar_union_" + std::string(offset) + "_" + std::string(opacity);
      ExpectBitmapsIdentical(actual, expected, label);
    }
  }
}

TEST_F(RendererRegressionTests, FractionalStrokeUnionPreservesGradientCoverage) {
  if (!IsRendererBackendAvailable(RendererBackend::Geode)) {
    GTEST_SKIP() << "Requires the Geode backend";
  }
  for (const std::string_view offset : {"0.25", "-0.25"}) {
    const std::string header =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 80 80\">"
        "<defs><linearGradient id=\"paint\" gradientUnits=\"userSpaceOnUse\" x2=\"80\">"
        "<stop stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\" stop-opacity=\"0.5\"/>"
        "</linearGradient></defs><g transform=\"translate(" +
        std::string(offset) + " " + std::string(offset) + ")\">";
    const std::string stroke =
        header +
        "<path d=\"M30 10H34V24H46V28H34V55H30V28H22V24H30Z\" "
        "fill=\"none\" stroke=\"url(#paint)\" stroke-width=\"6\"/></g></svg>";
    const std::string boundary = header +
                                 "<path d=\"M27 7H37V21H49V31H37V58H27V31H19V21H27Z\" "
                                 "fill=\"url(#paint)\"/></g></svg>";
    SVGDocument actualDocument = instantiateSubtree(stroke, {}, {80, 80});
    SVGDocument expectedDocument = instantiateSubtree(boundary, {}, {80, 80});
    ExpectBitmapsIdentical(RenderDocumentWithBackend(actualDocument, RendererBackend::Geode),
                           RenderDocumentWithBackend(expectedDocument, RendererBackend::Geode),
                           "gradient_stroke_union_" + std::string(offset));
  }
}

TEST_F(RendererRegressionTests, FractionalStrokeUnionPreservesClipMaskCoverage) {
  if (!IsRendererBackendAvailable(RendererBackend::Geode)) {
    GTEST_SKIP() << "Requires the Geode backend";
  }
  const Path centerline = PathBuilder()
                              .moveTo({30, 10})
                              .lineTo({34, 10})
                              .lineTo({34, 24})
                              .lineTo({46, 24})
                              .lineTo({46, 28})
                              .lineTo({34, 28})
                              .lineTo({34, 55})
                              .lineTo({30, 55})
                              .lineTo({30, 28})
                              .lineTo({22, 28})
                              .lineTo({22, 24})
                              .lineTo({30, 24})
                              .closePath()
                              .build();
  const Path pieces = centerline.strokeToFill({.width = 6.0}, 0.1);
  for (const std::string_view offset : {"0.25", "-0.25"}) {
    const auto source = [&](std::string_view path) {
      return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 80 80\">"
             "<defs><clipPath id=\"clip\"><path transform=\"translate(" +
             std::string(offset) + " " + std::string(offset) + ")\" d=\"" + std::string(path) +
             "\"/></clipPath></defs><rect width=\"80\" height=\"80\" fill=\"#ff00ff\" "
             "fill-opacity=\"0.5\" clip-path=\"url(#clip)\"/></svg>";
    };
    SVGDocument actualDocument = instantiateSubtree(source(pieces.toSVGPathData()), {}, {80, 80});
    SVGDocument expectedDocument =
        instantiateSubtree(source("M27 7H37V21H49V31H37V58H27V31H19V21H27Z"), {}, {80, 80});
    ExpectBitmapsIdentical(RenderDocumentWithBackend(actualDocument, RendererBackend::Geode),
                           RenderDocumentWithBackend(expectedDocument, RendererBackend::Geode),
                           "clip_stroke_union_" + std::string(offset));
  }
}

TEST_F(RendererRegressionTests, MarkerPercentResolvesAgainstReferencingViewport) {
  const char* svg = "donner/svg/renderer/testdata/marker_percent_nested_viewport.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/marker_percent_nested_viewport.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, TextDecorationUnderlineRenders) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_decoration_underline.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_decoration_underline.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, InlineSizeAutoFlowWrapsText) {
  // SVG2 inline-size: text greedily wraps to the 150px measure into stacked lines.
  const char* svg = "donner/svg/renderer/testdata/text_inline_size_wrap.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/text_inline_size_wrap.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  Params params = GoldenParams();
  params.withGeodeMaxPixelsDifferent(160);
  renderAndCompare(document, svg, golden, params);
}

TEST_F(RendererRegressionTests, PatternFillOnTextDoesNotLeakToNextShape) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_pattern_fill.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/geode_text_pattern_fill.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, SpanGradientOverridesElementPatternFill) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_span_gradient_over_pattern.svg";
  const char* golden =
      "donner/svg/renderer/testdata/golden/geode_text_span_gradient_over_pattern.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, SpanGradientOverridesElementPatternStroke) {
  const char* svg = "donner/svg/renderer/testdata/geode_text_span_gradient_over_pattern_stroke.svg";
  const char* golden =
      "donner/svg/renderer/testdata/golden/geode_text_span_gradient_over_pattern_stroke.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

TEST_F(RendererRegressionTests, TextOpacityAppliesOnceLikeGroupOpacity) {
  const RendererBitmap textOpacity = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120"
                  opacity="0.5">S</text>)svg");
  const RendererBitmap groupOpacity = RenderTextOpacityCase(
      R"svg(<g opacity="0.5"><text x="20" y="150" font-family="Noto Sans"
                                   font-size="120">S</text></g>)svg");
  const RendererBitmap opaque = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">S</text>)svg");

  ExpectVisibleBitmap(textOpacity, "text_opacity_visible");
  ExpectBitmapsDiffer(textOpacity, opaque, "text_opacity_differs_from_opaque");
  ExpectBitmapsIdentical(textOpacity, groupOpacity, "text_opacity_matches_group_opacity");
}

TEST_F(RendererRegressionTests, ImageOpacityAppliesOnceLikeGroupOpacity) {
  const RendererBitmap imageOpacity = RenderImageOpacityCase(true, false);
  const RendererBitmap groupOpacity = RenderImageOpacityCase(false, true);
  const RendererBitmap opaque = RenderImageOpacityCase(false, false);

  ExpectBitmapsDiffer(imageOpacity, opaque, "image_opacity_differs_from_opaque");
  ExpectBitmapsIdentical(imageOpacity, groupOpacity, "image_opacity_matches_group_opacity");
}

TEST_F(RendererRegressionTests, TextOpacityWithSpanWrapperAppliesOnceLikeGroupOpacity) {
  const RendererBitmap textOpacity = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120" opacity="0.5">
              <tspan>S</tspan></text>)svg");
  const RendererBitmap groupOpacity = RenderTextOpacityCase(
      R"svg(<g opacity="0.5"><text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan>S</tspan></text></g>)svg");
  const RendererBitmap opaque = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan>S</tspan></text>)svg");

  ExpectVisibleBitmap(textOpacity, "text_opacity_span_wrapper_visible");
  ExpectBitmapsDiffer(textOpacity, opaque, "text_opacity_span_wrapper_differs_from_opaque");
  ExpectBitmapsIdentical(textOpacity, groupOpacity,
                         "text_opacity_span_wrapper_matches_group_opacity");
}

TEST_F(RendererRegressionTests, NestedSpanOpacityComposesAsProduct) {
  const RendererBitmap nested = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan opacity="0.5"><tspan opacity="0.5">S</tspan></tspan></text>)svg");
  const RendererBitmap product = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan opacity="0.25">S</tspan></text>)svg");
  const RendererBitmap innerOnly = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan opacity="0.5">S</tspan></text>)svg");

  ExpectVisibleBitmap(nested, "nested_span_opacity_visible");
  ExpectBitmapsDiffer(nested, innerOnly, "nested_span_opacity_keeps_the_intermediate");
  ExpectBitmapsIdentical(nested, product, "nested_span_opacity_is_the_product");
}

TEST_F(RendererRegressionTests, SpanOpacityWithClipPathAppliesOnce) {
  const RendererBitmap spanOpacity = RenderTextOpacityCase(
      R"svg(<defs><clipPath id="clip"><rect width="200" height="200"/></clipPath></defs>
            <text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan opacity="0.5" clip-path="url(#clip)">S</tspan></text>)svg");
  const RendererBitmap groupOpacity = RenderTextOpacityCase(
      R"svg(<defs><clipPath id="clip"><rect width="200" height="200"/></clipPath></defs>
            <g opacity="0.5"><text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan clip-path="url(#clip)">S</tspan></text></g>)svg");
  const RendererBitmap opaque = RenderTextOpacityCase(
      R"svg(<defs><clipPath id="clip"><rect width="200" height="200"/></clipPath></defs>
            <text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan clip-path="url(#clip)">S</tspan></text>)svg");

  ExpectVisibleBitmap(spanOpacity, "span_opacity_clip_path_visible");
  ExpectBitmapsDiffer(spanOpacity, opaque, "span_opacity_clip_path_differs_from_opaque");
  ExpectBitmapsIdentical(spanOpacity, groupOpacity, "span_opacity_clip_path_applies_once");
}

TEST_F(RendererRegressionTests, OpaqueIntermediateSpanLeavesSpanOpacityUnchanged) {
  const RendererBitmap direct = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan opacity="0.5">S</tspan></text>)svg");
  const RendererBitmap throughOpaqueSpan = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan opacity="0.5"><tspan>S</tspan></tspan></text>)svg");
  const RendererBitmap opaque = RenderTextOpacityCase(
      R"svg(<text x="20" y="150" font-family="Noto Sans" font-size="120">
              <tspan><tspan>S</tspan></tspan></text>)svg");

  ExpectVisibleBitmap(direct, "span_opacity_visible");
  ExpectBitmapsDiffer(throughOpaqueSpan, opaque, "opaque_intermediate_span_differs_from_opaque");
  ExpectBitmapsIdentical(throughOpaqueSpan, direct, "opaque_intermediate_span_is_transparent");
}

TEST_F(RendererRegressionTests, NestedBaselineShiftRedrawIsIdempotent) {
  const char* svg = "donner/svg/renderer/testdata/text_nested_baseline_shift_idempotency.svg";
  SVGDocument document = loadSVG(svg, ResvgResourceRoot());

  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ExpectBitmapsIdentical(second, first, "nested_baseline_shift_redraw");
}

TEST_F(RendererRegressionTests, FeImageFragmentRedrawIsIdempotent) {
  const char* svg = "donner/svg/renderer/testdata/feimage_fragment_idempotency.svg";
  SVGDocument document = loadSVG(svg, ResvgResourceRoot());

  const RendererBitmap first = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const RendererBitmap second = RenderDocumentWithBackend(document, RendererBackend::TinySkia);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  ExpectBitmapsIdentical(second, first, "feimage_fragment_redraw");
}

// vector-effect: non-scaling-stroke keeps the stroke a constant device width under a scaled
// viewBox and an additional transform. The golden captures the correct rendering: thin (2px) blue
// non-scaling strokes next to thick (8px / 16px) red strokes that scale with the transform.
TEST_F(RendererRegressionTests, VectorEffectNonScalingStrokeIsConstantWidth) {
  const char* svg = "donner/svg/renderer/testdata/vector_effect_non_scaling_stroke.svg";
  const char* golden = "donner/svg/renderer/testdata/golden/vector_effect_non_scaling_stroke.png";

  SVGDocument document = loadSVG(svg, ResvgResourceRoot());
  renderAndCompare(document, svg, golden, GoldenParams());
}

// Self-validating guard against a silent no-op: the same document with and without the
// vector-effect attribute must render differently. If non-scaling-stroke were ignored, the two
// renders would be identical.
TEST_F(RendererRegressionTests, VectorEffectNonScalingStrokeChangesOutput) {
  const char* nonScalingSvg = "donner/svg/renderer/testdata/vector_effect_non_scaling_stroke.svg";
  const char* controlSvg = "donner/svg/renderer/testdata/vector_effect_scaling_stroke_control.svg";

  SVGDocument nonScalingDoc = loadSVG(nonScalingSvg, ResvgResourceRoot());
  SVGDocument controlDoc = loadSVG(controlSvg, ResvgResourceRoot());

  const RendererBitmap nonScaling =
      RenderDocumentWithBackend(nonScalingDoc, RendererBackend::TinySkia);
  const RendererBitmap control = RenderDocumentWithBackend(controlDoc, RendererBackend::TinySkia);

  ASSERT_FALSE(nonScaling.empty());
  ASSERT_FALSE(control.empty());
  ASSERT_EQ(nonScaling.dimensions, control.dimensions);
  ASSERT_EQ(nonScaling.pixels.size(), control.pixels.size());
  EXPECT_NE(nonScaling.pixels, control.pixels)
      << "vector-effect: non-scaling-stroke had no effect on the rendered output";
}

// `vector-effect: non-scaling-stroke` must hold the authored width under a non-uniform CTM.
// `scale(2, 1)` doubles a vertical stroke's device x extent while leaving a horizontal stroke's y
// extent unchanged; a single area-average scale factor cannot satisfy both. Both strokes must
// measure the authored 10px.
TEST_F(RendererRegressionTests, NonScalingStrokeIsExactUnderAnisotropicScale) {
  SVGDocument document = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="120" height="60">
      <g transform="scale(2, 1)">
        <line x1="20" y1="5" x2="20" y2="55" stroke="black" stroke-width="10"
              vector-effect="non-scaling-stroke"/>
        <line x1="5" y1="40" x2="55" y2="40" stroke="black" stroke-width="10"
              vector-effect="non-scaling-stroke"/>
      </g>
    </svg>)svg");

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  ASSERT_FALSE(bitmap.empty());

  // Device row 30 crosses only the vertical stroke (device x = 40).
  EXPECT_MEASURED_EXTENT(CountOpaqueInRow(bitmap, 30), 10, 2, bitmap,
                         "non_scaling_stroke_anisotropic_scale",
                         "A vertical non-scaling stroke must keep its authored width under "
                         "scale(2, 1)");
  // Device column 100 crosses only the horizontal stroke (device y = 40).
  EXPECT_MEASURED_EXTENT(CountOpaqueInColumn(bitmap, 100), 10, 2, bitmap,
                         "non_scaling_stroke_anisotropic_scale",
                         "A horizontal non-scaling stroke must keep its authored width under "
                         "scale(2, 1)");
}

// The same contract through a non-uniform `viewBox` mapping: a 60x60 viewBox stretched to 120x60
// with `preserveAspectRatio="none"` scales x by 2 and y by 1, so the non-scaling strokes must
// still measure the authored 10px on both axes.
TEST_F(RendererRegressionTests, NonScalingStrokeIsExactUnderNonUniformViewBox) {
  SVGDocument document = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="120" height="60" viewBox="0 0 60 60"
         preserveAspectRatio="none">
      <line x1="20" y1="5" x2="20" y2="55" stroke="black" stroke-width="10"
            vector-effect="non-scaling-stroke"/>
      <line x1="5" y1="40" x2="55" y2="40" stroke="black" stroke-width="10"
            vector-effect="non-scaling-stroke"/>
    </svg>)svg");

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  ASSERT_FALSE(bitmap.empty());

  EXPECT_MEASURED_EXTENT(CountOpaqueInRow(bitmap, 30), 10, 2, bitmap,
                         "non_scaling_stroke_non_uniform_viewbox",
                         "A vertical non-scaling stroke must keep its authored width under a "
                         "non-uniform viewBox");
  EXPECT_MEASURED_EXTENT(CountOpaqueInColumn(bitmap, 100), 10, 2, bitmap,
                         "non_scaling_stroke_non_uniform_viewbox",
                         "A horizontal non-scaling stroke must keep its authored width under a "
                         "non-uniform viewBox");
}

// Dash lengths are host-space CSS pixels for `non-scaling-stroke`, so an anisotropic CTM must not
// stretch them along the axis it scales. Under scale(2, 1) a horizontal dashed stroke would render
// each 10px dash as ~14px if the scalar compensation were still in effect.
TEST_F(RendererRegressionTests, NonScalingStrokeDashLengthIsExactUnderAnisotropicScale) {
  SVGDocument document = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="160" height="40">
      <g transform="scale(2, 1)">
        <line x1="10" y1="20" x2="60" y2="20" stroke="black" stroke-width="4"
              stroke-dasharray="10 10" vector-effect="non-scaling-stroke"/>
      </g>
    </svg>)svg");

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  ASSERT_FALSE(bitmap.empty());

  // The line starts at device x = 20 and the first dash is authored at 10px.
  EXPECT_MEASURED_EXTENT(FirstOpaqueRunLengthInRow(bitmap, 20, 20), 10, 2, bitmap,
                         "non_scaling_stroke_dash_length",
                         "A non-scaling dashed stroke must keep its authored dash length under "
                         "scale(2, 1)");
}

// A `vector-effect: non-scaling-stroke` stroke under a non-similarity CTM is stroked from a
// centerline that the driver transforms into host space, so the element's own transform is baked
// into the geometry both backends memoize per shape entity. Their invalidation only watches
// `ComputedPathComponent`, which a transform-only DOM mutation never touches.
TEST_F(RendererRegressionTests, NonScalingStrokeTransformMutationInvalidatesHostSpaceStrokeCache) {
  const Vector2i canvasSize(64, 64);
  SVGDocument document = instantiateSubtree(NonScalingStrokeMarkup("scale(2, 1)"), {}, canvasSize);

  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(ActiveRendererBackend());
  ASSERT_NE(renderer, nullptr);
  renderer->draw(document);
  const RendererBitmap beforeMutation = renderer->takeSnapshot();
  ASSERT_FALSE(beforeMutation.empty());

  auto path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());
  path->setAttribute("transform", "scale(1, 2)");

  renderer->draw(document);
  const RendererBitmap afterMutation = renderer->takeSnapshot();
  ASSERT_FALSE(afterMutation.empty());

  SVGDocument freshDocument =
      instantiateSubtree(NonScalingStrokeMarkup("scale(1, 2)"), {}, canvasSize);
  const RendererBitmap fresh = RenderDocumentWithBackend(freshDocument, ActiveRendererBackend());
  ASSERT_FALSE(fresh.empty());

  // Guards against a vacuous pass: the mutation has to change the rendering for a stale
  // host-space conversion to be distinguishable from a correctly invalidated one.
  ExpectBitmapsDiffer(fresh, beforeMutation, "non_scaling_stroke_transform_mutation_control");
  ExpectBitmapsIdentical(afterMutation, fresh, "non_scaling_stroke_transform_mutation");
}

// The cull bounds for a `non-scaling-stroke` shape inflate the centerline box in the space the
// stroke is expanded in. A pattern-painted stroke is expanded in local space at
// `authored / sqrt(|det|)`, which under `scale(2, 1)` reaches `2 / sqrt(2)` = ~1.41x the authored
// half width along x. Inflating by the authored half width instead drops a stroke whose drawn
// pixels are inside the viewport.
//
// The control adds an off-canvas subpath whose own stroke never reaches the viewport but whose
// centerline widens the cull box enough to span it, so the only difference between the two
// documents is whether the culling decision fires. The comparison is a device extent rather than
// bitmap identity: the extra subpath perturbs pattern tiling and Geode's band layout by a pixel or
// two, which says nothing about culling. Device row 50 is where the slanted centerline is closest
// to the viewport, so the drawn stroke reaches roughly four columns there.
TEST_F(RendererRegressionTests, PatternNonScalingStrokeAtViewportEdgeIsNotCulled) {
  SVGDocument document = ParseSvg(PatternEdgeStrokeSvg(""));
  SVGDocument controlDocument = ParseSvg(PatternEdgeStrokeSvg("M -5 -40 L -4 -40 "));

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  const RendererBitmap control = RenderDocumentWithActiveBackend(controlDocument);
  ASSERT_FALSE(bitmap.empty());
  ASSERT_FALSE(control.empty());

  // Guards against a vacuous pass: the drawn stroke really does reach the left edge columns.
  const int controlRun = FirstOpaqueRunLengthInRow(control, 50, 0);
  ASSERT_THAT(controlRun, testing::Ge(3))
      << "the uncullable control must paint into the viewport; rendered bitmap: "
      << WriteBitmapToTestOutputs(control, "pattern_non_scaling_stroke_edge_cull_control");

  EXPECT_MEASURED_EXTENT(
      FirstOpaqueRunLengthInRow(bitmap, 50, 0), controlRun, 1, bitmap,
      "pattern_non_scaling_stroke_edge_cull",
      "A pattern-painted non-scaling stroke whose drawn pixels reach the viewport "
      "must not be culled");
}

// A miter join's outer tip sits `strokeWidth / (2 * sin(theta/2))` past the vertex, clamped by
// `stroke-miterlimit`. Here the legs meet at ~53.13 degrees, so the tip reaches 22.4px past the
// vertex while half the stroke width is 10px: a cull box inflated by the half width alone sits
// entirely to the right of the canvas while the tip is inside it. @see PatternEdgeStrokeSvg for
// the control.
TEST_F(RendererRegressionTests, NonScalingStrokeMiterTipInsideViewportIsNotCulled) {
  SVGDocument document = ParseSvg(MiterTipStrokeSvg(""));
  SVGDocument controlDocument = ParseSvg(MiterTipStrokeSvg("M -5 -40 L -4 -40 "));

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  const RendererBitmap control = RenderDocumentWithActiveBackend(controlDocument);
  ASSERT_FALSE(bitmap.empty());
  ASSERT_FALSE(control.empty());

  const int controlRun = CountOpaqueInRow(control, 30);
  ASSERT_THAT(controlRun, testing::Ge(3))
      << "the uncullable control must paint the miter tip; rendered bitmap: "
      << WriteBitmapToTestOutputs(control, "non_scaling_stroke_miter_tip_cull_control");

  EXPECT_MEASURED_EXTENT(CountOpaqueInRow(bitmap, 30), controlRun, 1, bitmap,
                         "non_scaling_stroke_miter_tip_cull",
                         "A sharp miter whose tip reaches the viewport must not be culled");
}

// A square cap extends the segment by half the stroke width, so its far corner sits
// sqrt(2) * halfStroke = 14.1px from the endpoint against a 10px half width. The centerline bounds
// start at device x = 40 and the half-width inflate reaches only x = 30, both outside the 28px
// canvas, while the cap corner reaches x = 25.9. @see
// NonScalingStrokeMiterTipInsideViewportIsNotCulled for the control construction.
TEST_F(RendererRegressionTests, NonScalingStrokeSquareCapInsideViewportIsNotCulled) {
  SVGDocument document = ParseSvg(SquareCapStrokeSvg(""));
  SVGDocument controlDocument = ParseSvg(SquareCapStrokeSvg("M -5 -40 L -4 -40 "));

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  const RendererBitmap control = RenderDocumentWithActiveBackend(controlDocument);
  ASSERT_FALSE(bitmap.empty());
  ASSERT_FALSE(control.empty());

  const int controlRun = CountOpaqueInColumn(control, 27);
  ASSERT_THAT(controlRun, testing::Ge(3))
      << "the uncullable control must paint the cap corner; rendered bitmap: "
      << WriteBitmapToTestOutputs(control, "non_scaling_stroke_square_cap_cull_control");

  EXPECT_MEASURED_EXTENT(CountOpaqueInColumn(bitmap, 27), controlRun, 1, bitmap,
                         "non_scaling_stroke_square_cap_cull",
                         "A square cap reaching into the viewport must not be culled");
}

// One shape entity is drawn twice in a single frame through two `<use>` copies under different
// anisotropic CTMs. Shadow instances share the shape's data entity, so both host-space geometry
// caches are keyed on that entity and must key on the producing transform too; without that the
// second copy is drawn with the first copy's geometry.
TEST_F(RendererRegressionTests, NonScalingStrokeSharedShapeUnderTwoTransformsInOneFrame) {
  SVGDocument document = ParseSvg(TwoInstanceStrokeSvg(/*useShadowTrees=*/true));
  SVGDocument expandedDocument = ParseSvg(TwoInstanceStrokeSvg(/*useShadowTrees=*/false));

  const RendererBitmap shared = RenderDocumentWithActiveBackend(document);
  const RendererBitmap expanded = RenderDocumentWithActiveBackend(expandedDocument);
  ASSERT_FALSE(shared.empty());
  ASSERT_FALSE(expanded.empty());

  ExpectVisibleBitmap(expanded, "non_scaling_stroke_two_instances_expected");
  ExpectBitmapsIdentical(shared, expanded, "non_scaling_stroke_two_instances");
}

// A gradient-painted host-space stroke keeps the gradient authored in the element's user space and
// remaps it onto the host-space outline, with `objectBoundingBox` units still resolving against the
// element's own box. The control bakes the transform into the geometry and carries the same
// gradient explicitly, which must produce the same pixels.
//
// The shape's local bounding box is exactly (0, 0)-(100, 100), so `objectBoundingBox` units and
// `userSpaceOnUse` coordinates scaled by 100 name the same coordinate system, and a rotation
// commutes with the uniform box mapping.
TEST_F(RendererRegressionTests, NonScalingStrokeGradientMatchesPreBakedGeometry) {
  SVGDocument document = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="240" height="140">
      <defs>
        <linearGradient id="grad" gradientUnits="objectBoundingBox" gradientTransform="rotate(30)"
                        x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
      </defs>
      <g transform="scale(2, 1)">
        <path d="M 0 0 L 100 0 L 100 100 L 0 100 Z" fill="none" stroke="url(#grad)"
              stroke-width="16" vector-effect="non-scaling-stroke"/>
      </g>
    </svg>)svg");

  SVGDocument controlDocument = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="240" height="140">
      <defs>
        <linearGradient id="grad" gradientUnits="userSpaceOnUse"
                        gradientTransform="scale(2, 1) rotate(30)"
                        x1="0" y1="0" x2="100" y2="100">
          <stop offset="0" stop-color="red"/>
          <stop offset="1" stop-color="blue"/>
        </linearGradient>
      </defs>
      <path d="M 0 0 L 200 0 L 200 100 L 0 100 Z" fill="none" stroke="url(#grad)"
            stroke-width="16"/>
    </svg>)svg");

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  const RendererBitmap control = RenderDocumentWithActiveBackend(controlDocument);
  ASSERT_FALSE(bitmap.empty());
  ASSERT_FALSE(control.empty());

  ExpectVisibleBitmap(control, "non_scaling_stroke_gradient_control");
  ExpectBitmapsIdentical(bitmap, control, "non_scaling_stroke_gradient");
}

// `paint-order: stroke fill` draws the fill after the host-space stroke pass, which has to leave
// the renderer on the shape's own CTM. The control draws the same fill with the default paint
// order, where it precedes the stroke pass; only the fill is visible in both because the stroke is
// fully transparent.
TEST_F(RendererRegressionTests, NonScalingStrokePaintOrderKeepsFillPlacement) {
  SVGDocument document = ParseSvg(PaintOrderFillPlacementSvg("stroke fill"));
  SVGDocument controlDocument = ParseSvg(PaintOrderFillPlacementSvg("normal"));

  const RendererBitmap bitmap = RenderDocumentWithActiveBackend(document);
  const RendererBitmap control = RenderDocumentWithActiveBackend(controlDocument);
  ASSERT_FALSE(bitmap.empty());
  ASSERT_FALSE(control.empty());

  ExpectVisibleBitmap(control, "non_scaling_stroke_paint_order_control");
  ExpectBitmapsIdentical(bitmap, control, "non_scaling_stroke_paint_order");
}

// `pathLength` is authored against the element's own centerline, so the dash pattern it scales must
// not depend on whether the stroke happens to be drawn in local or host space. These two documents
// straddle the similarity threshold by one part in 5e8, which flips the branch; the rendered dash
// run has to be the same.
TEST_F(RendererRegressionTests, NonScalingStrokeDashPathLengthIsStableAcrossSimilarityBoundary) {
  SVGDocument similarity = ParseSvg(PathLengthDashSvg("0 0 100 50"));
  SVGDocument anisotropic = ParseSvg(PathLengthDashSvg("0 0 100 50.0000001"));

  const RendererBitmap similarityBitmap = RenderDocumentWithActiveBackend(similarity);
  const RendererBitmap anisotropicBitmap = RenderDocumentWithActiveBackend(anisotropic);
  ASSERT_FALSE(similarityBitmap.empty());
  ASSERT_FALSE(anisotropicBitmap.empty());

  // The authored 20-unit dash is held constant in host units, and `pathLength=100` against the
  // 50-unit local centerline scales it by 50 / 100, so each dash covers 10 host pixels. The
  // host-space branch reaches that number only because the dash array is resolved against the
  // local length before the geometry is mapped into host space.
  const int similarityRun = FirstOpaqueRunLengthInRow(similarityBitmap, 50, 0);
  EXPECT_MEASURED_EXTENT(similarityRun, 10, 2, similarityBitmap, "path_length_dash_similarity",
                         "pathLength must resolve the dash pattern against the local centerline");
  EXPECT_MEASURED_EXTENT(FirstOpaqueRunLengthInRow(anisotropicBitmap, 50, 0), similarityRun, 2,
                         anisotropicBitmap, "path_length_dash_anisotropic",
                         "a non-scaling dash run must not jump across the similarity threshold");
}

// The tiny-skia backend memoizes each shape's converted `tiny_skia::Path` on the shape's source
// entity, so a geometry change has to drop that entry before the next draw reads it. The caches
// live on the document, not on the renderer, so the control render has to come from a separately
// parsed document that has no cache entries at all - a second renderer over the same document
// would read the same (possibly stale) entry and agree with a wrong answer.
TEST_F(RendererRegressionTests, PathMutationInvalidatesTinySkiaConvertedPathCache) {
  SVGDocument document = instantiateSubtree(
      R"(<path id="p" d="M 0 0 L 4 0 L 4 4 Z" fill="black"/>)", {}, Vector2i(16, 16));

  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::TinySkia);
  ASSERT_NE(renderer, nullptr);
  renderer->draw(document);
  const RendererBitmap beforeMutation = renderer->takeSnapshot();
  ASSERT_FALSE(beforeMutation.empty());

  auto path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());
  path->setAttribute("d", "M 0 0 L 16 0 L 16 16 Z");

  renderer->draw(document);
  const RendererBitmap afterMutation = renderer->takeSnapshot();
  ASSERT_FALSE(afterMutation.empty());
  // Guards against a vacuous pass: if the mutation were invisible, a stale cache would be
  // indistinguishable from a correctly invalidated one.
  ASSERT_NE(afterMutation.pixels, beforeMutation.pixels)
      << "the `d` mutation must change the rendering for this test to detect a stale conversion";

  SVGDocument freshDocument = instantiateSubtree(
      R"(<path id="p" d="M 0 0 L 16 0 L 16 16 Z" fill="black"/>)", {}, Vector2i(16, 16));
  const RendererBitmap fresh = RenderDocumentWithBackend(freshDocument, RendererBackend::TinySkia);
  ASSERT_FALSE(fresh.empty());
  ExpectBitmapsIdentical(afterMutation, fresh, "tiny_skia_path_cache_invalidation");
}

// Same contract for the premultiplied-image cache, which is keyed by the element that loaded the
// pixels: replacing the `href` drops the loaded image, and the cached conversion has to go with it.
TEST_F(RendererRegressionTests, ImageHrefChangeInvalidatesTinySkiaPremultipliedImageCache) {
  const std::string redMarkup =
      std::string(R"(<image id="i" x="0" y="0" width="16" height="16" href=")") +
      std::string(kRedImageDataUri) + R"(" />)";
  const std::string blueMarkup =
      std::string(R"(<image id="i" x="0" y="0" width="16" height="16" href=")") +
      std::string(kBlueImageDataUri) + R"(" />)";

  auto fragment = instantiateSubtreeElementAs<SVGImageElement>(redMarkup, {}, Vector2i(16, 16));

  std::unique_ptr<RendererInterface> renderer = CreateRendererInstance(RendererBackend::TinySkia);
  ASSERT_NE(renderer, nullptr);
  renderer->draw(fragment.document);
  const RendererBitmap beforeMutation = renderer->takeSnapshot();
  ASSERT_FALSE(beforeMutation.empty());

  fragment->setHref(RcString(kBlueImageDataUri));

  renderer->draw(fragment.document);
  const RendererBitmap afterMutation = renderer->takeSnapshot();
  ASSERT_FALSE(afterMutation.empty());
  ASSERT_NE(afterMutation.pixels, beforeMutation.pixels)
      << "the `href` mutation must change the rendering for this test to detect a stale conversion";

  auto freshFragment =
      instantiateSubtreeElementAs<SVGImageElement>(blueMarkup, {}, Vector2i(16, 16));
  const RendererBitmap fresh =
      RenderDocumentWithBackend(freshFragment.document, RendererBackend::TinySkia);
  ASSERT_FALSE(fresh.empty());
  ExpectBitmapsIdentical(afterMutation, fresh, "tiny_skia_image_cache_invalidation");
}

// Per the SVG 2 object-bounding-box definition, a container's box is the union of its
// children's boxes, and text contributes the union of its glyph cells: advance width by the
// font's full ascent and descent. `visibility: hidden` suppresses painting only, so a hidden text
// child still contributes.
TEST_F(RendererRegressionTests, GroupObjectBoundingBoxUnionsTextChildren) {
  SVGDocument document = instantiateSubtree(R"(
    <svg viewBox="0 0 200 200" font-family="Noto Sans" font-size="40">
      <g id="group">
        <text id="hidden" x="50" y="105" font-size="50" visibility="hidden">Text</text>
        <text id="shown" x="60" y="100">Text</text>
      </g>
    </svg>
  )",
                                            {}, Vector2i(500, 500));
  RegisterFontsFromDirectoryForTesting(document, ResvgResourceRoot() / "fonts");
  // Rendering prepares the text layout that the bounding box is derived from.
  ASSERT_THAT(RenderDocumentWithBackend(document, RendererBackend::TinySkia).empty(),
              testing::IsFalse());

  auto group = document.querySelector("#group");
  auto hidden = document.querySelector("#hidden");
  auto shown = document.querySelector("#shown");
  ASSERT_THAT(group.has_value(), testing::IsTrue());
  ASSERT_THAT(hidden.has_value(), testing::IsTrue());
  ASSERT_THAT(shown.has_value(), testing::IsTrue());

  const Box2d hiddenBox = hidden->cast<SVGTextElement>().objectBoundingBox();
  const Box2d shownBox = shown->cast<SVGTextElement>().objectBoundingBox();
  ASSERT_THAT(hiddenBox.isEmpty(), testing::IsFalse())
      << "a `visibility: hidden` text element still has an object bounding box";
  ASSERT_THAT(shownBox.isEmpty(), testing::IsFalse());
  const Box2d expected = Box2d::Union(hiddenBox, shownBox);

  const std::optional<Box2d> actual =
      components::ShapeSystem().getShapeBounds(group->entityHandle());
  ASSERT_THAT(actual.has_value(), testing::IsTrue())
      << "a group whose only children are <text> must still report an object bounding box";
  EXPECT_THAT(*actual, BoxEq(Vector2Near(expected.topLeft.x, expected.topLeft.y),
                             Vector2Near(expected.bottomRight.x, expected.bottomRight.y)));
}

// `display: none` removes an element and its whole subtree from the rendering tree, so nothing
// inside it contributes to an ancestor's object bounding box. `display` does not inherit, so the
// `<text>` and `<rect>` below still report boxes of their own when asked directly; it is the
// hidden ancestor that keeps them out of the container's box.
TEST_F(RendererRegressionTests, GroupObjectBoundingBoxExcludesDisplayNoneSubtrees) {
  SVGDocument document = instantiateSubtree(R"(
    <svg viewBox="0 0 400 400" font-family="Noto Sans" font-size="40">
      <g id="group">
        <g display="none">
          <text id="hiddenText" x="250" y="300">Text</text>
          <rect id="hiddenRect" x="300" y="330" width="40" height="40"/>
        </g>
        <g>
          <text id="shownText" x="20" y="60">Text</text>
        </g>
      </g>
    </svg>
  )",
                                            {}, Vector2i(500, 500));
  RegisterFontsFromDirectoryForTesting(document, ResvgResourceRoot() / "fonts");
  // Rendering prepares the text layout that the bounding box is derived from.
  ASSERT_THAT(RenderDocumentWithBackend(document, RendererBackend::TinySkia).empty(),
              testing::IsFalse());

  auto group = document.querySelector("#group");
  auto hiddenText = document.querySelector("#hiddenText");
  auto hiddenRect = document.querySelector("#hiddenRect");
  auto shownText = document.querySelector("#shownText");
  ASSERT_THAT(group, testing::Ne(std::nullopt));
  ASSERT_THAT(hiddenText, testing::Ne(std::nullopt));
  ASSERT_THAT(hiddenRect, testing::Ne(std::nullopt));
  ASSERT_THAT(shownText, testing::Ne(std::nullopt));

  // The visible text inside a nested group is the whole of the expected box, which also proves the
  // traversal is not over-pruned into skipping visible nested subtrees.
  const Box2d expected = shownText->cast<SVGTextElement>().objectBoundingBox();
  ASSERT_THAT(expected.isEmpty(), testing::IsFalse());

  const Box2d hiddenTextBox = hiddenText->cast<SVGTextElement>().objectBoundingBox();
  const std::optional<Box2d> hiddenRectBox =
      components::ShapeSystem().getShapeBounds(hiddenRect->entityHandle());
  ASSERT_THAT(hiddenTextBox.isEmpty(), testing::IsFalse()) << "hidden text box: " << hiddenTextBox;
  ASSERT_THAT(hiddenRectBox, testing::Ne(std::nullopt));
  // Both hidden boxes must lie outside the expected box, otherwise including them would not be
  // observable and this test could not fail.
  ASSERT_THAT(hiddenTextBox.topLeft.x, testing::Gt(expected.bottomRight.x));
  ASSERT_THAT(hiddenRectBox->topLeft.x, testing::Gt(expected.bottomRight.x));

  const std::optional<Box2d> actual =
      components::ShapeSystem().getShapeBounds(group->entityHandle());
  ASSERT_THAT(actual.has_value(), testing::IsTrue());
  EXPECT_THAT(*actual, BoxEq(Vector2Near(expected.topLeft.x, expected.topLeft.y),
                             Vector2Near(expected.bottomRight.x, expected.bottomRight.y)));
}

// SVG 2 applies `clip-path`, `mask`, and `filter` to text content elements, so a `tspan` that
// covers all of its text element's content must render exactly as the same effect on the text
// element. Both forms resolve objectBoundingBox effect regions through the same glyph-cell box, so
// the two renders are pixel-identical and neither needs a golden image.
TEST_F(RendererRegressionTests, EffectOnFullCoverageTspanMatchesEffectOnTextElement) {
  struct Effect {
    const char* name;
    const char* defs;
    const char* attribute;
  };
  const Effect kEffects[] = {
      {"clip_path",
       R"svg(<clipPath id="e"><rect x="0" y="0" width="200" height="80"/></clipPath>)svg",
       R"svg(clip-path="url(#e)")svg"},
      {"mask",
       R"svg(<mask id="e"><rect x="20" y="20" width="160" height="160" fill="gray"/></mask>)svg",
       R"svg(mask="url(#e)")svg"},
      {"filter", R"svg(<filter id="e"><feGaussianBlur stdDeviation="4"/></filter>)svg",
       R"svg(filter="url(#e)")svg"},
  };

  for (const Effect& effect : kEffects) {
    SCOPED_TRACE(effect.name);

    const std::string prefix =
        std::string(effect.defs) + R"svg(<g font-family="Noto Sans" font-size="64">)svg";
    const std::string onSpanMarkup = prefix + R"svg(<text x="33" y="100"><tspan )svg" +
                                     effect.attribute + R"svg(>Text</tspan></text></g>)svg";
    const std::string onTextMarkup =
        prefix + R"svg(<text x="33" y="100" )svg" + effect.attribute + R"svg(>Text</text></g>)svg";
    const std::string plainMarkup = prefix + R"svg(<text x="33" y="100">Text</text></g>)svg";

    SVGDocument onSpan = instantiateSubtree(onSpanMarkup, {}, Vector2i(200, 200));
    SVGDocument onText = instantiateSubtree(onTextMarkup, {}, Vector2i(200, 200));
    SVGDocument plain = instantiateSubtree(plainMarkup, {}, Vector2i(200, 200));
    RegisterFontsFromDirectoryForTesting(onSpan, ResvgResourceRoot() / "fonts");
    RegisterFontsFromDirectoryForTesting(onText, ResvgResourceRoot() / "fonts");
    RegisterFontsFromDirectoryForTesting(plain, ResvgResourceRoot() / "fonts");

    const RendererBitmap actual = RenderDocumentWithBackend(onSpan, ActiveRendererBackend());
    const RendererBitmap expected = RenderDocumentWithBackend(onText, ActiveRendererBackend());
    const RendererBitmap unaffected = RenderDocumentWithBackend(plain, ActiveRendererBackend());
    ASSERT_THAT(actual.empty(), testing::IsFalse());
    ASSERT_THAT(expected.empty(), testing::IsFalse());
    ExpectVisibleBitmap(unaffected, std::string("plain_text_visible_") + effect.name);
    ExpectBitmapsDiffer(expected, unaffected, std::string("effect_on_text_changes_") + effect.name);
    ExpectBitmapsIdentical(actual, expected, std::string("effect_on_tspan_") + effect.name);
  }
}

// `visibility` is inherited and a descendant may set it back to `visible`, so a visible span
// inside a `visibility: hidden` `<text>` still paints. The text element renders as a unit through
// one rendering instance, and its draw already filters glyphs per span, so the instance has to
// survive the element's own hidden style for that filter to have anything to run on.
TEST_F(RendererRegressionTests, VisibleSpanInsideHiddenTextRootStillPaints) {
  const std::string kPrefix = R"svg(<g font-family="Noto Sans" font-size="40">)svg";
  const std::string kSuffix = R"svg(</g>)svg";
  const std::string kHiddenRoot =
      R"svg(<text x="20" y="60" visibility="hidden"><tspan visibility="visible">Text</tspan>)svg"
      R"svg(</text>)svg";
  const std::string kVisibleRoot =
      R"svg(<text x="20" y="60"><tspan visibility="visible">Text</tspan></text>)svg";
  const std::string kNoOverride =
      R"svg(<text x="20" y="60" visibility="hidden"><tspan>Text</tspan></text>)svg";

  SVGDocument hiddenRoot =
      instantiateSubtree(kPrefix + kHiddenRoot + kSuffix, {}, Vector2i(200, 200));
  SVGDocument visibleRoot =
      instantiateSubtree(kPrefix + kVisibleRoot + kSuffix, {}, Vector2i(200, 200));
  SVGDocument noOverride =
      instantiateSubtree(kPrefix + kNoOverride + kSuffix, {}, Vector2i(200, 200));
  SVGDocument noText = instantiateSubtree(kPrefix + kSuffix, {}, Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(hiddenRoot, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(visibleRoot, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(noOverride, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(noText, ResvgResourceRoot() / "fonts");

  const RendererBitmap actual = RenderDocumentWithBackend(hiddenRoot, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(visibleRoot, ActiveRendererBackend());
  const RendererBitmap inherited = RenderDocumentWithBackend(noOverride, ActiveRendererBackend());
  const RendererBitmap blank = RenderDocumentWithBackend(noText, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse()) << "the hidden-root document rendered nothing";
  ASSERT_THAT(expected.empty(), testing::IsFalse()) << "the visible-root document rendered nothing";

  // The span paints at all, so the comparison below cannot hold with nothing painted on either
  // side.
  ExpectVisibleBitmap(expected, "visible_span_inside_visible_text_root");
  ExpectBitmapsIdentical(actual, expected, "visible_span_inside_hidden_text_root");
  // A span that inherits the hidden root's visibility still paints nothing, so the fix is a filter
  // on the instance's spans rather than the removal of the visibility gate.
  ExpectBitmapsIdentical(inherited, blank, "hidden_text_root_without_visible_span");
}

// `visibility` is inherited, and a descendant may set it back to `visible`, so a visible span
// nested inside a `visibility: hidden` span still paints. A span that declares `clip-path`, `mask`
// or `filter` is painted by its own rendering instance, and the visible descendant's glyphs belong
// to that instance, so suppressing the instance from the hidden span's own style would drop them.
// A full-coverage clip path changes nothing it is applied to, which makes the render with the
// effect comparable to the same markup without it.
TEST_F(RendererRegressionTests, VisibleSpanInsideHiddenEffectSpanStillPaints) {
  const std::string kPrefix =
      R"svg(<clipPath id="c"><rect x="0" y="0" width="200" height="200"/></clipPath>)svg"
      R"svg(<g font-family="Noto Sans" font-size="40">)svg";
  const std::string kSuffix = R"svg(</g>)svg";
  const std::string kVisibleInsideHidden =
      R"svg(<text x="20" y="60"><tspan visibility="hidden" clip-path="url(#c)">)svg"
      R"svg(<tspan visibility="visible">Text</tspan></tspan></text>)svg";
  const std::string kVisibleInsideHiddenNoEffect =
      R"svg(<text x="20" y="60"><tspan visibility="hidden">)svg"
      R"svg(<tspan visibility="visible">Text</tspan></tspan></text>)svg";
  const std::string kAllHidden =
      R"svg(<text x="20" y="60"><tspan visibility="hidden" clip-path="url(#c)">Text</tspan>)svg"
      R"svg(</text>)svg";

  SVGDocument withEffect =
      instantiateSubtree(kPrefix + kVisibleInsideHidden + kSuffix, {}, Vector2i(200, 200));
  SVGDocument withoutEffect =
      instantiateSubtree(kPrefix + kVisibleInsideHiddenNoEffect + kSuffix, {}, Vector2i(200, 200));
  SVGDocument allHidden =
      instantiateSubtree(kPrefix + kAllHidden + kSuffix, {}, Vector2i(200, 200));
  SVGDocument noText = instantiateSubtree(kPrefix + kSuffix, {}, Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(withEffect, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(withoutEffect, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(allHidden, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(noText, ResvgResourceRoot() / "fonts");

  const RendererBitmap actual = RenderDocumentWithBackend(withEffect, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(withoutEffect, ActiveRendererBackend());
  const RendererBitmap hiddenOnly = RenderDocumentWithBackend(allHidden, ActiveRendererBackend());
  const RendererBitmap blank = RenderDocumentWithBackend(noText, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(expected.empty(), testing::IsFalse());

  // The override has to be observable without the effect, otherwise the comparison below would
  // hold with nothing painted on either side.
  ExpectVisibleBitmap(expected, "visible_span_inside_hidden_span_no_effect");
  ExpectBitmapsIdentical(actual, expected, "visible_span_inside_hidden_effect_span");
  // A hidden span with no visible descendant still paints nothing, so the fix is a filter on the
  // effect instance's spans rather than the removal of the visibility gate.
  ExpectBitmapsIdentical(hiddenOnly, blank, "hidden_effect_span_without_visible_descendant");
}

// A `<use>` copy renders the referenced text through the light tree's laid-out spans, but the
// per-span instances that carry `clip-path`, `mask`, and `filter` exist only in the light tree.
// The copy must still paint every span; it does so without those effects, which is what it did
// before spans could own an effect at all. Pinning that as an equivalence keeps a later change
// deliberate: once a copy instantiates its own span instances, this comparison must be updated.
//
// The clip rect covers the referenced text where it renders in place and excludes the copy, so
// the referenced text is identical in both documents and only the copy can differ.
TEST_F(RendererRegressionTests, UseCopyPaintsEveryTextSpanWithoutSpanEffects) {
  const std::string kPrefix =
      R"svg(<clipPath id="c"><rect x="0" y="0" width="200" height="120"/></clipPath>)svg"
      R"svg(<g font-family="Noto Sans" font-size="40"><text id="t" x="20" y="60">)svg";
  const std::string kSuffix = R"svg(</text></g>)svg";
  const std::string kUse = R"svg(<use href="#t" y="100"/>)svg";
  const std::string kSpanWithEffect = R"svg(<tspan clip-path="url(#c)">Text</tspan>)svg";
  const std::string kSpanWithoutEffect = R"svg(<tspan>Text</tspan>)svg";

  SVGDocument withEffect =
      instantiateSubtree(kPrefix + kSpanWithEffect + kSuffix + kUse, {}, Vector2i(200, 200));
  SVGDocument withoutEffect =
      instantiateSubtree(kPrefix + kSpanWithoutEffect + kSuffix + kUse, {}, Vector2i(200, 200));
  SVGDocument withoutUse =
      instantiateSubtree(kPrefix + kSpanWithEffect + kSuffix, {}, Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(withEffect, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(withoutEffect, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(withoutUse, ResvgResourceRoot() / "fonts");

  const RendererBitmap actual = RenderDocumentWithBackend(withEffect, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(withoutEffect, ActiveRendererBackend());
  const RendererBitmap noCopy = RenderDocumentWithBackend(withoutUse, ActiveRendererBackend());
  ASSERT_THAT(actual.empty(), testing::IsFalse());
  ASSERT_THAT(expected.empty(), testing::IsFalse());
  ExpectBitmapsDiffer(actual, noCopy, "use_copy_of_effect_span_adds_ink");
  ExpectBitmapsIdentical(actual, expected, "use_copy_paints_span_without_effect");
}

// `<a>` carries the text components unconditionally so it can act as a text content element inside
// text, so an objectBoundingBox effect on an `<a>` that groups ordinary graphics must resolve its
// region from the children's shapes rather than reaching the text engine, which requires a text
// root. Outside text `<a>` is an ordinary group, so each form must render exactly as `<g>` does.
TEST_F(RendererRegressionTests, ObjectBoundingBoxEffectOnAnchorGroupingShapes) {
  struct Effect {
    const char* name;
    const char* defs;
    const char* attribute;
    /// False for an effect a container element does not apply yet, where comparing against the
    /// same document without the effect would prove nothing.
    bool observableOnAContainer;
  };
  const Effect kEffects[] = {
      {"clip_path", R"svg(<clipPath id="e"><circle cx="90" cy="80" r="40"/></clipPath>)svg",
       R"svg(clip-path="url(#e)")svg", true},
      // A `mask` on a container element is dropped. That predates this behavior and is unrelated
      // to it: reverting the bounding box source leaves the same result, and `<g>` in place of
      // `<a>` renders identically, so only the equivalence below is asserted for it.
      {"mask",
       R"svg(<mask id="e"><rect x="0" y="0" width="90" height="200" fill="white"/></mask>)svg",
       R"svg(mask="url(#e)")svg", false},
      {"filter", R"svg(<filter id="e"><feGaussianBlur stdDeviation="3"/></filter>)svg",
       R"svg(filter="url(#e)")svg", true},
  };

  for (const Effect& effect : kEffects) {
    SCOPED_TRACE(effect.name);

    const std::string rect = R"svg(<rect x="33" y="40" width="120" height="80"/>)svg";
    const std::string onAnchor =
        std::string(effect.defs) + "<a " + effect.attribute + ">" + rect + "</a>";
    const std::string onGroup =
        std::string(effect.defs) + "<g " + effect.attribute + ">" + rect + "</g>";
    const std::string plainMarkup = std::string(effect.defs) + rect;

    SVGDocument anchor = instantiateSubtree(onAnchor, {}, Vector2i(200, 200));
    SVGDocument group = instantiateSubtree(onGroup, {}, Vector2i(200, 200));
    SVGDocument plain = instantiateSubtree(plainMarkup, {}, Vector2i(200, 200));

    const RendererBitmap actual = RenderDocumentWithBackend(anchor, ActiveRendererBackend());
    const RendererBitmap expected = RenderDocumentWithBackend(group, ActiveRendererBackend());
    const RendererBitmap unaffected = RenderDocumentWithBackend(plain, ActiveRendererBackend());
    ASSERT_THAT(actual.empty(), testing::IsFalse());
    ASSERT_THAT(expected.empty(), testing::IsFalse());
    if (effect.observableOnAContainer) {
      ExpectBitmapsDiffer(expected, unaffected,
                          std::string("anchor_effect_changes_") + effect.name);
    }
    ExpectBitmapsIdentical(actual, expected, std::string("anchor_effect_") + effect.name);
  }
}

// A span that owns an effect paints after the spans the text root still paints, rather than in
// document order. That is a known limitation of giving a span its own rendering instance, and this
// pins it so a change to it is deliberate.
//
// The first span's filter floods a region wide enough to reach over the first glyph of the later
// span, and only that glyph. Sampling inside its stem discriminates the two orders: the flood wins
// there only because the span that owns it paints last. The later span's second glyph sits outside
// the flood region and stays its own color, which proves the sampled pixel is glyph ink the flood
// covered rather than a gap between the spans.
TEST_F(RendererRegressionTests, EffectSpanPaintsAfterTheTextRootsRemainingSpans) {
  const std::string markup =
      R"svg(<filter id="e" x="-5%" y="-5%" width="160%" height="110%">)svg"
      R"svg(<feFlood flood-color="blue"/></filter>)svg"
      R"svg(<g font-family="Noto Sans" font-size="64"><text x="20" y="100">)svg"
      R"svg(<tspan filter="url(#e)">AA</tspan><tspan fill="red">BB</tspan>)svg"
      R"svg(</text></g>)svg";
  SVGDocument document = instantiateSubtree(markup, {}, Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(document, ResvgResourceRoot() / "fonts");
  const RendererBitmap bitmap = RenderDocumentWithBackend(document, ActiveRendererBackend());
  ASSERT_THAT(bitmap.empty(), testing::IsFalse());
  ASSERT_THAT(bitmap.dimensions, testing::Eq(Vector2i(200, 200)));

  // Noto Sans at 64 px: the filtered span's glyph cells run x 20 to 101.8, so a 160% region ends
  // at x 146.8. The later span's glyphs ink x 108.0 to 139.9 and x 149.6 to 181.5, putting the
  // first inside the flood and the second outside it.
  EXPECT_THAT(PixelAt(bitmap, 111, 75), test::RgbaEq(0, 0, 255, 255))
      << "the span that owns the filter must paint after the text root's remaining spans";
  EXPECT_THAT(PixelAt(bitmap, 152, 75), test::RgbaEq(255, 0, 0, 255))
      << "the sampled glyph must be ink the flood covered, not a gap between the spans";
}

}  // namespace
}  // namespace donner::svg
