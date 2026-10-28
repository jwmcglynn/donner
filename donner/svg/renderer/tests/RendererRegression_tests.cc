#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <memory>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGFEOffsetElement.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGTSpanElement.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextEngine.h"
#include "tiny_skia/Painter.h"
#include "tiny_skia/Pixmap.h"

namespace donner::svg {
namespace {

using test::ExpectSnapshotHasPixels;
using test::PixelAt;

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

/// Counts pixels with alpha above \p threshold in the given device row. An empty snapshot fails
/// the calling test instead of counting as a row with no opaque pixel.
int CountOpaqueInRow(const RendererBitmap& bitmap, int y, uint8_t threshold = 128,
                     std::source_location caller = std::source_location::current()) {
  if (!ExpectSnapshotHasPixels(bitmap, caller)) {
    return 0;
  }
  int count = 0;
  for (int x = 0; x < bitmap.dimensions.x; ++x) {
    if (PixelAt(bitmap, x, y, caller)[3] > threshold) {
      ++count;
    }
  }
  return count;
}

/// Counts pixels with alpha above \p threshold in the given device column. An empty snapshot fails
/// the calling test instead of counting as a column with no opaque pixel.
int CountOpaqueInColumn(const RendererBitmap& bitmap, int x, uint8_t threshold = 128,
                        std::source_location caller = std::source_location::current()) {
  if (!ExpectSnapshotHasPixels(bitmap, caller)) {
    return 0;
  }
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    if (PixelAt(bitmap, x, y, caller)[3] > threshold) {
      ++count;
    }
  }
  return count;
}

/// Length of the first run of pixels with alpha above \p threshold in device row \p y, at or after
/// \p startX. Leading transparent pixels are skipped; returns 0 when the row has no opaque pixel
/// at or after \p startX. An empty snapshot fails the calling test instead of measuring no run.
int FirstOpaqueRunLengthInRow(const RendererBitmap& bitmap, int y, int startX,
                              uint8_t threshold = 128,
                              std::source_location caller = std::source_location::current()) {
  if (!ExpectSnapshotHasPixels(bitmap, caller)) {
    return 0;
  }
  int length = 0;
  for (int x = startX; x < bitmap.dimensions.x; ++x) {
    if (PixelAt(bitmap, x, y, caller)[3] > threshold) {
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

/// Uses the existing pixelmatch assertion to reject a vacuous empty-bitmap identity result. An
/// empty render fails here, outside the captured comparison, because its failure there would read
/// as visible content.
void ExpectVisibleBitmap(const RendererBitmap& bitmap, std::string_view label,
                         std::source_location caller = std::source_location::current()) {
  if (!ExpectSnapshotHasPixels(bitmap, caller)) {
    return;
  }
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

/// A renderer that could not read its frame back returns an empty bitmap, which has no content to
/// be visible, to differ, or to count. Each guard this suite uses must reject one, because an
/// equivalence or extent test whose renders all came back empty would otherwise pass.
TEST(RendererRegressionHelpersTest, AnEmptyRenderIsNeitherVisibleNorDifferentNorCounted) {
  EXPECT_NONFATAL_FAILURE(CountOpaqueInRow(RendererBitmap{}, 0), "empty 0x0 snapshot");
  EXPECT_NONFATAL_FAILURE(CountOpaqueInColumn(RendererBitmap{}, 0), "empty 0x0 snapshot");
  EXPECT_NONFATAL_FAILURE(FirstOpaqueRunLengthInRow(RendererBitmap{}, 0, 0), "empty 0x0 snapshot");
  EXPECT_NONFATAL_FAILURE(ExpectVisibleBitmap(RendererBitmap{}, "visible"), "empty 0x0 snapshot");
  EXPECT_NONFATAL_FAILURE(ExpectBitmapsDiffer(RendererBitmap{}, RendererBitmap{}, "differ"),
                          "empty 0x0 snapshot");
}

/// Rejects vacuous fixtures with the canonical comparison against a transparent bitmap.
void AssertVisibleFixture(const RendererBitmap& bitmap, std::string_view label) {
  ASSERT_THAT(bitmap.empty(), testing::IsFalse());
  RendererBitmap empty = bitmap;
  std::fill(empty.pixels.begin(), empty.pixels.end(), 0);
  int visiblePixels = -1;
  testing::TestPartResultArray differences;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &differences);
    editor::tests::CompareBitmapToBitmap(bitmap, empty, label,
                                         editor::tests::PixelmatchIdentityParams(), &visiblePixels);
  }
  EXPECT_THAT(visiblePixels, testing::Gt(0));
}

/// Renders \p body at 200x200 with the hermetic test fonts and the configured backend.
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

// Independent contract oracles for resvg cases whose historical labels or reference images
// do not establish the behavior of the supported syntax.
RendererBitmap RenderReferenceContract(std::string_view body) {
  SVGDocument document = instantiateSubtree(body, {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

RendererBitmap RenderReferenceRadial(std::string_view attributes, bool singleStop = false) {
  return RenderReferenceContract(
      std::string(R"svg(<radialGradient id="g" )svg") + std::string(attributes) +
      (singleStop ? R"svg(><stop stop-color="blue"/>)svg"
                  : R"svg(><stop stop-color="red"/><stop offset="1" stop-color="blue"/>)svg") +
      R"svg(</radialGradient>
      <rect x="8" y="8" width="48" height="48" fill="url(#g)"/>
      <rect x="1" y="1" width="3" height="3" fill="lime"/>)svg");
}

TEST_F(RendererRegressionTests, ReferenceContractRadialNegativeRadiiDoNotPaint) {
  const auto expected =
      RenderReferenceContract(R"svg(<rect x="1" y="1" width="3" height="3" fill="lime"/>)svg");
  ExpectVisibleBitmap(expected, "radial_negative_sibling_visible");
  for (std::string_view attribute :
       {R"(r="-0.25")", R"(fr="-0.25")", R"(r="0" fr="-0.25")", R"(r="-1e-20")"}) {
    SCOPED_TRACE(attribute);
    ExpectBitmapsIdentical(RenderReferenceRadial(attribute), expected, "negative_radius_no_paint");
  }
  const auto zeroRadius = RenderReferenceRadial(R"(r="0")");
  ExpectBitmapsDiffer(zeroRadius, expected, "zero_radius_last_stop_control");
  EXPECT_THAT(PixelAt(zeroRadius, 32, 32), testing::ElementsAre(0, 0, 255, 255));
}

TEST_F(RendererRegressionTests, ReferenceContractRadialSingleStopNegativeRadiiDoNotPaint) {
  const auto expected =
      RenderReferenceContract(R"svg(<rect x="1" y="1" width="3" height="3" fill="lime"/>)svg");
  for (std::string_view attribute : {R"(r="-0.25")", R"(fr="-0.25")", R"(r="-1e-20")"}) {
    SCOPED_TRACE(attribute);
    ExpectBitmapsIdentical(RenderReferenceRadial(attribute, true), expected,
                           "single_stop_negative_radius_no_paint");
  }
  const auto valid = RenderReferenceRadial("", true);
  ExpectBitmapsDiffer(valid, expected, "single_stop_valid_control");
  EXPECT_THAT(PixelAt(valid, 32, 32), testing::ElementsAre(0, 0, 255, 255));
}

TEST_F(RendererRegressionTests, ReferenceContractRadialContainingStartCircleDoesNotPaint) {
  const auto expected =
      RenderReferenceContract(R"svg(<rect x="1" y="1" width="3" height="3" fill="lime"/>)svg");
  for (std::string_view attribute : {R"(fr="0.5")", R"(fr="0.75")"}) {
    SCOPED_TRACE(attribute);
    ExpectBitmapsIdentical(RenderReferenceRadial(attribute), expected, "contained_circle_no_paint");
  }
  ExpectBitmapsDiffer(RenderReferenceRadial(R"(fr="0.25")"), expected, "nonempty_annulus_control");
}

TEST_F(RendererRegressionTests, ReferenceContractSingularRadialTransformDoesNotPaint) {
  const auto expected =
      RenderReferenceContract(R"svg(<rect x="1" y="1" width="3" height="3" fill="lime"/>)svg");
  ExpectBitmapsIdentical(RenderReferenceRadial(R"svg(gradientTransform="matrix(0 0 0 0 0 0)")svg"),
                         expected, "singular_radial_no_paint");
  ExpectBitmapsDiffer(RenderReferenceRadial(""), expected, "ordinary_radial_control");
}

TEST_F(RendererRegressionTests, ReferenceContractInvalidGradientUnitsUseInitialValue) {
  const auto expected = RenderReferenceRadial(R"(gradientUnits="objectBoundingBox")");
  ExpectVisibleBitmap(expected, "radial_default_units_visible");
  ExpectBitmapsIdentical(RenderReferenceRadial(R"(gradientUnits="invalid")"), expected,
                         "invalid_gradient_units_default");
}

TEST_F(RendererRegressionTests, ReferenceContractSpaceSeparatedRgbMatchesHex) {
  const auto actual = RenderReferenceContract(
      R"svg(<rect x="8" y="8" width="48" height="48" fill="rgb(17 133 49)"/>)svg");
  const auto expected =
      RenderReferenceContract(R"svg(<rect x="8" y="8" width="48" height="48" fill="#118531"/>)svg");
  ExpectVisibleBitmap(actual, "space_rgb_visible");
  ExpectBitmapsIdentical(actual, expected, "space_rgb_hex_identity");
}

TEST_F(RendererRegressionTests, ReferenceContractObsoleteIccPaintRejectsWholeDeclaration) {
  const auto actual = RenderReferenceContract(R"svg(
    <linearGradient id="g"><stop stop-color="red"/><stop offset="1" stop-color="blue"/>
    </linearGradient><g fill="lime"><rect x="8" y="8" width="48" height="48"
      style="fill: url(#g) red icc-color(profile, 0.2, 0.4, 0.1)"/></g>)svg");
  const auto expected =
      RenderReferenceContract(R"svg(<rect x="8" y="8" width="48" height="48" fill="lime"/>)svg");
  ExpectVisibleBitmap(actual, "obsolete_icc_inherited_visible");
  ExpectBitmapsIdentical(actual, expected, "obsolete_icc_declaration_rejected");
}

// The font presentation shorthand is a supported Donner extension.
TEST_F(RendererRegressionTests, ReferenceContractUnitlessFontShorthandMatchesLonghands) {
  SVGDocument shorthand = instantiateSubtree(R"svg(
    <text x="4" y="38" font="bold italic 28 Noto Sans">Ab</text>)svg",
                                             {}, Vector2i(64, 64));
  SVGDocument expanded = instantiateSubtree(R"svg(
    <text x="4" y="38" font-style="italic" font-weight="bold" font-size="28"
          font-family="Noto Sans">Ab</text>)svg",
                                            {}, Vector2i(64, 64));
  RegisterFontsFromDirectoryForTesting(shorthand, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(expanded, ResvgResourceRoot() / "fonts");
  const auto actual = RenderDocumentWithBackend(shorthand, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "unitless_font_shorthand_visible");
  ExpectBitmapsIdentical(actual, RenderDocumentWithBackend(expanded, ActiveRendererBackend()),
                         "unitless_font_shorthand_longhands");
}

TEST_F(RendererRegressionTests, ReferenceContractUnsizedRootUsesCallerCanvasWithoutContentFit) {
  SVGDocument document = ParseSvg(R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <rect x="8" y="8" width="16" height="16" fill="lime"/></svg>)svg");
  EXPECT_EQ(document.canvasSize(), Vector2i(512, 512));
  document.setCanvasSize(64, 64);
  const auto actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
  const auto expected =
      RenderReferenceContract(R"svg(<rect x="8" y="8" width="16" height="16" fill="lime"/>)svg");
  ASSERT_EQ(actual.dimensions, Vector2i(64, 64));
  ExpectVisibleBitmap(actual, "unsized_root_visible");
  ExpectBitmapsIdentical(actual, expected, "unsized_root_caller_canvas");
  EXPECT_THAT(PixelAt(actual, 12, 12), testing::ElementsAre(0, 255, 0, 255));
  EXPECT_EQ(PixelAt(actual, 32, 32)[3], 0);
}

TEST_F(RendererRegressionTests, ReferenceContractArcsUsesDocumentedMiterFallback) {
  const auto render = [](std::string_view join) {
    return RenderReferenceContract(std::string(R"svg(<path d="M12 52 Q12 8 48 12 Q24 32 36 52"
      fill="none" stroke="green" stroke-width="5" stroke-linejoin=")svg") +
                                   std::string(join) + R"svg("/>)svg");
  };
  const auto actual = render("arcs");
  ExpectVisibleBitmap(actual, "arcs_fallback_visible");
  ExpectBitmapsIdentical(actual, render("miter"), "arcs_documented_miter_fallback");
}

RendererBitmap RenderCssFilterContract(std::string_view filter, bool style = false) {
  const std::string attribute = style ? "style=\"filter: " : "filter=\"";
  SVGDocument document = instantiateSubtree(
      std::string(R"svg(<rect x="12" y="12" width="20" height="20" fill="seagreen"
                           color="blue" font-size="16" )svg") +
          attribute + std::string(filter) + R"svg("/>)svg",
      {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

TEST_F(RendererRegressionTests, CssFilterContractInvalidShadowStyleRejectsWholeChain) {
  const auto expected = RenderCssFilterContract("none");
  ExpectVisibleBitmap(expected, "invalid_shadow_plain_visible");
  for (std::string_view filter :
       {"invert(1) drop-shadow(4 5)", "invert(1) drop-shadow(4px 5px -2px)"}) {
    SCOPED_TRACE(filter);
    ExpectBitmapsIdentical(RenderCssFilterContract(filter, true), expected,
                           "invalid_shadow_style_ignored");
  }
  ExpectBitmapsIdentical(RenderCssFilterContract("drop-shadow(4 5 -2)"), expected,
                         "negative_shadow_presentation_ignored");
}

TEST_F(RendererRegressionTests, CssFilterContractShadowUnitsColorsAndDefaultMatch) {
  const auto expected = RenderCssFilterContract("drop-shadow(blue 12px 6px 3px)", true);
  ExpectBitmapsDiffer(expected, RenderCssFilterContract("none"), "shadow_visible_control");
  for (std::string_view filter :
       {"drop-shadow(blue 12 6 3)", "drop-shadow(12px 6px 3px blue)", "drop-shadow(12px 6px 3px)",
        "drop-shadow(blue 0.75em 0.375em 0.1875em)",
        "drop-shadow(blue 3.175mm 1.5875mm 0.79375mm)"}) {
    SCOPED_TRACE(filter);
    ExpectBitmapsIdentical(RenderCssFilterContract(filter), expected, "shadow_units_colors");
  }
}

TEST_F(RendererRegressionTests, CssFilterContractEmShadowUsesComputedNondefaultFontSize) {
  SVGDocument document = instantiateSubtree(R"svg(
    <rect x="12" y="12" width="20" height="20" fill="seagreen" font-size="64"
          filter="drop-shadow(blue 0.1875em 0.09375em 0.046875em)"/>)svg",
                                            {}, Vector2i(64, 64));
  ExpectBitmapsIdentical(RenderDocumentWithBackend(document, ActiveRendererBackend()),
                         RenderCssFilterContract("drop-shadow(blue 12px 6px 3px)"),
                         "shadow_nondefault_font_size_units");
}

TEST_F(RendererRegressionTests, CssFilterContractShadowColorAlphaIsAppliedOnce) {
  const auto actual = RenderCssFilterContract("drop-shadow(rgba(0,0,255,0.5) 28px 0 0)", true);
  SVGDocument expectedDocument = instantiateSubtree(R"svg(
    <rect x="40" y="12" width="20" height="20" fill="rgba(0,0,255,0.5)"/>
    <rect x="12" y="12" width="20" height="20" fill="seagreen"/>)svg",
                                                    {}, Vector2i(64, 64));
  const auto expected = RenderDocumentWithBackend(expectedDocument, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "translucent_shadow_visible");
  ExpectBitmapsIdentical(actual, expected, "shadow_color_alpha_once");
}

TEST_F(RendererRegressionTests, CssFilterContractColorAmountsMatchIndependentEndpoints) {
  for (const auto& [function, color] : std::array<std::pair<std::string_view, std::string_view>, 7>{
           {{"brightness", "black"},
            {"contrast", "rgb(128 128 128)"},
            {"grayscale", "seagreen"},
            {"invert", "seagreen"},
            {"opacity", "none"},
            {"sepia", "seagreen"},
            {"saturate", "rgb(115 115 115)"}}}) {
    SCOPED_TRACE(function);
    SVGDocument expectedDocument = instantiateSubtree(
        std::string(R"svg(<rect x="12" y="12" width="20" height="20" fill=")svg") +
            std::string(color) + R"svg("/>)svg",
        {}, Vector2i(64, 64));
    ExpectBitmapsIdentical(RenderCssFilterContract(std::string(function) + "(0)"),
                           RenderDocumentWithBackend(expectedDocument, ActiveRendererBackend()),
                           "filter_zero_endpoint_" + std::string(function));
  }
}

TEST_F(RendererRegressionTests, CssFilterContractColorAmountSyntaxDefaultsAndInvalidValues) {
  const auto plain = RenderCssFilterContract("none");
  for (std::string_view function :
       {"brightness", "contrast", "grayscale", "invert", "opacity", "saturate", "sepia"}) {
    SCOPED_TRACE(function);
    const auto render = [function](std::string_view value) {
      return RenderCssFilterContract(std::string(function) + "(" + std::string(value) + ")");
    };
    ExpectBitmapsIdentical(render(""), render("1"), "filter_default_" + std::string(function));
    ExpectBitmapsIdentical(render("50%"), render("0.5"), "filter_percent_" + std::string(function));
    ExpectBitmapsIdentical(render("200%"), render("2"), "filter_two_" + std::string(function));
    ExpectBitmapsIdentical(render("-1"), plain, "filter_negative_" + std::string(function));
  }
  for (std::string_view function : {"grayscale", "invert", "opacity", "sepia"}) {
    ExpectBitmapsIdentical(RenderCssFilterContract(std::string(function) + "(2)"),
                           RenderCssFilterContract(std::string(function) + "(1)"),
                           "filter_clamp_" + std::string(function));
  }
}

TEST_F(RendererRegressionTests, CssFilterContractShadowHalfPixelOffsetsUseRoundedPixels) {
  const auto actual = RenderCssFilterContract("drop-shadow(blue 12.5px 6.5px 0)", true);
  SVGDocument expectedDocument = instantiateSubtree(R"svg(
    <rect x="25" y="19" width="20" height="20" fill="blue"/>
    <rect x="12" y="12" width="20" height="20" fill="seagreen"/>)svg",
                                                    {}, Vector2i(64, 64));
  ExpectBitmapsIdentical(actual,
                         RenderDocumentWithBackend(expectedDocument, ActiveRendererBackend()),
                         "shadow_half_pixel_rounding_contract");
}

TEST_F(RendererRegressionTests, CssFilterContractUrlChainsMatchEquivalentPrimitiveGraphs) {
  const auto render = [](std::string_view definitions, std::string_view filter) {
    SVGDocument document = instantiateSubtree(
        std::string(definitions) +
            R"svg(<rect x="12" y="12" width="20" height="20" fill="seagreen" filter=")svg" +
            std::string(filter) + R"svg("/>)svg",
        {}, Vector2i(64, 64));
    return RenderDocumentWithBackend(document, ActiveRendererBackend());
  };
  const std::string_view definitions = R"svg(
    <filter id="flood"><feFlood flood-color="blue"/></filter>
    <filter id="blur"><feGaussianBlur stdDeviation="1"/></filter>
    <filter id="combined"><feFlood flood-color="blue"/><feGaussianBlur stdDeviation="1"/></filter>
    <filter id="step"><feOffset dx="1"/></filter>
    <filter id="twice"><feOffset dx="2"/></filter>
  )svg";
  const auto expected = render(definitions, "url(#combined)");
  ExpectVisibleBitmap(expected, "url_chain_combined_visible");
  ExpectBitmapsIdentical(render(definitions, "url(#flood) url(#blur)"), expected,
                         "url_chain_implicit_previous_input");
  ExpectBitmapsIdentical(render(definitions, "url(#step) url(#step)"),
                         render(definitions, "url(#twice)"),
                         "repeated_url_implicit_previous_input");
}

// The flood fills a region 4px inside the canvas, nearer than the blur's box passes spread, so the
// canvas-edge render must equal the same filter rendered with room around it.
TEST_F(RendererRegressionTests, FilterBlurNearCanvasEdgeMatchesBlurWithRoom) {
  constexpr std::string_view kFilteredRect = R"svg(
    <filter id="f" filterUnits="userSpaceOnUse" x="4" y="4" width="56" height="56">
      <feFlood flood-color="seagreen"/><feGaussianBlur stdDeviation="6"/>
    </filter>
    <rect x="12" y="12" width="40" height="40" fill="red" filter="url(#f)"/>)svg";
  SVGDocument atEdge = instantiateSubtree(kFilteredRect, {}, Vector2i(64, 64));
  SVGDocument withRoom =
      instantiateSubtree(std::string(R"svg(<g transform="translate(32 32)">)svg") +
                             std::string(kFilteredRect) + "</g>",
                         {}, Vector2i(128, 128));
  const RendererBitmap actual = RenderDocumentWithBackend(atEdge, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "blur_canvas_edge_visible");
  editor::tests::CompareBitmapToBitmap(
      actual,
      editor::tests::CropBitmap(RenderDocumentWithBackend(withRoom, ActiveRendererBackend()),
                                Vector2i(32, 32), Vector2i(64, 64)),
      "blur_canvas_edge_spill", editor::tests::PixelmatchIdentityParams());
}

// The derived filter inherits the blur, y and height through href, and its own x cuts through the
// rect's left side while the inherited height cuts its bottom. The region clips the blur's input
// and result, so inside it the render must equal a blur of the rect's visible part rendered with
// room around it, and outside it nothing is drawn.
TEST_F(RendererRegressionTests, FilterHrefRegionClipMatchesClippedSourceBlurredWithRoom) {
  SVGDocument clippedByRegion = instantiateSubtree(R"svg(
    <filter id="base" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="40">
      <feGaussianBlur stdDeviation="3"/>
    </filter>
    <filter id="derived" x="20" width="44" href="#base"/>
    <filter id="unused" y="10" href="#derived"/>
    <rect x="4" y="16" width="48" height="32" fill="seagreen" filter="url(#derived)"/>)svg",
                                                   {}, Vector2i(64, 64));
  SVGDocument withRoom = instantiateSubtree(R"svg(
    <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64">
      <feGaussianBlur stdDeviation="3"/>
    </filter>
    <rect x="20" y="16" width="32" height="24" fill="seagreen" filter="url(#f)"/>)svg",
                                            {}, Vector2i(64, 64));
  const RendererBitmap actual = RenderDocumentWithBackend(clippedByRegion, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "href_region_clip_visible");

  const auto expectTransparent = [&actual](Vector2i origin, Vector2i size, std::string_view label) {
    const RendererBitmap outside = editor::tests::CropBitmap(actual, origin, size);
    RendererBitmap transparent = outside;
    std::fill(transparent.pixels.begin(), transparent.pixels.end(), 0);
    editor::tests::CompareBitmapToBitmap(outside, transparent, label,
                                         editor::tests::PixelmatchIdentityParams());
  };
  expectTransparent(Vector2i(0, 0), Vector2i(20, 64), "href_region_clip_outside_left");
  expectTransparent(Vector2i(20, 40), Vector2i(44, 24), "href_region_clip_outside_bottom");
  editor::tests::CompareBitmapToBitmap(
      editor::tests::CropBitmap(actual, Vector2i(20, 0), Vector2i(44, 40)),
      editor::tests::CropBitmap(RenderDocumentWithBackend(withRoom, ActiveRendererBackend()),
                                Vector2i(20, 0), Vector2i(44, 40)),
      "href_region_clip_inside", editor::tests::PixelmatchIdentityParams());
}

/// A drop shadow whose offset carries its source toward or past the edge of the filter raster.
/// Shapes are pixel-aligned rectangles, which both backends rasterize to identical coverage.
struct EdgeDropShadowCase {
  std::string_view name;   //!< Label for the comparison outputs.
  std::string_view shape;  //!< Filtered shape markup, open for the `fill` and `filter` attributes.
  int dx;                  //!< Shadow offset along x, in user units.
  int dy;                  //!< Shadow offset along y, in user units.
};

constexpr std::array<EdgeDropShadowCase, 6> kEdgeDropShadowCases = {{
    {"past_right_edge", R"svg(<rect x="44" y="20" width="16" height="24")svg", 14, 0},
    {"past_left_edge", R"svg(<rect x="4" y="20" width="16" height="24")svg", -14, 0},
    {"past_bottom_edge", R"svg(<rect x="20" y="44" width="24" height="16")svg", 0, 14},
    {"past_top_edge", R"svg(<rect x="20" y="4" width="24" height="16")svg", 0, -14},
    // Blur spill beyond the left edge is carried back into view by the positive offset.
    {"into_view_from_left_edge", R"svg(<rect x="1" y="26" width="8" height="12")svg", 14, 0},
    {"past_top_right_corner", R"svg(<rect x="48" y="2" width="14" height="14")svg", 12, -12},
}};

/// Renders the case's shape filtered by \p primitives over a filter region that is exactly the
/// 64x64 canvas, so the filter raster edge is the canvas edge.
RendererBitmap RenderEdgeDropShadow(const EdgeDropShadowCase& shadowCase,
                                    std::string_view primitives, RendererBackend backend) {
  SVGDocument document = instantiateSubtree(
      std::string(R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="64"
                               height="64">)svg") +
          std::string(primitives) + "</filter>" + std::string(shadowCase.shape) +
          R"svg( fill="seagreen" filter="url(#f)"/>)svg",
      {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, backend);
}

std::string EdgeDropShadowPrimitive(const EdgeDropShadowCase& shadowCase) {
  return "<feDropShadow dx=\"" + std::to_string(shadowCase.dx) + "\" dy=\"" +
         std::to_string(shadowCase.dy) +
         R"svg(" stdDeviation="3" flood-color="blue" flood-opacity="0.75"/>)svg";
}

/// The Filter Effects definition of feDropShadow as an explicit primitive chain: blur the alpha,
/// offset it, color it with the flood, and merge the source over it.
std::string EdgeDropShadowChain(const EdgeDropShadowCase& shadowCase) {
  return R"svg(<feGaussianBlur in="SourceAlpha" stdDeviation="3"/><feOffset dx=")svg" +
         std::to_string(shadowCase.dx) + "\" dy=\"" + std::to_string(shadowCase.dy) +
         R"svg(" result="offsetBlur"/><feFlood flood-color="blue" flood-opacity="0.75"/>
           <feComposite in2="offsetBlur" operator="in"/>
           <feMerge><feMergeNode/><feMergeNode in="SourceGraphic"/></feMerge>)svg";
}

TEST_F(RendererRegressionTests, DropShadowNearRasterEdgeMatchesBlurThenOffsetChain) {
  for (const EdgeDropShadowCase& shadowCase : kEdgeDropShadowCases) {
    SCOPED_TRACE(shadowCase.name);
    const RendererBitmap expected =
        RenderEdgeDropShadow(shadowCase, EdgeDropShadowChain(shadowCase), ActiveRendererBackend());
    AssertVisibleFixture(expected, "edge_drop_shadow_chain_" + std::string(shadowCase.name));
    editor::tests::CompareBitmapToBitmap(
        RenderEdgeDropShadow(shadowCase, EdgeDropShadowPrimitive(shadowCase),
                             ActiveRendererBackend()),
        expected, "edge_drop_shadow_" + std::string(shadowCase.name),
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(RendererRegressionTests, DropShadowNearRasterEdgeMatchesAcrossBackends) {
  if (!IsRendererBackendAvailable(RendererBackend::Geode)) {
    GTEST_SKIP() << "Requires the Geode backend";
  }
  for (const EdgeDropShadowCase& shadowCase : kEdgeDropShadowCases) {
    SCOPED_TRACE(shadowCase.name);
    const std::string primitive = EdgeDropShadowPrimitive(shadowCase);
    editor::tests::CompareBitmapToBitmap(
        RenderEdgeDropShadow(shadowCase, primitive, RendererBackend::TinySkia),
        RenderEdgeDropShadow(shadowCase, primitive, RendererBackend::Geode),
        "edge_drop_shadow_cpu_vs_geode_" + std::string(shadowCase.name),
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(RendererRegressionTests, CssFilterContractMixedUrlGrayscaleUsesSrgb) {
  SVGDocument actualDocument = instantiateSubtree(R"svg(
    <filter id="identity"><feOffset/></filter>
    <rect x="12" y="12" width="20" height="20" fill="seagreen"
          filter="url(#identity) grayscale()"/>)svg",
                                                  {}, Vector2i(64, 64));
  // CSS grayscale uses encoded sRGB: .2126*46 + .7152*139 + .0722*87 = 115.4738.
  SVGDocument expectedDocument = instantiateSubtree(R"svg(
    <rect x="12" y="12" width="20" height="20" fill="rgb(115 115 115)"/>)svg",
                                                    {}, Vector2i(64, 64));
  const auto actual = RenderDocumentWithBackend(actualDocument, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "mixed_url_grayscale_visible");
  ExpectBitmapsIdentical(actual,
                         RenderDocumentWithBackend(expectedDocument, ActiveRendererBackend()),
                         "mixed_url_grayscale_srgb");
}

// Filter primitive limit: a filter graph holds at most 32 primitives. A filter over the limit is a
// valid filter whose preparation fails closed, so the element it applies to is not rendered, while
// independent content still renders. It is never truncated to its first 32 primitives.

/// Filter `id` covering the whole 84x16 canvas, holding \p offsets one-pixel feOffset primitives
/// and then \p blurs feGaussianBlur primitives.
std::string PrimitiveLimitFilter(std::string_view id, std::size_t offsets, std::size_t blurs = 0) {
  std::string markup = std::string(R"svg(<filter id=")svg") + std::string(id) +
                       R"svg(" filterUnits="userSpaceOnUse" x="0" y="0" width="84" height="16"
                       color-interpolation-filters="sRGB">)svg";
  for (std::size_t i = 0; i < offsets; ++i) {
    markup += R"svg(<feOffset dx="1"/>)svg";
  }
  for (std::size_t i = 0; i < blurs; ++i) {
    markup += R"svg(<feGaussianBlur stdDeviation="1"/>)svg";
  }
  return markup + "</filter>";
}

/// Renders \p definitions, then a seagreen square at x=4 carrying \p filter, then an unfiltered
/// lime control square at x=72.
RendererBitmap RenderPrimitiveLimitCase(std::string_view definitions, std::string_view filter) {
  SVGDocument document = instantiateSubtree(
      std::string(definitions) +
          R"svg(<rect x="4" y="4" width="8" height="8" fill="seagreen" filter=")svg" +
          std::string(filter) + R"svg("/>
          <rect x="72" y="4" width="8" height="8" fill="lime"/>)svg",
      {}, Vector2i(84, 16));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

/// The unfiltered oracle: a square filled with \p fill at each of \p squareXs, and the lime control
/// square.
RendererBitmap RenderPrimitiveLimitOracle(std::initializer_list<std::string_view> squareXs,
                                          std::string_view fill = "seagreen") {
  std::string body;
  for (std::string_view squareX : squareXs) {
    body += std::string(R"svg(<rect x=")svg") + std::string(squareX) +
            R"svg(" y="4" width="8" height="8" fill=")svg" + std::string(fill) + R"svg("/>)svg";
  }
  SVGDocument document =
      instantiateSubtree(body + R"svg(<rect x="72" y="4" width="8" height="8" fill="lime"/>)svg",
                         {}, Vector2i(84, 16));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

TEST_F(RendererRegressionTests, FilterPrimitiveLimitAppliesEveryPrimitiveAtTheLimit) {
  const RendererBitmap shifted = RenderPrimitiveLimitOracle({"36"});
  AssertVisibleFixture(shifted, "primitive_limit_shifted_oracle");
  editor::tests::CompareBitmapToBitmap(
      RenderPrimitiveLimitCase(PrimitiveLimitFilter("f", 32), "url(#f)"), shifted,
      "primitive_limit_32_offsets", editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(
      RenderPrimitiveLimitCase(PrimitiveLimitFilter("a", 16) + PrimitiveLimitFilter("b", 16),
                               "url(#a) url(#b)"),
      shifted, "primitive_limit_32_offsets_url_chain", editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, FilterPrimitiveLimitRefusesElementOverTheLimit) {
  const RendererBitmap refused = RenderPrimitiveLimitOracle({});
  AssertVisibleFixture(refused, "primitive_limit_refused_oracle");
  struct Case {
    std::string_view name;
    std::string definitions;
    std::string_view filter;
  };
  const std::array<Case, 4> cases = {{
      {"offsets_32_blur_1", PrimitiveLimitFilter("f", 32, 1), "url(#f)"},
      {"offsets_33", PrimitiveLimitFilter("f", 33), "url(#f)"},
      {"offsets_4096", PrimitiveLimitFilter("f", 4096), "url(#f)"},
      {"url_chain_16_17", PrimitiveLimitFilter("a", 16) + PrimitiveLimitFilter("b", 17),
       "url(#a) url(#b)"},
  }};
  for (const Case& item : cases) {
    SCOPED_TRACE(item.name);
    editor::tests::CompareBitmapToBitmap(RenderPrimitiveLimitCase(item.definitions, item.filter),
                                         refused, "primitive_limit_" + std::string(item.name),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// \p count `opacity(1)` filter functions.
std::string IdentityFilterFunctions(std::size_t count) {
  std::string functions;
  for (std::size_t i = 0; i < count; ++i) {
    functions += i == 0 ? "opacity(1)" : " opacity(1)";
  }
  return functions;
}

TEST_F(RendererRegressionTests, FilterPrimitiveLimitAppliesToCssFilterFunctions) {
  const RendererBitmap unshifted = RenderPrimitiveLimitOracle({"4"});
  AssertVisibleFixture(unshifted, "primitive_limit_unshifted_oracle");
  editor::tests::CompareBitmapToBitmap(RenderPrimitiveLimitCase("", IdentityFilterFunctions(32)),
                                       unshifted, "primitive_limit_32_functions",
                                       editor::tests::PixelmatchIdentityParams());
  const RendererBitmap refused = RenderPrimitiveLimitOracle({});
  for (const std::size_t count : {std::size_t{33}, std::size_t{1024}}) {
    SCOPED_TRACE(count);
    editor::tests::CompareBitmapToBitmap(
        RenderPrimitiveLimitCase("", IdentityFilterFunctions(count)), refused,
        "primitive_limit_" + std::to_string(count) + "_functions",
        editor::tests::PixelmatchIdentityParams());
  }
}

/// Renders a host whose filter is an feImage of the seagreen square, which itself carries
/// `url(#inner)` defined by \p innerFilter, beside the lime control square.
RendererBitmap RenderPrimitiveLimitFeImageCase(const std::string& innerFilter) {
  SVGDocument document = instantiateSubtree(
      R"svg(<defs>
        <filter id="image" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="16"
                color-interpolation-filters="sRGB">
          <feImage href="#source"/>
        </filter>)svg" +
          innerFilter + R"svg(
        <rect id="source" x="4" y="4" width="8" height="8" fill="seagreen" filter="url(#inner)"/>
      </defs>
      <rect width="64" height="16" fill="red" filter="url(#image)"/>
      <rect x="72" y="4" width="8" height="8" fill="lime"/>)svg",
      {}, Vector2i(84, 16));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

// The at-limit oracle is the same feImage of the square under one 32-pixel feOffset, so the
// comparison isolates the primitive count from how feImage resamples filtered content.
TEST_F(RendererRegressionTests, FilterPrimitiveLimitAppliesInsideFeImageContent) {
  const RendererBitmap shifted = RenderPrimitiveLimitFeImageCase(
      R"svg(<filter id="inner" filterUnits="userSpaceOnUse" x="0" y="0" width="84" height="16"
          color-interpolation-filters="sRGB"><feOffset dx="32"/></filter>)svg");
  AssertVisibleFixture(shifted, "primitive_limit_fe_image_shifted_oracle");
  editor::tests::CompareBitmapToBitmap(
      RenderPrimitiveLimitFeImageCase(PrimitiveLimitFilter("inner", 32)), shifted,
      "primitive_limit_fe_image_32_offsets", editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(
      RenderPrimitiveLimitFeImageCase(PrimitiveLimitFilter("inner", 4096)),
      RenderPrimitiveLimitOracle({}), "primitive_limit_fe_image_4096_offsets",
      editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(
      RenderPrimitiveLimitFeImageCase(PrimitiveLimitFilter("inner", 32, 1)),
      RenderPrimitiveLimitOracle({}), "primitive_limit_fe_image_33_primitives",
      editor::tests::PixelmatchIdentityParams());
}

// Appending a 33rd primitive to a rendered filter refuses its element; removing it renders again.
TEST_F(RendererRegressionTests, FilterPrimitiveLimitFollowsDomMutation) {
  SVGDocument document = instantiateSubtree(
      PrimitiveLimitFilter("f", 32) +
          R"svg(<rect x="4" y="4" width="8" height="8" fill="seagreen" filter="url(#f)"/>
          <rect x="72" y="4" width="8" height="8" fill="lime"/>)svg",
      {}, Vector2i(84, 16));
  const RendererBitmap shifted = RenderPrimitiveLimitOracle({"36"});
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithBackend(document, ActiveRendererBackend()),
                                       shifted, "primitive_limit_mutation_before",
                                       editor::tests::PixelmatchIdentityParams());

  auto filter = document.querySelector("#f");
  ASSERT_TRUE(filter.has_value());
  SVGFEOffsetElement extra = SVGFEOffsetElement::Create(document);
  filter->appendChild(extra);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithBackend(document, ActiveRendererBackend()), RenderPrimitiveLimitOracle({}),
      "primitive_limit_mutation_appended", editor::tests::PixelmatchIdentityParams());

  filter->removeChild(extra);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithBackend(document, ActiveRendererBackend()),
                                       shifted, "primitive_limit_mutation_removed",
                                       editor::tests::PixelmatchIdentityParams());
}

// Per-primitive resource limits fail closed like the primitive limit: at the limit the primitive
// applies in full, and one over refuses the element instead of dropping or ignoring the excess.

/// Filter `f` covering the canvas: a 32-pixel feOffset with result `s`, then \p primitive.
std::string ShiftedPrimitiveFilter(std::string_view primitive) {
  return std::string(R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="84"
      height="16" color-interpolation-filters="sRGB"><feOffset dx="32" result="s"/>)svg") +
         std::string(primitive) + "</filter>";
}

/// Repeats \p item \p count times, separated by \p separator.
std::string RepeatJoined(std::string_view item, std::size_t count, std::string_view separator) {
  std::string result;
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      result += separator;
    }
    result += item;
  }
  return result;
}

TEST_F(RendererRegressionTests, FilterMergeInputLimit) {
  const auto merge = [](std::size_t shiftedInputs) {
    return ShiftedPrimitiveFilter(
        "<feMerge>" + RepeatJoined(R"svg(<feMergeNode in="s"/>)svg", shiftedInputs, "") +
        R"svg(<feMergeNode in="SourceGraphic"/></feMerge>)svg");
  };
  const RendererBitmap both = RenderPrimitiveLimitOracle({"4", "36"});
  AssertVisibleFixture(both, "merge_limit_both_oracle");
  editor::tests::CompareBitmapToBitmap(RenderPrimitiveLimitCase(merge(15), "url(#f)"), both,
                                       "merge_limit_16_inputs",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(RenderPrimitiveLimitCase(merge(16), "url(#f)"),
                                       RenderPrimitiveLimitOracle({}), "merge_limit_17_inputs",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, FilterTransferTableLimit) {
  const auto transfer = [](std::size_t values) {
    return ShiftedPrimitiveFilter(
        R"svg(<feComponentTransfer in="SourceGraphic"><feFuncR type="table" tableValues=")svg" +
        RepeatJoined("0", values, " ") + R"svg("/></feComponentTransfer>)svg");
  };
  const RendererBitmap noRed = RenderPrimitiveLimitOracle({"4"}, "rgb(0 139 87)");
  AssertVisibleFixture(noRed, "transfer_limit_no_red_oracle");
  editor::tests::CompareBitmapToBitmap(RenderPrimitiveLimitCase(transfer(1024), "url(#f)"), noRed,
                                       "transfer_limit_1024_values",
                                       editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(RenderPrimitiveLimitCase(transfer(1025), "url(#f)"),
                                       RenderPrimitiveLimitOracle({}), "transfer_limit_1025_values",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, FilterConvolveKernelLimit) {
  // An identity kernel. The kernel is applied rotated 180 degrees, so the one sits at
  // order - 1 - floor(order / 2) on each axis to land on the default target.
  const auto convolve = [](std::string_view order, int columns, std::size_t values) {
    const auto flipped = static_cast<std::size_t>(columns - 1 - columns / 2);
    const std::size_t target = flipped * static_cast<std::size_t>(columns) + flipped;
    std::string kernel;
    for (std::size_t i = 0; i < values; ++i) {
      kernel += i == 0 ? "" : " ";
      kernel += i == target ? "1" : "0";
    }
    return ShiftedPrimitiveFilter(R"svg(<feConvolveMatrix order=")svg" + std::string(order) +
                                  R"svg(" kernelMatrix=")svg" + kernel + R"svg("/>)svg");
  };
  // Geode executes kernels of at most 25 values and outputs transparent black for larger ones
  // (bug #2101), so only the CPU backend can show a full-size kernel applying.
  if (ActiveRendererBackend() != RendererBackend::Geode) {
    const RendererBitmap shifted = RenderPrimitiveLimitOracle({"36"});
    editor::tests::CompareBitmapToBitmap(
        RenderPrimitiveLimitCase(convolve("64", 64, 4096), "url(#f)"), shifted,
        "convolve_limit_order_64", editor::tests::PixelmatchIdentityParams());
  }
  const RendererBitmap refused = RenderPrimitiveLimitOracle({});
  editor::tests::CompareBitmapToBitmap(
      RenderPrimitiveLimitCase(convolve("65", 65, 4225), "url(#f)"), refused,
      "convolve_limit_order_65", editor::tests::PixelmatchIdentityParams());
  editor::tests::CompareBitmapToBitmap(
      RenderPrimitiveLimitCase(convolve("64", 64, 4097), "url(#f)"), refused,
      "convolve_limit_4097_values", editor::tests::PixelmatchIdentityParams());
}

// Removing an over-limit `order` restores the default 3x3 order, so the filter renders again.
TEST_F(RendererRegressionTests, FilterConvolveOrderRemovalClearsLimitRejection) {
  SVGDocument document = instantiateSubtree(
      ShiftedPrimitiveFilter(
          R"svg(<feConvolveMatrix id="k" order="65" kernelMatrix="0 0 0 0 1 0 0 0 0"/>)svg") +
          R"svg(<rect x="4" y="4" width="8" height="8" fill="seagreen" filter="url(#f)"/>
          <rect x="72" y="4" width="8" height="8" fill="lime"/>)svg",
      {}, Vector2i(84, 16));
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithBackend(document, ActiveRendererBackend()), RenderPrimitiveLimitOracle({}),
      "convolve_order_removal_before", editor::tests::PixelmatchIdentityParams());

  auto convolve = document.querySelector("#k");
  ASSERT_TRUE(convolve.has_value());
  convolve->removeAttribute(xml::XMLQualifiedNameRef("order"));
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithBackend(document, ActiveRendererBackend()),
                                       RenderPrimitiveLimitOracle({"36"}),
                                       "convolve_order_removal_after",
                                       editor::tests::PixelmatchIdentityParams());
}

// A valid animated order replaces an over-limit base order while it runs, and the base order is
// rejected again once the animation ends.
TEST_F(RendererRegressionTests, FilterConvolveAnimatedOrderOverridesLimitRejection) {
  SVGDocument document = instantiateSubtree(
      ShiftedPrimitiveFilter(R"svg(<feConvolveMatrix order="65" kernelMatrix="1">
            <set attributeName="order" to="1" dur="1s"/></feConvolveMatrix>)svg") +
          R"svg(<rect x="4" y="4" width="8" height="8" fill="seagreen" filter="url(#f)"/>
          <rect x="72" y="4" width="8" height="8" fill="lime"/>)svg",
      {}, Vector2i(84, 16));
  document.setTime(0.5);
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithBackend(document, ActiveRendererBackend()),
                                       RenderPrimitiveLimitOracle({"36"}),
                                       "convolve_animated_order_active",
                                       editor::tests::PixelmatchIdentityParams());
  document.setTime(2);
  editor::tests::CompareBitmapToBitmap(
      RenderDocumentWithBackend(document, ActiveRendererBackend()), RenderPrimitiveLimitOracle({}),
      "convolve_animated_order_ended", editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, UnitlessPresentationAttributeBlurUsesUserUnits) {
  const auto render = [](std::string_view filter) {
    const std::string markup =
        R"svg(<rect x="20" y="20" width="40" height="40" fill="seagreen" filter=")svg" +
        std::string(filter) + R"svg("/>)svg";
    SVGDocument document = instantiateSubtree(markup, {}, Vector2i(80, 80));
    return RenderDocumentWithBackend(document, ActiveRendererBackend());
  };

  const RendererBitmap unitless = render("blur(4)");
  const RendererBitmap px = render("blur(4px)");
  const RendererBitmap plain = render("none");

  ExpectVisibleBitmap(unitless, "unitless_blur_visible");
  ExpectBitmapsIdentical(unitless, px, "unitless_blur_matches_px");
  ExpectBitmapsDiffer(unitless, plain, "unitless_blur_changes_pixels");
  EXPECT_THAT(PixelAt(unitless, 16, 40)[3], testing::Gt(0));
  EXPECT_EQ(PixelAt(plain, 16, 40)[3], 0);
}

TEST_F(RendererRegressionTests, UnitlessStylesheetBlurIsIgnored) {
  SVGDocument styled = instantiateSubtree(
      R"svg(<style>rect { filter: blur(4); }</style>
             <rect x="20" y="20" width="40" height="40" fill="seagreen"/>)svg",
      {}, Vector2i(80, 80));
  SVGDocument plain =
      instantiateSubtree(R"svg(<rect x="20" y="20" width="40" height="40" fill="seagreen"/>)svg",
                         {}, Vector2i(80, 80));

  const RendererBitmap actual = RenderDocumentWithBackend(styled, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(plain, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "invalid_stylesheet_filter_visible");
  ExpectBitmapsIdentical(actual, expected, "invalid_stylesheet_filter_ignored");
  EXPECT_EQ(PixelAt(actual, 16, 40)[3], 0);
}

// An feImage that references an element renders that element in the user space of the filtered
// element, so the filtered element's transform also applies to the referenced content. The filter
// region starts at the user-space origin, so the referenced content is not offset by it. The red
// filtered content is fully replaced by the feImage result.
RendererBitmap RenderFeImageFragment(std::string_view hostTransform, std::string_view hostRect,
                                     std::string_view sourceRect) {
  const std::string markup =
      R"svg(<defs>
      <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="200" height="200">
        <feImage href="#src"/>
      </filter>
      <rect id="src" )svg" +
      std::string(sourceRect) + R"svg( fill="lime"/>
    </defs>
    <g transform=")svg" +
      std::string(hostTransform) + R"svg(" filter="url(#f)"><rect )svg" + std::string(hostRect) +
      R"svg( fill="red"/></g>)svg";
  SVGDocument document = instantiateSubtree(markup, {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

// The untransformed filtered element is the oracle: its referenced content already sits where the
// transformed case must place it, and both cases resample the fragment at whole-pixel offsets.
TEST_F(RendererRegressionTests, FeImageFragmentFollowsTranslatedFilteredElement) {
  const RendererBitmap expected =
      RenderFeImageFragment("translate(0 0)", R"svg(x="24" y="24" width="32" height="32")svg",
                            R"svg(x="32" y="32" width="16" height="16")svg");
  ExpectVisibleBitmap(expected, "fe_image_translated_expected_visible");
  EXPECT_THAT(PixelAt(expected, 40, 40), test::RgbaEq(0, 255, 0, 255));

  const RendererBitmap actual =
      RenderFeImageFragment("translate(24 24)", R"svg(width="32" height="32")svg",
                            R"svg(x="8" y="8" width="16" height="16")svg");
  editor::tests::CompareBitmapToBitmap(actual, expected, "fe_image_fragment_translated_host",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, FeImageFragmentOutsideCanvasBeforeHostTransformIsVisible) {
  // Before the host transform, both the filtered content and the referenced content lie entirely
  // beyond the 64x64 canvas; the host transform brings the referenced content onto the canvas.
  const RendererBitmap expected =
      RenderFeImageFragment("translate(0 0)", R"svg(width="48" height="48")svg",
                            R"svg(x="8" y="8" width="16" height="16")svg");
  ExpectVisibleBitmap(expected, "fe_image_offcanvas_expected_visible");
  EXPECT_THAT(PixelAt(expected, 16, 16), test::RgbaEq(0, 255, 0, 255));

  const RendererBitmap actual =
      RenderFeImageFragment("translate(-96 -96)", R"svg(x="96" y="96" width="48" height="48")svg",
                            R"svg(x="104" y="104" width="16" height="16")svg");
  editor::tests::CompareBitmapToBitmap(actual, expected,
                                       "fe_image_fragment_offcanvas_before_host_transform",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, FeImageFragmentFollowsScaledFilteredElement) {
  // translate(8 8) scale(2) maps the referenced 8x8 square to device pixels 8 through 24.
  const RendererBitmap actual =
      RenderFeImageFragment("translate(8 8) scale(2)", R"svg(width="16" height="16")svg",
                            R"svg(width="8" height="8")svg");
  ExpectVisibleBitmap(actual, "fe_image_scaled_visible");
  EXPECT_THAT(PixelAt(actual, 16, 16), test::RgbaEq(0, 255, 0, 255));
  EXPECT_THAT(PixelAt(actual, 12, 12), test::RgbaEq(0, 255, 0, 255));
  EXPECT_EQ(PixelAt(actual, 4, 4)[3], 0);
  EXPECT_EQ(PixelAt(actual, 40, 40)[3], 0);
}

RendererBitmap RenderFeImageCase(std::string_view markup, Vector2i canvasSize) {
  SVGDocument document = instantiateSubtree(markup, {}, canvasSize);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

/// Renders a complete document whose root sets its own size and viewBox.
RendererBitmap RenderFeImageDocument(std::string_view svg) {
  SVGDocument document = ParseSvg(svg);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

TEST_F(RendererRegressionTests, FeImageFragmentFollowsRotatedFilteredElement) {
  // rotate(180 32 32) maps the referenced square at user 8..24 to device pixels 40..56, through
  // the general (non-axis-aligned) fragment placement path.
  const RendererBitmap actual = RenderFeImageCase(
      R"svg(<defs>
        <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="200" height="200">
          <feImage href="#src"/>
        </filter>
        <rect id="src" x="8" y="8" width="16" height="16" fill="lime"/>
      </defs>
      <g transform="rotate(180 32 32)" filter="url(#f)">
        <rect width="64" height="64" fill="red"/>
      </g>)svg",
      Vector2i(64, 64));
  ExpectVisibleBitmap(actual, "fe_image_rotated_visible");
  EXPECT_THAT(PixelAt(actual, 48, 48), test::RgbaEq(0, 255, 0, 255));
  EXPECT_EQ(PixelAt(actual, 16, 16)[3], 0);
  EXPECT_EQ(PixelAt(actual, 32, 32)[3], 0);
}

TEST_F(RendererRegressionTests, FeImageFragmentFollowsTranslatedFilteredElementUnderViewBoxOrigin) {
  const auto render = [](std::string_view hostTransform, std::string_view sourceXY) {
    return RenderFeImageDocument(
        std::string(
            R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="10 10 64 64">
          <defs>
            <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="200" height="200">
              <feImage href="#src"/>
            </filter>
            <rect id="src" )svg") +
        std::string(sourceXY) + R"svg( width="16" height="16" fill="lime"/>
          </defs>
          <g transform=")svg" +
        std::string(hostTransform) +
        R"svg(" filter="url(#f)"><rect x="10" y="10" width="64" height="64" fill="red"/></g>
        </svg>)svg");
  };
  const RendererBitmap expected = render("translate(0 0)", R"svg(x="36" y="36")svg");
  ExpectVisibleBitmap(expected, "fe_image_viewbox_expected_visible");
  EXPECT_THAT(PixelAt(expected, 34, 34), test::RgbaEq(0, 255, 0, 255));
  editor::tests::CompareBitmapToBitmap(render("translate(8 8)", R"svg(x="28" y="28")svg"), expected,
                                       "fe_image_fragment_viewbox_origin",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, FeImageFragmentUsesAnimatedSubregionSize) {
  // The static width is 10, but the animation holds it at 64, so the referenced square at x=24
  // is inside the feImage subregion while the animation runs.
  SVGDocument document = instantiateSubtree(
      R"svg(<defs>
        <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64">
          <feImage id="i" href="#src" width="10"/>
        </filter>
        <rect id="src" x="24" y="24" width="16" height="16" fill="lime"/>
      </defs>
      <rect width="64" height="64" fill="red" filter="url(#f)"/>
      <animate href="#i" attributeName="width" from="64" to="64" dur="2s"/>)svg",
      {}, Vector2i(64, 64));
  document.setTime(1);
  const RendererBitmap actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "fe_image_animated_width_visible");
  EXPECT_THAT(PixelAt(actual, 32, 32), test::RgbaEq(0, 255, 0, 255));
}

// A fragment raster that would cover a large filter region must not exhaust the per-frame filter
// preparation budget: the feImage still renders, and so does a later filtered element.
TEST_F(RendererRegressionTests, FeImageFragmentWithLargeRegionKeepsFrameFilterBudget) {
  struct Case {
    std::string_view name;
    std::string_view filter;
    std::string_view source;
    std::string_view host;
    Vector2i canvas;
    Vector2i limePixel;
  };
  const std::array<Case, 2> cases = {{
      // Default -10%/120% region of a 900x900 element: 1080x1080 raster pixels.
      {"default_region", R"svg(<filter id="f"><feImage href="#src"/></filter>)svg",
       R"svg(x="400" y="400" width="100" height="100")svg", R"svg(width="900" height="900")svg",
       Vector2i(900, 900), Vector2i(360, 360)},
      // A 2000x2000 user-space region over a 200x200 canvas.
      {"user_space_region",
       R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="-900" y="-900" width="2000"
          height="2000"><feImage href="#src"/></filter>)svg",
       R"svg(x="950" y="950" width="40" height="40")svg", R"svg(width="200" height="200")svg",
       Vector2i(200, 200), Vector2i(70, 70)},
  }};
  for (const Case& item : cases) {
    SCOPED_TRACE(item.name);
    const RendererBitmap actual = RenderFeImageCase(
        std::string(R"svg(<defs>)svg") + std::string(item.filter) + R"svg(<rect id="src" )svg" +
            std::string(item.source) + R"svg( fill="lime"/>
            <filter id="g"><feFlood flood-color="blue"/></filter>
          </defs>
          <rect )svg" +
            std::string(item.host) + R"svg( fill="red" filter="url(#f)"/>
          <rect x="10" y="10" width="20" height="20" filter="url(#g)"/>)svg",
        item.canvas);
    EXPECT_THAT(PixelAt(actual, item.limePixel.x, item.limePixel.y), test::RgbaEq(0, 255, 0, 255))
        << "the feImage fragment must render";
    EXPECT_THAT(PixelAt(actual, 20, 20), test::RgbaEq(0, 0, 255, 255))
        << "a later filtered element must still render";
  }
}

// Several fragment rasters in one frame must not need more of the filter preparation budget than
// canvas-sized rasters would: each 1000x1000 subregion alone fits the budget, but two do not.
TEST_F(RendererRegressionTests, FeImageFragmentsWithLargeRegionsShareFrameFilterBudget) {
  const RendererBitmap actual = RenderFeImageCase(
      R"svg(<defs>
        <filter id="f1" filterUnits="userSpaceOnUse" x="-200" y="-200" width="1000" height="1000">
          <feImage href="#a"/>
        </filter>
        <filter id="f2" filterUnits="userSpaceOnUse" x="-200" y="-200" width="1000" height="1000">
          <feImage href="#b"/>
        </filter>
        <rect id="a" x="300" y="300" width="40" height="40" fill="lime"/>
        <rect id="b" x="500" y="500" width="40" height="40" fill="lime"/>
        <filter id="g"><feFlood flood-color="blue"/></filter>
      </defs>
      <g filter="url(#f1)"><rect width="600" height="300" fill="red"/></g>
      <g filter="url(#f2)"><rect y="300" width="600" height="300" fill="red"/></g>
      <rect x="560" y="10" width="20" height="20" filter="url(#g)"/>)svg",
      Vector2i(600, 600));
  EXPECT_THAT(PixelAt(actual, 120, 120), test::RgbaEq(0, 255, 0, 255))
      << "the first feImage fragment must render";
  EXPECT_THAT(PixelAt(actual, 320, 320), test::RgbaEq(0, 255, 0, 255))
      << "the second feImage fragment must render";
  EXPECT_THAT(PixelAt(actual, 570, 20), test::RgbaEq(0, 0, 255, 255))
      << "a later filtered element must still render";
}

#ifdef DONNER_TEXT_FULL
TEST_F(RendererRegressionTests, TinySkiaBitmapGlyphPreservesDecodedPremultipliedAlpha) {
  SVGDocument document = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="220" height="180">
      <text id="emoji" x="20" y="150" font-family="Noto Color Emoji" font-size="109">&#x1F600;</text>
    </svg>)svg");
  RegisterFontsFromDirectoryForTesting(document, ResvgResourceRoot() / "fonts");
  const RendererBitmap actual = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  ASSERT_THAT(actual.empty(), testing::IsFalse());

  auto access = document.writeAccess();
  const auto element = document.querySelector("#emoji");
  ASSERT_THAT(element, testing::Ne(std::nullopt));
  Registry& registry = document.registry();
  FontManager& fonts = registry.ctx().get<FontManager>();
  TextEngine& engine = registry.ctx().get<TextEngine>();
  const auto& geometry = engine.ensureComputedTextGeometryComponent(element->entityHandle());
  ASSERT_THAT(geometry.runs, testing::SizeIs(1));
  const TextRun& run = geometry.runs.front();
  EXPECT_THAT(run.font, testing::Eq(fonts.findFont("Noto Color Emoji")));
  ASSERT_THAT(engine.isBitmapOnly(run.font), testing::IsTrue());
  ASSERT_THAT(run.glyphs, testing::SizeIs(1));
  const TextGlyph& glyph = run.glyphs.front();
  EXPECT_THAT(glyph.glyphIndex, testing::Gt(0));
  EXPECT_THAT(glyph.xPosition, testing::DoubleEq(20));
  EXPECT_THAT(glyph.yPosition, testing::DoubleEq(150));
  const auto bitmap = engine.bitmapGlyph(run.font, glyph.glyphIndex,
                                         engine.scaleForPixelHeight(run.font, run.usedFontSizePx));
  ASSERT_THAT(bitmap, testing::Ne(std::nullopt));
  ASSERT_THAT(bitmap->scale, testing::DoubleEq(1));
  ASSERT_THAT(bitmap->width, testing::Gt(0));
  ASSERT_THAT(bitmap->height, testing::Gt(0));
  ASSERT_THAT(bitmap->rgbaPixels, testing::SizeIs(bitmap->width * bitmap->height * 4));
  int partialAlphaPixels = 0;
  for (std::size_t offset = 0; offset < bitmap->rgbaPixels.size(); offset += 4) {
    const uint8_t alpha = bitmap->rgbaPixels[offset + 3];
    if (alpha > 0 && alpha < 255 && bitmap->rgbaPixels[offset] > 0) {
      ++partialAlphaPixels;
    }
  }
  ASSERT_THAT(partialAlphaPixels, testing::Gt(0))
      << "Decoded glyph must exercise colored partial alpha";
  std::cout << "Decoded bitmap partial-alpha pixels: " << partialAlphaPixels << '\n';

  const int left = 20 + static_cast<int>(bitmap->bearingX);
  const int top = 150 - static_cast<int>(bitmap->bearingY);
  ASSERT_THAT(bitmap->bearingX, testing::DoubleEq(static_cast<int>(bitmap->bearingX)));
  ASSERT_THAT(bitmap->bearingY, testing::DoubleEq(static_cast<int>(bitmap->bearingY)));
  ASSERT_THAT(left, testing::Ge(0));
  ASSERT_THAT(top, testing::Ge(0));
  ASSERT_THAT(left + bitmap->width, testing::Le(220));
  ASSERT_THAT(top + bitmap->height, testing::Le(180));
  RendererBitmap expected;
  expected.dimensions = Vector2i(220, 180);
  expected.rowBytes = 220 * 4;
  expected.alphaType = AlphaType::Unpremultiplied;
  expected.pixels.resize(expected.rowBytes * 180, 0);
  // At the native strike scale and integer origin, decoded texels are the independent oracle.
  for (int row = 0; row < bitmap->height; ++row) {
    std::copy_n(bitmap->rgbaPixels.begin() + row * bitmap->width * 4, bitmap->width * 4,
                expected.pixels.begin() + (top + row) * expected.rowBytes + left * 4);
  }
  UnpremultiplyRgbaInPlace(expected.pixels);
  editor::tests::CompareBitmapToBitmap(actual, expected, "bitmap_glyph_native_alpha",
                                       editor::tests::PixelmatchIdentityParams());
}
#endif

#ifdef DONNER_TEXT_FULL
TEST_F(RendererRegressionTests, TinySkiaBitmapGlyphRotationMatchesGroupRotation) {
  for (const int angle : {90, -90, 30}) {
    SCOPED_TRACE(angle);
    const std::string text =
        R"svg(<text id="emoji" x="80" y="120" font-family="Noto Color Emoji" font-size="48")svg";
    const std::string outer =
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="260" height="260">
        <g transform="translate(15 9) scale(1.25 0.75)">)svg";
    SVGDocument actualDocument =
        ParseSvg(outer + text + R"svg( rotate=")svg" + std::to_string(angle) +
                 R"svg(">&#x1F600;</text></g></svg>)svg");
    SVGDocument expectedDocument =
        ParseSvg(outer + R"svg(<g transform="rotate()svg" + std::to_string(angle) +
                 R"svg( 80 120)">)svg" + text + R"svg(>&#x1F600;</text></g></g></svg>)svg");
    RegisterFontsFromDirectoryForTesting(actualDocument, ResvgResourceRoot() / "fonts");
    RegisterFontsFromDirectoryForTesting(expectedDocument, ResvgResourceRoot() / "fonts");
    const RendererBitmap actual =
        RenderDocumentWithBackend(actualDocument, RendererBackend::TinySkia);
    const RendererBitmap expected =
        RenderDocumentWithBackend(expectedDocument, RendererBackend::TinySkia);
    ASSERT_THAT(actual.pixels, testing::Contains(testing::Gt(0))) << "Actual glyph must be visible";
    ASSERT_THAT(expected.pixels, testing::Contains(testing::Gt(0)))
        << "Reference glyph must be visible";
    for (auto [document, rotation] :
         {std::pair(&actualDocument, angle), std::pair(&expectedDocument, 0)}) {
      auto access = document->writeAccess();
      const auto element = document->querySelector("#emoji");
      ASSERT_THAT(element, testing::Ne(std::nullopt));
      Registry& registry = document->registry();
      FontManager& fonts = registry.ctx().get<FontManager>();
      TextEngine& engine = registry.ctx().get<TextEngine>();
      const auto& geometry = engine.ensureComputedTextGeometryComponent(element->entityHandle());
      ASSERT_THAT(geometry.runs, testing::SizeIs(1));
      const TextRun& run = geometry.runs.front();
      EXPECT_THAT(run.font, testing::Eq(fonts.findFont("Noto Color Emoji")));
      ASSERT_THAT(engine.isBitmapOnly(run.font), testing::IsTrue());
      ASSERT_THAT(run.glyphs, testing::SizeIs(1));
      const TextGlyph& glyph = run.glyphs.front();
      EXPECT_THAT(glyph.glyphIndex, testing::Gt(0));
      EXPECT_THAT(glyph.xPosition, testing::DoubleEq(80));
      EXPECT_THAT(glyph.yPosition, testing::DoubleEq(120));
      EXPECT_THAT(glyph.rotateDegrees, testing::DoubleEq(rotation));
      const auto bitmap = engine.bitmapGlyph(
          run.font, glyph.glyphIndex, engine.scaleForPixelHeight(run.font, run.usedFontSizePx));
      ASSERT_THAT(bitmap, testing::Ne(std::nullopt));
      ASSERT_THAT(bitmap->width, testing::Gt(0));
      ASSERT_THAT(bitmap->height, testing::Gt(0));
      ASSERT_THAT(bitmap->rgbaPixels, testing::SizeIs(bitmap->width * bitmap->height * 4));
      EXPECT_THAT(bitmap->rgbaPixels, testing::Contains(testing::Gt(0)));
    }
    editor::tests::CompareBitmapToBitmap(actual, expected,
                                         "bitmap_glyph_rotation_" + std::to_string(angle),
                                         editor::tests::PixelmatchIdentityParams());
  }
}
#endif

TEST_F(RendererRegressionTests, MiterClipMatchesClippedJoinPolygon) {
  SVGDocument expectedDocument = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
      <path fill="green" d="M16 33 L30.25 14 L33.75 14 L48 33
                            L40 39 L32 28.333333333333333 L24 39 Z"/>
    </svg>)svg");
  // Geometry identity excludes edge AA; the corpus comparison retains normal antialiasing.
  const RendererBitmap expected =
      RenderDocumentWithBackendForAscii(expectedDocument, ActiveRendererBackend());
  for (const std::string_view path : {"M20 36 L32 20 L44 36", "M44 36 L32 20 L20 36"}) {
    SCOPED_TRACE(path);
    SVGDocument document =
        ParseSvg(std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
          <path fill="none" stroke="green" stroke-width="10" stroke-linejoin="miter-clip"
                stroke-miterlimit="1.2" d=")svg") +
                 std::string(path) + R"svg("/></svg>)svg");
    const RendererBitmap actual =
        RenderDocumentWithBackendForAscii(document, ActiveRendererBackend());
    editor::tests::CompareBitmapToBitmap(actual, expected, "miter_clip_analytic_join",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

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

// One span whose face lacks some clusters draws each of them in the first face that covers it,
// exactly as spans naming each face would. Mplus 1p has CJK and Latin but no Arabic; Amiri has
// the Arabic word. Each tspan boundary here sits between faces, where no kerning applies.
TEST_F(RendererRegressionTests, MixedScriptSpanMatchesSpansNamingEachFace) {
  constexpr std::string_view kCjk = "&#x65E5;&#x672C; ";
  constexpr std::string_view kArabic = "&#x62F;&#x627;&#x644;";
  for (const std::string_view writingMode : {"horizontal-tb", "vertical-rl"}) {
    SCOPED_TRACE(writingMode);
    const std::string text =
        std::string(R"svg(<text x="40" y="40" font-size="32" writing-mode=")svg") +
        std::string(writingMode) + R"svg(" font-family="'Mplus 1p'">)svg";
    const RendererBitmap actual =
        RenderTextOpacityCase(text + std::string(kCjk) + std::string(kArabic) + "</text>");
    const RendererBitmap expected = RenderTextOpacityCase(
        text + "<tspan>" + std::string(kCjk) + R"svg(</tspan><tspan font-family="Amiri">)svg" +
        std::string(kArabic) + "</tspan></text>");
    AssertVisibleFixture(expected, "mixed_script_explicit_faces_visible");
    editor::tests::CompareBitmapToBitmap(
        actual, expected, std::string("mixed_script_span_") + std::string(writingMode),
        editor::tests::PixelmatchIdentityParams());
  }
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

TEST_F(RendererRegressionTests, NestedTextRotationMatchesExplicitCharacterRotations) {
  const RendererBitmap inherited = RenderTextOpacityCase(
      R"svg(<text x="15" y="90" font-family="Noto Sans" font-size="24"
                  rotate="1 2 3 4 5 6 7">A<tspan rotate="10 20 30">B<tspan>C</tspan><tspan rotate="90">D</tspan><tspan>E</tspan></tspan><tspan>F</tspan><tspan>G</tspan><tspan>H</tspan></text>)svg");
  const RendererBitmap explicitRotations = RenderTextOpacityCase(
      R"svg(<text x="15" y="90" font-family="Noto Sans" font-size="24"
                  rotate="1">A<tspan rotate="10">B<tspan rotate="20">C</tspan><tspan rotate="90">D</tspan><tspan rotate="30">E</tspan></tspan><tspan rotate="6">F</tspan><tspan rotate="7">G</tspan><tspan rotate="7">H</tspan></text>)svg");

  ASSERT_THAT(inherited.empty(), testing::IsFalse());
  ASSERT_THAT(explicitRotations.empty(), testing::IsFalse());
  AssertVisibleFixture(explicitRotations, "explicit_character_rotations_visible");
  editor::tests::CompareBitmapToBitmap(inherited, explicitRotations, "nested_character_rotations",
                                       editor::tests::PixelmatchIdentityParams());
}

// `lengthAdjust="spacing"` accrues one per-gap adjustment after each character of the span, the
// last included. SVG 2 leaves the following text undefined; Chrome shifts it by the accrued amount,
// and the resvg references agree. A span fitted to its natural length plus two gaps therefore
// places every glyph, including the following text, where a `letter-spacing` of one gap does.
TEST_F(RendererRegressionTests, SpacingTextLengthOnSpanPlacesFollowingTextLikeLetterSpacing) {
  constexpr double kGap = 6.0;
  const auto spanMarkup = [](std::string_view spanAttributes) {
    return std::string(
               R"svg(<text x="10" y="90" font-family="Noto Sans" font-size="24"><tspan id="span" )svg") +
           std::string(spanAttributes) + R"svg(>nmn</tspan>uu</text>)svg";
  };

  SVGDocument fitted =
      instantiateSubtree(spanMarkup(""), parser::SVGParser::Options(), Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(fitted, ResvgResourceRoot() / "fonts");
  auto span = fitted.querySelector("#span")->cast<SVGTSpanElement>();
  span.setTextLength(Lengthd(span.getComputedTextLength() + 2.0 * kGap, Lengthd::Unit::None));
  const RendererBitmap actual = RenderDocumentWithBackend(fitted, ActiveRendererBackend());
  const RendererBitmap expected =
      RenderTextOpacityCase(spanMarkup("letter-spacing=\"" + std::to_string(kGap) + "\""));

  AssertVisibleFixture(expected, "letter_spaced_span_visible");
  editor::tests::CompareBitmapToBitmap(actual, expected, "spacing_text_length_following_text",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, UnicodePositioningAcrossSpansMatchesExplicitPositions) {
  struct Case {
    const char* name;
    const char* prefix;
    int prefixCodepoints;
  };
  const Case cases[] = {
      {"supplementary", "&#x1F600;", 1},
      {"BMP", "&#xE9;", 1},
      {"combining", "A&#x301;", 2},
      {"joiner", "&#x200D;", 1},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    const std::string start = R"svg(<text font-family="Noto Sans" font-size="24" )svg";
    const std::string inherited = start +
                                  R"svg(x="10 20 30 40" y="55 65 75 85" dx="1 2 3 4" dy="1 2 3 4"
                      rotate="0 10 20 30">)svg" +
                                  test.prefix + "<tspan>B</tspan>C</text>";
    const int bX = 11 * (test.prefixCodepoints + 1);
    const int bY = 56 + 11 * test.prefixCodepoints;
    const int bRotation = 10 * test.prefixCodepoints;
    const std::string prefixPositions = test.prefixCodepoints == 1
                                            ? R"svg(x="11" y="56" rotate="0")svg"
                                            : R"svg(x="11" y="56" rotate="0")svg";
    const std::string explicitPositions =
        start + "><tspan " + prefixPositions + ">" + test.prefix + "</tspan><tspan x='" +
        std::to_string(bX + (test.prefixCodepoints == 2 ? 2 : 0)) + "' y='" +
        std::to_string(bY + (test.prefixCodepoints == 2 ? 2 : 0)) + "' rotate='" +
        std::to_string(bRotation) + "'>B</tspan><tspan x='" + std::to_string(bX + 11) + "' y='" +
        std::to_string(bY + 11) + "' rotate='" + std::to_string(bRotation + 10) +
        "'>C</tspan></text>";
    const RendererBitmap actual = RenderTextOpacityCase(inherited);
    const RendererBitmap expected = RenderTextOpacityCase(explicitPositions);
    ASSERT_THAT(actual.empty(), testing::IsFalse());
    AssertVisibleFixture(expected, std::string("unicode_positions_visible_") + test.name);
    editor::tests::CompareBitmapToBitmap(actual, expected,
                                         std::string("unicode_positions_") + test.name,
                                         editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(RendererRegressionTests, TypographicGroupsSkipAbsoluteAndCarryRelativePositions) {
  const RendererBitmap actual = RenderTextOpacityCase(R"svg(
    <text font-family="Noto Sans" font-size="24" x="10 99 80" y="55 99 55"
          dx="1 2 3" dy="1 4 3" rotate="10 80 30">A&#x301;<tspan>B</tspan></text>
  )svg");
  const RendererBitmap expected = RenderTextOpacityCase(R"svg(
    <text font-family="Noto Sans" font-size="24"><tspan x="11" y="56" rotate="10">A&#x301;</tspan><tspan x="85" y="62" rotate="30">B</tspan></text>
  )svg");
  AssertVisibleFixture(expected, "typographic_groups_visible");
  editor::tests::CompareBitmapToBitmap(actual, expected, "typographic_groups_positions",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, JoinerDoesNotMergeSeparatelyShapedGlyphs) {
  const RendererBitmap actual = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24" x="12 99 80" y="65" rotate="0 70 20">A&#x200D;B</text>)svg");
  const RendererBitmap expected = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24"><tspan x="12" y="65" rotate="0">A&#x200D;</tspan><tspan x="80" y="65" rotate="20">B</tspan></text>)svg");
  AssertVisibleFixture(expected, "separate_joiner_glyphs_visible");
  editor::tests::CompareBitmapToBitmap(actual, expected, "separate_joiner_glyphs_positions",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, AbsolutePositionsBreakOptionalLigatures) {
  const RendererBitmap actual = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24" x="12 70" y="65">fi</text>)svg");
  const RendererBitmap expected = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24"><tspan x="12" y="65">f</tspan><tspan x="70" y="65">i</tspan></text>)svg");
  AssertVisibleFixture(expected, "optional_ligatures_visible");
  editor::tests::CompareBitmapToBitmap(actual, expected, "optional_ligatures_positions",
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_F(RendererRegressionTests, FinalInternalRotationRepeatsOnFollowingGroup) {
  const RendererBitmap actual = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24" x="12 99 80" y="65" rotate="10 70">A&#x301;B</text>)svg");
  const RendererBitmap expected = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24"><tspan x="12" y="65" rotate="10">A&#x301;</tspan><tspan x="80" y="65" rotate="70">B</tspan></text>)svg");
  AssertVisibleFixture(expected, "internal_rotation_visible");
  editor::tests::CompareBitmapToBitmap(actual, expected, "internal_rotation_positions",
                                       editor::tests::PixelmatchIdentityParams());
}

#ifdef DONNER_TEXT_FULL
TEST_F(RendererRegressionTests, JoinedOutlineGlyphsPreserveSourceIndicesAcrossSpans) {
  const RendererBitmap actual = RenderTextOpacityCase(R"svg(
    <text font-family="Noto Sans Devanagari" font-size="20" x="15 99 99 85" y="80 99 99 130"
          dx="1 2 3 4" dy="1 2 3 4" rotate="0 10 20 30">&#x915;&#x200D;&#x93F;<tspan>B</tspan></text>
  )svg");
  const RendererBitmap expected = RenderTextOpacityCase(R"svg(
    <text font-family="Noto Sans Devanagari" font-size="20"><tspan x="16" y="81" rotate="0">&#x915;&#x200D;&#x93F;</tspan><tspan x="94" y="139" rotate="30">B</tspan></text>
  )svg");
  AssertVisibleFixture(expected, "joined_outline_glyphs_visible");
  editor::tests::CompareBitmapToBitmap(actual, expected, "joined_outline_glyphs_positions",
                                       editor::tests::PixelmatchIdentityParams());
}
#endif

TEST_F(RendererRegressionTests, EmptyTextSpansDoNotConsumeRelativePositioning) {
  const RendererBitmap actual = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24" y="60" dx="5" dy="7"><tspan>A</tspan>B</text>)svg");
  const RendererBitmap expected = RenderTextOpacityCase(
      R"svg(<text font-family="Noto Sans" font-size="24" x="5" y="67">A<tspan>B</tspan></text>)svg");
  AssertVisibleFixture(expected, "empty_text_relative_positions_visible");
  editor::tests::CompareBitmapToBitmap(actual, expected, "empty_text_relative_positions",
                                       editor::tests::PixelmatchIdentityParams());
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

/// A 4x4 checkerboard of two opaque colors, as a PNG data URI. Upscaled twelvefold, every
/// destination pixel lands between source texel centers, so the sampling kernel decides each one.
constexpr std::string_view kCheckerImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAIAAAAmkwkpAAAAGklEQVR42mM4oRElt+oOhGSAs05oRDHglAEAXzIXcULo"
    "C6MAAAAASUVORK5CYII=";

/// Renders the checkerboard as an `<image>` at 48x48 on a 64x64 canvas with the given
/// `image-rendering` attribute, omitted when empty, with \p backend.
RendererBitmap RenderCheckerImage(std::string_view imageRendering,
                                  RendererBackend backend = ActiveRendererBackend()) {
  std::string body = R"svg(<image x="8" y="8" width="48" height="48")svg";
  if (!imageRendering.empty()) {
    body += " image-rendering=\"" + std::string(imageRendering) + "\"";
  }
  body += " href=\"" + std::string(kCheckerImageDataUri) + "\"/>";
  SVGDocument document = instantiateSubtree(body, parser::SVGParser::Options(), Vector2i(64, 64));
  return RenderDocumentWithBackend(document, backend);
}

/// Renders the same placement through `feImage`, with the given `image-rendering` attribute on the
/// `feImage`, omitted when empty. Its default quality sampling is the Mitchell-Netravali cubic
/// (B = C = 1/3) on both backends, which makes it the oracle for the `<image>` quality kernel.
RendererBitmap RenderCheckerFeImage(std::string_view imageRendering = "") {
  std::string body =
      R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="64" height="64">
              <feImage x="8" y="8" width="48" height="48")svg";
  if (!imageRendering.empty()) {
    body += " image-rendering=\"" + std::string(imageRendering) + "\"";
  }
  body += " href=\"" + std::string(kCheckerImageDataUri) + R"svg("/>
            </filter>
            <rect width="64" height="64" filter="url(#f)"/>)svg";
  SVGDocument document = instantiateSubtree(body, parser::SVGParser::Options(), Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

/// Comparison for an image-sampling oracle: identity on TinySkia, which shares tiny-skia's
/// sampling code, and the canonical default comparison on Geode. GPU sampling evaluates in float
/// on the device and lands one 8-bit step from the reference in places: hardware bilinear
/// filtering has reduced subtexel precision (684 pixels on Metal for the smooth case), and the
/// shader cubic differs by at most one step in one channel (28 and 612 pixels on an Intel Vulkan
/// device for the opaque and translucent cubic cases).
editor::tests::BitmapGoldenCompareParams ImageSamplingOracleParams() {
  return ActiveRendererBackend() == RendererBackend::Geode
             ? editor::tests::BitmapGoldenCompareParams{}
             : editor::tests::PixelmatchIdentityParams();
}

TEST_F(RendererRegressionTests, ImageQualityRenderingUsesMitchellCubicSampling) {
  const RendererBitmap mitchell = RenderCheckerFeImage();
  AssertVisibleFixture(mitchell, "image_mitchell_oracle_visible");
  for (std::string_view imageRendering : {"", "auto", "optimizeQuality", "high-quality"}) {
    SCOPED_TRACE(testing::Message() << "image-rendering=\"" << imageRendering << "\"");
    editor::tests::CompareBitmapToBitmap(
        RenderCheckerImage(imageRendering), mitchell,
        "image_quality_mitchell_" +
            std::string(imageRendering.empty() ? "unspecified" : imageRendering),
        ImageSamplingOracleParams());
  }
}

/// Straight RGBA pixels of a \p width x \p height image, row-major.
using StraightImage = std::vector<std::uint8_t>;

/// The 4x4 checkerboard decoded from \ref kCheckerImageDataUri.
StraightImage CheckerPixels() {
  StraightImage pixels;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const bool even = (x + y) % 2 == 0;
      pixels.insert(pixels.end(), {std::uint8_t(even ? 200 : 30), std::uint8_t(even ? 40 : 170),
                                   std::uint8_t(even ? 90 : 220), 255});
    }
  }
  return pixels;
}

/// A 4x4 checkerboard of opaque black and half-transparent white, as a PNG data URI. Cubic
/// overshoot next to the opaque black texels pushes premultiplied color above alpha.
constexpr std::string_view kTranslucentCheckerDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAG0lEQVR42mNgYGD4/////wY4jcJhYPjPQFAFAL5AI+Eh"
    "uRmVAAAAAElFTkSuQmCC";

/// The pixels decoded from \ref kTranslucentCheckerDataUri.
StraightImage TranslucentCheckerPixels() {
  StraightImage pixels;
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const bool even = (x + y) % 2 == 0;
      pixels.insert(pixels.end(), {std::uint8_t(even ? 0 : 255), std::uint8_t(even ? 0 : 255),
                                   std::uint8_t(even ? 0 : 255), std::uint8_t(even ? 255 : 128)});
    }
  }
  return pixels;
}

/// An 8x8 opaque image whose every texel differs from its neighbors, as a PNG data URI.
constexpr std::string_view kGradientImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAJ0lEQVR42mNkYGBQYPivwMCIRrIwKPxnYGBkYEAnWRgU"
    "sIgyDLAOAGXeId0ohAJHAAAAAElFTkSuQmCC";

/// The pixels decoded from \ref kGradientImageDataUri.
StraightImage GradientPixels() {
  StraightImage pixels;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      pixels.insert(pixels.end(), {std::uint8_t(x * 32), std::uint8_t(y * 32),
                                   std::uint8_t((x + y) % 2 == 0 ? 0 : 255), 255});
    }
  }
  return pixels;
}

/// Opaque gray canvas fill; under it, premultiplied color above alpha brightens the result.
constexpr std::uint8_t kReferenceGray = 128;

/// Renders `<image>` markup over an opaque gray 64x64 canvas with the active backend.
RendererBitmap RenderImageOverGray(std::string_view imageMarkup) {
  const std::string body =
      R"svg(<rect width="64" height="64" fill="rgb(128,128,128)"/>)svg" + std::string(imageMarkup);
  SVGDocument document = instantiateSubtree(body, parser::SVGParser::Options(), Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

/// Draws \p pixels (\p width x \p height, straight alpha) into \p destRect of an opaque gray
/// 64x64 canvas with tiny-skia directly, the sampling reference resvg and TinySkia share. Its
/// pipeline also picks nearest sampling for a translation-only transform.
RendererBitmap TinySkiaImageReference(const StraightImage& pixels, int width, int height,
                                      const Box2d& destRect, tiny_skia::FilterQuality quality) {
  constexpr int kSize = 64;
  std::vector<std::uint8_t> canvas(kSize * kSize * 4, kReferenceGray);
  for (std::size_t i = 3; i < canvas.size(); i += 4) {
    canvas[i] = 255;
  }
  std::optional<tiny_skia::Pixmap> target =
      tiny_skia::Pixmap::fromVec(std::move(canvas), tiny_skia::IntSize(kSize, kSize));
  const std::vector<std::uint8_t> premultiplied = PremultiplyRgba(pixels);
  const std::optional<tiny_skia::PixmapView> source = tiny_skia::PixmapView::fromBytes(
      premultiplied, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  if (!target.has_value() || !source.has_value()) {
    ADD_FAILURE() << "could not build the tiny-skia reference pixmaps";
    return RendererBitmap{};
  }
  tiny_skia::PixmapPaint paint;
  paint.quality = quality;
  const tiny_skia::Transform targetFromImage = tiny_skia::Transform::fromRow(
      static_cast<float>(destRect.width() / width), 0.0f, 0.0f,
      static_cast<float>(destRect.height() / height), static_cast<float>(destRect.topLeft.x),
      static_cast<float>(destRect.topLeft.y));
  tiny_skia::MutablePixmapView targetView = target->mutableView();
  tiny_skia::Painter::drawPixmap(targetView, 0, 0, *source, paint, targetFromImage);
  const std::span<const std::uint8_t> data = target->data();
  RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(kSize, kSize);
  bitmap.rowBytes = kSize * 4;
  bitmap.alphaType = AlphaType::Premultiplied;
  bitmap.pixels.assign(data.begin(), data.end());
  return bitmap;
}

TEST_F(RendererRegressionTests, ImageSmoothRenderingUsesBilinearSampling) {
  editor::tests::CompareBitmapToBitmap(
      RenderImageOverGray(
          R"svg(<image x="8" y="8" width="48" height="48" image-rendering="smooth" href=")svg" +
          std::string(kCheckerImageDataUri) + "\"/>"),
      TinySkiaImageReference(CheckerPixels(), 4, 4, Box2d::FromXYWH(8, 8, 48, 48),
                             tiny_skia::FilterQuality::Bilinear),
      "image_smooth_bilinear", ImageSamplingOracleParams());
}

TEST_F(RendererRegressionTests, ImageCubicClampsChannelsOnlyToOne) {
  editor::tests::CompareBitmapToBitmap(
      RenderImageOverGray(R"svg(<image x="8" y="8" width="48" height="48" href=")svg" +
                          std::string(kTranslucentCheckerDataUri) + "\"/>"),
      TinySkiaImageReference(TranslucentCheckerPixels(), 4, 4, Box2d::FromXYWH(8, 8, 48, 48),
                             tiny_skia::FilterQuality::Bicubic),
      "image_cubic_translucent", ImageSamplingOracleParams());
}

/// The `image-rendering` values of the sampling tests, with the empty string for none.
constexpr std::array<std::string_view, 8> kImageRenderingValues = {
    "",       "auto",          "optimizeQuality", "high-quality",
    "smooth", "optimizeSpeed", "crisp-edges",     "pixelated"};

std::string ImageRenderingLabel(std::string_view imageRendering) {
  return std::string(imageRendering.empty() ? "unspecified" : imageRendering);
}

/// The `image-rendering` attribute for \p imageRendering, empty when it is.
std::string ImageRenderingAttribute(std::string_view imageRendering) {
  return imageRendering.empty() ? std::string()
                                : " image-rendering=\"" + std::string(imageRendering) + "\"";
}

TEST_F(RendererRegressionTests, ImageTranslatedAtIntrinsicSizeSamplesNearest) {
  // The interior excludes the edge pixels the fractional translation covers partially.
  const Vector2i interiorOrigin(11, 21);
  const Vector2i interiorSize(7, 7);
  const RendererBitmap expected = editor::tests::CropBitmap(
      TinySkiaImageReference(GradientPixels(), 8, 8, Box2d::FromXYWH(10.25, 20.75, 8, 8),
                             tiny_skia::FilterQuality::Bicubic),
      interiorOrigin, interiorSize);
  for (std::string_view imageRendering : kImageRenderingValues) {
    SCOPED_TRACE(testing::Message() << "image-rendering=\"" << imageRendering << "\"");
    editor::tests::CompareBitmapToBitmap(
        editor::tests::CropBitmap(
            RenderImageOverGray(R"svg(<image x="10.25" y="20.75")svg" +
                                ImageRenderingAttribute(imageRendering) + " href=\"" +
                                std::string(kGradientImageDataUri) + "\"/>"),
            interiorOrigin, interiorSize),
        expected, "image_translated_nearest_" + ImageRenderingLabel(imageRendering),
        editor::tests::PixelmatchIdentityParams());
  }
}

// The feImage oracles are `<image>` drawn by tiny-skia, whose sampling resvg shares. Both
// backends' feImage samplers evaluate the kernels exactly, while Geode's own `<image>` blit
// differs from tiny-skia by one 8-bit step in places (hardware bilinear subtexel precision, and
// the blit's cubic under rotation), so it is not an identity oracle.
TEST_F(RendererRegressionTests, FeImageSamplesLikeImageForEachRendering) {
  for (std::string_view imageRendering : kImageRenderingValues) {
    SCOPED_TRACE(testing::Message() << "image-rendering=\"" << imageRendering << "\"");
    const RendererBitmap expected = RenderCheckerImage(imageRendering, RendererBackend::TinySkia);
    AssertVisibleFixture(expected, "feimage_image_oracle_" + ImageRenderingLabel(imageRendering));
    editor::tests::CompareBitmapToBitmap(RenderCheckerFeImage(imageRendering), expected,
                                         "feimage_sampling_" + ImageRenderingLabel(imageRendering),
                                         editor::tests::PixelmatchIdentityParams());
  }
}

/// Renders \p element on a transparent 64x64 canvas through \p filterPrimitives, or unfiltered
/// when they are empty, inside \p groupTransform, with \p backend.
RendererBitmap RenderSamplingCase(std::string_view groupTransform, std::string_view element,
                                  std::string_view filterPrimitives,
                                  RendererBackend backend = ActiveRendererBackend()) {
  std::string body;
  if (!filterPrimitives.empty()) {
    body = R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="-64" y="-64" width="192"
                         height="192">)svg" +
           std::string(filterPrimitives) + "</filter>";
  }
  body += "<g transform=\"" + std::string(groupTransform) + "\">";
  body += filterPrimitives.empty()
              ? std::string(element)
              : std::string(R"svg(<rect width="64" height="64" filter="url(#f)"/>)svg");
  body += "</g>";
  SVGDocument document = instantiateSubtree(body, parser::SVGParser::Options(), Vector2i(64, 64));
  return RenderDocumentWithBackend(document, backend);
}

TEST_F(RendererRegressionTests, FeImageTranslatedAtIntrinsicSizeSamplesLikeImage) {
  // The interior excludes the edge pixels the fractional translation covers partially.
  const Vector2i interiorOrigin(11, 21);
  const Vector2i interiorSize(7, 7);
  for (std::string_view imageRendering : kImageRenderingValues) {
    SCOPED_TRACE(testing::Message() << "image-rendering=\"" << imageRendering << "\"");
    const std::string attributes = R"svg(x="10.25" y="20.75" width="8" height="8")svg" +
                                   ImageRenderingAttribute(imageRendering) + " href=\"" +
                                   std::string(kGradientImageDataUri) + "\"";
    editor::tests::CompareBitmapToBitmap(
        editor::tests::CropBitmap(RenderSamplingCase("", "", "<feImage " + attributes + "/>"),
                                  interiorOrigin, interiorSize),
        editor::tests::CropBitmap(RenderSamplingCase("", "<image " + attributes + "/>", ""),
                                  interiorOrigin, interiorSize),
        "feimage_translated_" + ImageRenderingLabel(imageRendering),
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(RendererRegressionTests, FeImageRotatedSamplesLikeImage) {
  // The interior excludes the image edges, which `<image>` antialiases and feImage clips to whole
  // pixels.
  const Vector2i interiorOrigin(16, 16);
  const Vector2i interiorSize(32, 32);
  for (std::string_view imageRendering : kImageRenderingValues) {
    SCOPED_TRACE(testing::Message() << "image-rendering=\"" << imageRendering << "\"");
    const std::string attributes = R"svg(x="8" y="8" width="48" height="48")svg" +
                                   ImageRenderingAttribute(imageRendering) + " href=\"" +
                                   std::string(kCheckerImageDataUri) + "\"";
    editor::tests::CompareBitmapToBitmap(
        editor::tests::CropBitmap(
            RenderSamplingCase("rotate(30 32 32)", "", "<feImage " + attributes + "/>"),
            interiorOrigin, interiorSize),
        editor::tests::CropBitmap(
            RenderSamplingCase("rotate(30 32 32)", "<image " + attributes + "/>", "",
                               RendererBackend::TinySkia),
            interiorOrigin, interiorSize),
        "feimage_rotated_" + ImageRenderingLabel(imageRendering),
        editor::tests::PixelmatchIdentityParams());
  }
}

TEST_F(RendererRegressionTests, EffectSpanLayoutRunsOncePerTextElement) {
  std::string body =
      R"svg(<defs><clipPath id="clip"><rect width="200" height="200"/></clipPath></defs>
                         <text x="10" y="150" font-family="Noto Sans" font-size="120">)svg";
  for (int span = 0; span < 12; ++span) {
    body += R"svg(<tspan clip-path="url(#clip)">S</tspan>)svg";
  }
  body += "</text>";
  SVGDocument document = instantiateSubtree(body, parser::SVGParser::Options(), Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(document, ResvgResourceRoot() / "fonts");

  const RendererBitmap bitmap = RenderDocumentWithBackend(document, ActiveRendererBackend());
  ExpectVisibleBitmap(bitmap, "effect_span_layout_visible");
  const auto* textEngine = document.registry().ctx().find<TextEngine>();
  ASSERT_NE(textEngine, nullptr);
  EXPECT_EQ(textEngine->layoutCallCountForTesting(), 1u);

  const RendererBitmap second = RenderDocumentWithBackend(document, ActiveRendererBackend());
  ExpectBitmapsIdentical(second, bitmap, "effect_span_second_frame_matches_first");
  EXPECT_EQ(textEngine->layoutCallCountForTesting(), 2u);
}

TEST_F(RendererRegressionTests, ExternalTextEffectContextPaintIsResolvedForEachUse) {
  const std::filesystem::path testdata = "donner/svg/renderer/testdata/regressions";
  SVGDocument shared = loadSVG(
      "donner/svg/renderer/testdata/regressions/use-external-text-effect-context-shared.svg",
      testdata);
  SVGDocument separate = loadSVG(
      "donner/svg/renderer/testdata/regressions/use-external-text-effect-context-separate.svg",
      testdata);
  RegisterFontsFromDirectoryForTesting(shared, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(separate, ResvgResourceRoot() / "fonts");

  const RendererBitmap actual = RenderDocumentWithBackend(shared, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(separate, ActiveRendererBackend());
  ExpectVisibleBitmap(expected, "external_text_effect_context_visible");
  ExpectBitmapsIdentical(actual, expected, "external_text_effect_context_per_use");
}

TEST_F(RendererRegressionTests, EffectOwnerContextPaintOverridesTheTextRootPaint) {
  const std::filesystem::path testdata = "donner/svg/renderer/testdata/regressions";
  SVGDocument context =
      loadSVG("donner/svg/renderer/testdata/regressions/use-external-text-effect-owner-context.svg",
              testdata);
  SVGDocument explicitPaint = loadSVG(
      "donner/svg/renderer/testdata/regressions/use-external-text-effect-owner-explicit.svg",
      testdata);
  RegisterFontsFromDirectoryForTesting(context, ResvgResourceRoot() / "fonts");
  RegisterFontsFromDirectoryForTesting(explicitPaint, ResvgResourceRoot() / "fonts");

  const RendererBitmap actual = RenderDocumentWithBackend(context, ActiveRendererBackend());
  const RendererBitmap expected = RenderDocumentWithBackend(explicitPaint, ActiveRendererBackend());
  ExpectVisibleBitmap(expected, "effect_owner_context_paint_visible");
  ExpectBitmapsIdentical(actual, expected, "effect_owner_context_paint_matches_explicit_red");
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

/// A transformed, clipped text render and the boxes its text element reports.
struct ClippedTextRender {
  RendererBitmap bitmap;  //!< The rendered 200x200 canvas.
  Box2d objectBox;        //!< The text's object bounding box, in its own user space.
  Box2d inkBox;           //!< The text's glyph ink extents, in its own user space.
};

/// Renders "Hog" in 48px Noto Sans, rotated and translated by its own `transform`, under the
/// `#clip` clip path that \p clipPath defines.
ClippedTextRender RenderTransformedClippedText(std::string_view clipPath) {
  SVGDocument document = instantiateSubtree(
      std::string(clipPath) +
          R"svg(<text id="text" font-family="Noto Sans" font-size="48" fill="green"
                      clip-path="url(#clip)" transform="translate(40 30) rotate(30)">Hog</text>)svg",
      parser::SVGParser::Options(), Vector2i(200, 200));
  RegisterFontsFromDirectoryForTesting(document, ResvgResourceRoot() / "fonts");
  ClippedTextRender result;
  // Rendering prepares the text layout that the boxes are derived from. The software renderer runs
  // in every variant, so both text tiers draw this case.
  result.bitmap = RenderDocumentWithBackend(document, RendererBackend::TinySkia);
  const auto text = document.querySelector("#text");
  EXPECT_THAT(text, testing::Ne(std::nullopt));
  if (text) {
    result.objectBox = text->cast<SVGTextElement>().objectBoundingBox();
    result.inkBox = text->cast<SVGTextElement>().inkBoundingBox();
  }
  return result;
}

/// The `#clip` userSpaceOnUse clip path whose rectangle covers the middle 60% of \p box, the
/// placement an objectBoundingBox rectangle at 0.2 with size 0.6 resolves to on that box.
std::string UserSpaceClipOnMiddleOf(const Box2d& box) {
  std::ostringstream clipPath;
  clipPath << std::setprecision(17)
           << R"svg(<clipPath id="clip" clipPathUnits="userSpaceOnUse"><rect x=")svg"
           << box.topLeft.x + 0.2 * box.width() << R"svg(" y=")svg"
           << box.topLeft.y + 0.2 * box.height() << R"svg(" width=")svg" << 0.6 * box.width()
           << R"svg(" height=")svg" << 0.6 * box.height() << R"svg("/></clipPath>)svg";
  return clipPath.str();
}

/// Counts the pixels where \p actual differs from \p expected under the canonical identity
/// comparison, without reporting the difference as a failure. Returns -1 when the two bitmaps
/// cannot be compared, for example because their sizes differ.
int IdentityMismatchCount(const RendererBitmap& actual, const RendererBitmap& expected,
                          std::string_view label) {
  int mismatchedPixels = -1;
  testing::TestPartResultArray differences;
  {
    testing::ScopedFakeTestPartResultReporter capture(
        testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &differences);
    editor::tests::CompareBitmapToBitmap(
        actual, expected, label, editor::tests::PixelmatchIdentityParams(), &mismatchedPixels);
  }
  return mismatchedPixels;
}

// An objectBoundingBox clip on text with a `transform` maps the unit square onto the text's
// object bounding box in the text's own user space, which the transform then places on the
// canvas. For text that box is the union of the glyph cells: each glyph's advance by the font's
// ascent and descent, not its ink. The same rectangle in userSpaceOnUse units must clip
// identically, and the rectangle placed on the ink extents must not.
TEST_F(RendererRegressionTests, ObjectBoundingBoxClipOnTransformedTextUsesLocalGlyphCells) {
  const ClippedTextRender objectUnits = RenderTransformedClippedText(
      R"svg(<clipPath id="clip" clipPathUnits="objectBoundingBox">
              <rect x="0.2" y="0.2" width="0.6" height="0.6"/></clipPath>)svg");
  AssertVisibleFixture(objectUnits.bitmap, "obb_clip_on_transformed_text_visible");

  // Noto Sans Regular has a 1000-unit em, ascent 1069 and descent -293. The text sits at its
  // local origin, so its glyph cells span y from -1.069 to 0.293 em at 48px.
  EXPECT_THAT(objectUnits.objectBox.topLeft, Vector2Near(0.0, -1.069 * 48.0));
  EXPECT_THAT(objectUnits.objectBox.bottomRight.y, testing::DoubleNear(0.293 * 48.0, 0.01));
  // The ink control is only meaningful if the two boxes differ.
  ASSERT_THAT(objectUnits.inkBox.topLeft.y, testing::Gt(objectUnits.objectBox.topLeft.y + 1.0));

  const ClippedTextRender cellUserSpace =
      RenderTransformedClippedText(UserSpaceClipOnMiddleOf(objectUnits.objectBox));
  editor::tests::CompareBitmapToBitmap(objectUnits.bitmap, cellUserSpace.bitmap,
                                       "obb_clip_matches_glyph_cell_user_space_clip",
                                       editor::tests::PixelmatchIdentityParams());

  const ClippedTextRender inkUserSpace =
      RenderTransformedClippedText(UserSpaceClipOnMiddleOf(objectUnits.inkBox));
  EXPECT_THAT(
      IdentityMismatchCount(objectUnits.bitmap, inkUserSpace.bitmap, "obb_clip_versus_ink_clip"),
      testing::Gt(0))
      << "a clip on the ink extents must render differently";
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
      {"filter", R"svg(<filter id="e"><feGaussianBlur stdDeviation="3"/></filter>)svg",
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

/// Copy the \p size pixel window at \p origin out of \p source.
RendererBitmap CropBitmap(const RendererBitmap& source, Vector2i origin, Vector2i size) {
  RendererBitmap result;
  if (origin.x < 0 || origin.y < 0 || origin.x + size.x > source.dimensions.x ||
      origin.y + size.y > source.dimensions.y) {
    ADD_FAILURE() << "Crop window lies outside the source bitmap";
    return result;
  }
  result.dimensions = size;
  result.rowBytes = static_cast<std::size_t>(size.x) * 4u;
  result.alphaType = source.alphaType;
  result.pixels.resize(result.rowBytes * static_cast<std::size_t>(size.y));
  for (int y = 0; y < size.y; ++y) {
    const auto sourceRow = source.pixels.begin() +
                           static_cast<std::ptrdiff_t>((origin.y + y) * source.rowBytes +
                                                       static_cast<std::size_t>(origin.x) * 4u);
    std::copy_n(sourceRow, result.rowBytes,
                result.pixels.begin() + static_cast<std::ptrdiff_t>(y * result.rowBytes));
  }
  return result;
}

/// A filter that reads input pixels away from each output position, so pixels near a canvas edge
/// depend on filtered content that lies beyond the canvas. Each edge has its own primitives, so a
/// primitive that shifts content pulls it in from beyond that edge.
struct SpreadingFilterCase {
  std::string_view name;              ///< Test name suffix.
  std::string_view topLeft;           ///< Primitives for content past the top-left edges.
  std::string_view bottomRight;       ///< Primitives for content past the bottom-right edges.
  std::string_view filterAttributes;  ///< Extra attributes on the filter element.
};

/// Which canvas edges the filtered rectangle runs past.
enum class OffCanvasEdges { TopLeft, BottomRight };

using FilterInputMarginParam = std::tuple<SpreadingFilterCase, OffCanvasEdges>;

class FilterInputMarginTest : public testing::TestWithParam<FilterInputMarginParam> {};

/// Renders a filtered lime square through a filter whose region extends well past the canvas.
/// \p shift translates the whole scene, so a larger canvas can show the same scene with the
/// off-canvas content in view.
RendererBitmap RenderSpreadingFilter(const SpreadingFilterCase& filter, OffCanvasEdges edges,
                                     Vector2i canvasSize, int shift) {
  const bool topLeft = edges == OffCanvasEdges::TopLeft;
  const std::string_view rect = topLeft ? R"svg(x="-100" y="-100" width="140" height="140")svg"
                                        : R"svg(x="24" y="24" width="140" height="140")svg";
  const std::string markup =
      R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="-200" y="-200" width="464" height="464" )svg" +
      std::string(filter.filterAttributes) + ">" +
      std::string(topLeft ? filter.topLeft : filter.bottomRight) + R"svg(</filter>
    <g transform="translate()svg" +
      std::to_string(shift) + " " + std::to_string(shift) +
      R"svg()"><g filter="url(#f)"><rect )svg" + std::string(rect) +
      R"svg( fill="lime"/></g></g>)svg";
  SVGDocument document = instantiateSubtree(markup, {}, canvasSize);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

/// Renders \p render at 64x64 and, as the oracle, on a canvas with 64 pixels of room on every side
/// cropped back to the original canvas; every reach in these scenes is far smaller than that room.
template <typename Render>
void ExpectEdgePixelsMatchRoomyCanvas(const Render& render, std::string_view label) {
  const RendererBitmap actual = render(Vector2i(64, 64), /*shift=*/0);
  const RendererBitmap roomy = render(Vector2i(192, 192), /*shift=*/64);
  ExpectVisibleBitmap(actual, std::string(label) + "_visible");
  editor::tests::CompareBitmapToBitmap(actual,
                                       CropBitmap(roomy, Vector2i(64, 64), Vector2i(64, 64)), label,
                                       editor::tests::PixelmatchIdentityParams());
}

TEST_P(FilterInputMarginTest, ContentBeyondTheCanvasReachesEdgePixels) {
  const auto& [filter, edges] = GetParam();
  ExpectEdgePixelsMatchRoomyCanvas(
      [&](Vector2i canvasSize, int shift) {
        return RenderSpreadingFilter(filter, edges, canvasSize, shift);
      },
      "filter_input_margin");
}

/// Renders an feImage of a lime square, blurred, inside a filtered group translated so the square
/// and the filter region run past the canvas's top-left corner. The filter region is no larger than
/// the canvas, so the whole feImage subregion is rasterized. \p shift translates the whole scene,
/// as above.
RendererBitmap RenderBlurredFeImage(Vector2i canvasSize, int shift) {
  const std::string markup =
      R"svg(<defs>
        <filter id="f" filterUnits="userSpaceOnUse" x="80" y="80" width="60" height="60">
          <feImage href="#src"/><feGaussianBlur stdDeviation="3"/>
        </filter>
        <rect id="src" width="140" height="140" fill="lime"/>
      </defs>
      <g transform="translate()svg" +
      std::to_string(shift) + " " + std::to_string(shift) +
      R"svg()"><g transform="translate(-100 -100)" filter="url(#f)">
        <rect width="400" height="400" fill="red"/>
      </g></g>)svg";
  SVGDocument document = instantiateSubtree(markup, {}, canvasSize);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

// An feImage result feeds the blur like any other input, so the referenced content just past the
// canvas edge must reach the blurred edge pixels.
TEST_F(RendererRegressionTests, BlurredFeImageReadsContentBeyondTheCanvas) {
  ExpectEdgePixelsMatchRoomyCanvas(RenderBlurredFeImage, "blurred_fe_image_input_margin");
}

// The transformed blur path blurs a rotated element along its own axes. It must keep working when
// the capture gets an input margin, here for an anisotropic blur crossing the top-left edges. A
// quarter turn keeps every transform exact, so the shifted oracle matches bit for bit.
RendererBitmap RenderRotatedAnisotropicBlur(Vector2i canvasSize, int shift) {
  const std::string markup =
      R"svg(<filter id="f"><feGaussianBlur stdDeviation="8 1"/></filter>
      <g transform="translate()svg" +
      std::to_string(shift) + " " + std::to_string(shift) +
      R"svg()"><g transform="rotate(90 10 10)" filter="url(#f)">
        <rect x="-20" y="-20" width="60" height="60" fill="lime"/>
      </g></g>)svg";
  SVGDocument document = instantiateSubtree(markup, {}, canvasSize);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

TEST_F(RendererRegressionTests, RotatedAnisotropicBlurReadsContentBeyondTheCanvas) {
  ExpectEdgePixelsMatchRoomyCanvas(RenderRotatedAnisotropicBlur, "rotated_blur_input_margin");
}

/// Renders \p primitives on a band under a horizontal skew. The band's filter region runs 50
/// pixels past the right canvas edge, so a capture with the whole sampling reach covers all of it
/// and renders the same pixels as a canvas with room past that edge.
RendererBitmap RenderSkewedFilter(std::string_view primitives, Vector2i canvasSize) {
  const std::string markup =
      R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="-120" y="30" width="58" height="14">)svg" +
      std::string(primitives) + R"svg(</filter>
      <g transform="matrix(1 0 4 1 0 0)" filter="url(#f)">
        <rect x="-110" y="30" width="48" height="14" fill="lime"/>
      </g>)svg";
  SVGDocument document = instantiateSubtree(markup, {}, canvasSize);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

/// Stack the \p width pixels starting at column \p x0 of each of \p rows of \p source.
RendererBitmap RowStrips(const RendererBitmap& source, const std::vector<int>& rows, int x0,
                         int width) {
  RendererBitmap result;
  result.dimensions = Vector2i(width, static_cast<int>(rows.size()));
  result.rowBytes = static_cast<std::size_t>(width) * 4u;
  result.alphaType = source.alphaType;
  for (int y : rows) {
    const RendererBitmap row = CropBitmap(source, Vector2i(x0, y), Vector2i(width, 1));
    result.pixels.insert(result.pixels.end(), row.pixels.begin(), row.pixels.end());
  }
  return result;
}

// The transformed blur path samples along the skewed filter axes, so the capture must cover the
// device footprint of the sampling reach: a blur of 3 here reads about 45 pixels along device X.
//
// Oracle: on a canvas as wide as the filter region the whole band is in view, and some of its
// blurred rows are level across the 12 columns that end at x 64. Where the band runs on past the
// 64 pixel canvas, those rows must stay level up to its edge, so the edge strip (columns 58..63)
// of the 64 pixel render must match its interior strip (columns 52..57), raw pixels through
// pixelmatch. Each edge row must also be visible, so a row missing from both strips cannot pass
// as level. The strips come from one render because Geode's skewed resampling rounds a level or
// two differently for different canvas sizes, so a render on another canvas is not an exact
// reference.
TEST_F(RendererRegressionTests, SkewedBlurReadsContentBeyondTheCanvas) {
  for (std::string_view primitives :
       {std::string_view(R"svg(<feGaussianBlur stdDeviation="3"/>)svg"),
        std::string_view(
            R"svg(<feGaussianBlur stdDeviation="3"/><feOffset dx="-2" dy="-2"/>)svg")}) {
    SCOPED_TRACE(primitives);
    const RendererBitmap actual = RenderSkewedFilter(primitives, Vector2i(64, 64));
    const RendererBitmap roomy = RenderSkewedFilter(primitives, Vector2i(114, 64));
    ExpectVisibleBitmap(actual, "skewed_blur_input_margin_visible");
    std::vector<int> levelRows;
    for (int y = 0; y < 64; ++y) {
      const RendererBitmap row = CropBitmap(roomy, Vector2i(52, y), Vector2i(12, 1));
      // A row is checked when it is covered and every pixel matches its first one.
      bool level = row.pixels[3] != 0;
      for (int x = 1; x < 12 && level; ++x) {
        level = std::equal(row.pixels.begin(), row.pixels.begin() + 4, row.pixels.begin() + 4 * x);
      }
      if (level) {
        levelRows.push_back(y);
      }
    }
    ASSERT_THAT(levelRows, testing::Not(testing::IsEmpty()))
        << "the scene must have level rows to check";
    editor::tests::CompareBitmapToBitmap(
        RowStrips(actual, levelRows, 58, 6), RowStrips(actual, levelRows, 52, 6),
        "skewed_blur_edge_strip_level", editor::tests::PixelmatchIdentityParams());
    for (int y : levelRows) {
      SCOPED_TRACE(testing::Message() << "row " << y);
      ExpectVisibleBitmap(CropBitmap(actual, Vector2i(58, y), Vector2i(6, 1)),
                          "skewed_blur_edge_row_visible");
    }
  }
}

// Both executors round each feOffset to whole pixels, so eight offsets of 0.6 move content 8
// pixels, not 4.8. The capture must reach that far: a square just past the left edge lands at the
// left edge, matching the same square drawn there without a filter.
TEST_F(RendererRegressionTests, ChainedFractionalOffsetsReachTheirRoundedDistance) {
  std::string offsets;
  for (int i = 0; i < 8; ++i) {
    offsets += R"svg(<feOffset dx="0.6"/>)svg";
  }
  SVGDocument document = instantiateSubtree(
      R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="-50" y="0" width="114" height="64">)svg" +
          offsets + R"svg(</filter>
      <g filter="url(#f)"><rect x="-8" y="8" width="8" height="16" fill="lime"/></g>)svg",
      {}, Vector2i(64, 64));
  SVGDocument expected = instantiateSubtree(
      R"svg(<rect x="0" y="8" width="8" height="16" fill="lime"/>)svg", {}, Vector2i(64, 64));
  editor::tests::CompareBitmapToBitmap(RenderDocumentWithBackend(document, ActiveRendererBackend()),
                                       RenderDocumentWithBackend(expected, ActiveRendererBackend()),
                                       "chained_fractional_offsets",
                                       editor::tests::PixelmatchIdentityParams());
}

// Under a skew the transformed blur path runs offsets on a local raster scaled by each filter
// axis's length and rounds them there. Under matrix(1 0 1 1 0 0) an offset of (0.6, -0.6) moves
// nothing along device X before rounding, but rounds to (1, -1) local pixels, which moves content
// about 0.29 device pixels right; 24 of them, with the blur that selects the transformed path,
// move it about 7 pixels. Content just past the left edge then lands inside the canvas, at
// (2, 25).
TEST_F(RendererRegressionTests, SkewedLocalRasterOffsetsReachTheirRoundedDistance) {
  std::string primitives;
  for (int i = 0; i < 24; ++i) {
    primitives += R"svg(<feOffset dx="0.6" dy="-0.6"/>)svg";
  }
  primitives += R"svg(<feGaussianBlur stdDeviation="0.01"/>)svg";
  SVGDocument document = instantiateSubtree(
      R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="-100" y="0" width="200" height="64">)svg" +
          primitives + R"svg(</filter>
      <g transform="matrix(1 0 1 1 0 0)" filter="url(#f)">
        <rect x="-52" y="36" width="8" height="12" fill="lime"/>
      </g>)svg",
      {}, Vector2i(64, 64));
  const RendererBitmap actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
  EXPECT_THAT(PixelAt(actual, 2, 25),
              test::Rgba(testing::Eq(0), testing::Gt(200), testing::Eq(0), testing::Gt(200)))
      << "the offset content must land inside the canvas";
}

// A capture margin must never turn an admissible filter into a frame-wide rejection. At 8x zoom a
// large blur's margin would need a raster over the frame's filter memory, so the layer falls back
// to a viewport-sized capture; it still renders, and so does a later filtered element.
TEST_F(RendererRegressionTests, LargeBlurMarginFallsBackWithinFrameFilterBudget) {
  SVGDocument document = ParseSvg(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 128 128">
      <filter id="f" filterUnits="userSpaceOnUse" x="-2000" y="-2000" width="4256" height="4256">
        <feGaussianBlur stdDeviation="40"/>
      </filter>
      <filter id="g"><feFlood flood-color="blue"/></filter>
      <rect x="-1000" y="-1000" width="2128" height="2128" fill="lime" filter="url(#f)"/>
      <rect x="120" y="2" width="6" height="6" filter="url(#g)"/>
    </svg>)svg");
  const RendererBitmap actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
  ASSERT_THAT(actual.dimensions, testing::Eq(Vector2i(1024, 1024)));
  EXPECT_THAT(PixelAt(actual, 512, 512),
              test::Rgba(testing::Eq(0), testing::Gt(0), testing::Eq(0), testing::Gt(0)))
      << "the blurred element must render";
  EXPECT_THAT(PixelAt(actual, 984, 40), test::RgbaEq(0, 0, 255, 255))
      << "a later filtered element must still render";
}

/// Renders a turbulence-filtered square, optionally inside a group whose filter shifts its content
/// out and back. That outer filter reads past the canvas but changes no pixels, so it must not
/// move the inner turbulence noise either.
RendererBitmap RenderTurbulenceInsideShift(bool shifted) {
  const std::string inner =
      R"svg(<rect x="-10" y="-10" width="40" height="40" filter="url(#t)"/>)svg";
  const std::string markup =
      std::string(R"svg(<filter id="t"><feTurbulence baseFrequency="0.1" numOctaves="2"/></filter>
      <filter id="shift" filterUnits="userSpaceOnUse" x="-50" y="-50" width="200" height="200">
        <feOffset dx="5" dy="5"/><feOffset dx="-5" dy="-5"/>
      </filter>)svg") +
      (shifted ? R"svg(<g filter="url(#shift)">)svg" + inner + "</g>" : "<g>" + inner + "</g>");
  SVGDocument document = instantiateSubtree(markup, {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

TEST_F(RendererRegressionTests, NestedTurbulenceKeepsItsNoiseUnderAnOuterInputMargin) {
  const RendererBitmap expected = RenderTurbulenceInsideShift(false);
  ExpectVisibleBitmap(expected, "nested_turbulence_visible");
  editor::tests::CompareBitmapToBitmap(RenderTurbulenceInsideShift(true), expected,
                                       "nested_turbulence_input_margin",
                                       editor::tests::PixelmatchIdentityParams());
}

// A large feImage fragment is rasterized only over what can reach the visible surface. A later
// primitive that pulls fragment content into view extends that reach: here the offset brings a
// square from beyond the visible fragment window to device pixels (3..8, 10..15). The square lies
// outside the canvas-sized fallback window too, so only a window widened by the reach finds it.
TEST_F(RendererRegressionTests, FeImageFragmentWindowIncludesDownstreamReach) {
  SVGDocument document = instantiateSubtree(
      R"svg(<defs>
        <filter id="f" filterUnits="userSpaceOnUse" x="-100" y="-20" width="280" height="200">
          <feImage href="#src"/><feOffset dy="-10"/>
        </filter>
        <rect id="src" x="110" y="22" width="5" height="5" fill="lime"/>
      </defs>
      <g transform="rotate(90)" filter="url(#f)">
        <rect x="-100" y="-80" width="280" height="200" fill="red"/>
      </g>)svg",
      {}, Vector2i(64, 64));
  const RendererBitmap actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
  ExpectVisibleBitmap(actual, "fe_image_downstream_reach_visible");
  EXPECT_THAT(PixelAt(actual, 5, 12),
              test::Rgba(testing::Eq(0), testing::Gt(200), testing::Eq(0), testing::Gt(200)))
      << "the offset square must render";
  EXPECT_THAT(PixelAt(actual, 40, 40), test::RgbaEq(0, 0, 0, 0));
}

/// Renders the off-canvas feImage scene: before the translate(-96 -96) host transform, the
/// referenced square and the filtered element lie beyond the 64x64 canvas. \p primitives follow
/// the feImage, whose result is named "i".
RendererBitmap RenderOffCanvasFeImage(std::string_view primitives) {
  SVGDocument document = instantiateSubtree(std::string(R"svg(<defs>
        <filter id="f" filterUnits="userSpaceOnUse" x="0" y="0" width="200" height="200">
          <feImage href="#src" result="i"/>)svg") +
                                                std::string(primitives) + R"svg(
        </filter>
        <rect id="src" x="104" y="104" width="16" height="16" fill="lime"/>
      </defs>
      <g transform="translate(-96 -96)" filter="url(#f)">
        <rect x="96" y="96" width="48" height="48" fill="red"/>
      </g>)svg",
                                            {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

// A reach-widened fragment window that exceeds the canvas-sized byte limit must fall back to the
// unwidened visible window, never to the canvas window, which ignores the host transform. A graph
// whose reach is the whole region keeps the unwidened visible window for the same reason.
TEST_F(RendererRegressionTests, FeImageFragmentWindowFallsBackToVisibleWindow) {
  for (std::string_view primitives :
       {std::string_view(R"svg(<feGaussianBlur stdDeviation="1"/>)svg"),
        std::string_view(R"svg(<feTile in="i"/>)svg")}) {
    SCOPED_TRACE(primitives);
    const RendererBitmap actual = RenderOffCanvasFeImage(primitives);
    ExpectVisibleBitmap(actual, "fe_image_visible_window_fallback_visible");
    EXPECT_THAT(PixelAt(actual, 16, 16),
                test::Rgba(testing::Eq(0), testing::Gt(200), testing::Eq(0), testing::Gt(200)))
        << "the referenced square must render";
  }
}

// Morphology dilation spreads content that lies entirely beyond the canvas into view.
TEST_F(RendererRegressionTests, DilateSpreadsContentFromBeyondTheCanvas) {
  ExpectEdgePixelsMatchRoomyCanvas(
      [](Vector2i canvasSize, int shift) {
        SVGDocument document = instantiateSubtree(
            std::string(R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="-200" y="-200"
                width="464" height="464"><feMorphology operator="dilate" radius="6"/></filter>
              <g transform="translate()svg") +
                std::to_string(shift) + " " + std::to_string(shift) + R"svg()">
                <g filter="url(#f)"><rect x="-40" y="-40" width="38" height="38" fill="lime"/></g>
              </g>)svg",
            {}, canvasSize);
        return RenderDocumentWithBackend(document, ActiveRendererBackend());
      },
      "dilate_input_margin");
}

/// Renders \p replayed, which draws turbulence-filtered content again out of draw order, inside a
/// filter layer. When \p shifted, that layer reads past each output pixel and so gets an input
/// margin; otherwise it reads no neighbors. Turbulence noise follows raster pixel coordinates, so
/// the two renders differ only if the margin moves the noise.
RendererBitmap RenderReplayedTurbulence(std::string_view replayed, bool shifted) {
  SVGDocument document = instantiateSubtree(
      std::string(R"svg(<filter id="t"><feTurbulence baseFrequency="0.1" numOctaves="2"/></filter>
      <filter id="shift" filterUnits="userSpaceOnUse" x="-50" y="-50" width="200" height="200">
        <feOffset dx="5" dy="5"/><feOffset dx="-5" dy="-5"/>
      </filter>
      <filter id="same" filterUnits="userSpaceOnUse" x="-50" y="-50" width="200" height="200">
        <feColorMatrix type="saturate" values="1"/>
      </filter>
      <mask id="m"><rect x="-10" y="-10" width="40" height="40" fill="white" filter="url(#t)"/>
      </mask>
      <marker id="turbulent" markerUnits="userSpaceOnUse" markerWidth="64" markerHeight="64"
          overflow="visible">
        <rect x="-10" y="-10" width="40" height="40" filter="url(#t)"/>
      </marker>
      <marker id="masked" markerUnits="userSpaceOnUse" markerWidth="64" markerHeight="64"
          overflow="visible">
        <rect width="64" height="64" fill="lime" mask="url(#m)"/>
      </marker>)svg") +
          std::string(replayed) +
          (shifted ? R"svg(<g filter="url(#shift)">)svg" : R"svg(<g filter="url(#same)">)svg") +
          std::string(replayed) + "</g>",
      {}, Vector2i(64, 64));
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

// Mask and marker content draws again at the referencing element, so turbulence in it must keep
// its noise when that element lies inside a filter layer with an input margin. The cases cover a
// mask, a marker, and a mask inside a marker.
TEST_F(RendererRegressionTests, ReplayedTurbulenceKeepsItsNoiseUnderAnInputMargin) {
  for (std::string_view replayed :
       {std::string_view(R"svg(<rect width="64" height="64" fill="lime" mask="url(#m)"/>)svg"),
        std::string_view(R"svg(<path d="M0 0 L1 0" marker-start="url(#turbulent)"/>)svg"),
        std::string_view(R"svg(<path d="M0 0 L1 0" marker-start="url(#masked)"/>)svg")}) {
    SCOPED_TRACE(replayed);
    const RendererBitmap expected = RenderReplayedTurbulence(replayed, /*shifted=*/false);
    ExpectVisibleBitmap(expected, "replayed_turbulence_visible");
    editor::tests::CompareBitmapToBitmap(RenderReplayedTurbulence(replayed, /*shifted=*/true),
                                         expected, "replayed_turbulence_input_margin",
                                         editor::tests::PixelmatchIdentityParams());
  }
}

// A primitive with wrapping edges reads the opposite edge of its input. Here the filter region
// starts at the left canvas edge and ends 36 pixels beyond the right one, and a lime strip lies at
// its far end, beyond the canvas and beyond the primitive's own reach. Wrapping must still carry
// that strip around to the left canvas edge.
TEST_F(RendererRegressionTests, WrapEdgesReadTheOppositeRegionEdgeBeyondTheCanvas) {
  for (std::string_view primitive :
       {std::string_view(R"svg(<feGaussianBlur stdDeviation="3" edgeMode="wrap"/>)svg"),
        std::string_view(R"svg(<feConvolveMatrix order="5" edgeMode="wrap"
            kernelMatrix="1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1"/>)svg")}) {
    SCOPED_TRACE(primitive);
    SVGDocument document = instantiateSubtree(
        std::string(R"svg(<filter id="f" filterUnits="userSpaceOnUse" x="0" y="-200" width="100"
            height="464">)svg") +
            std::string(primitive) + R"svg(</filter>
        <g filter="url(#f)"><rect x="88" y="-200" width="12" height="464" fill="lime"/></g>)svg",
        {}, Vector2i(64, 64));
    const RendererBitmap actual = RenderDocumentWithBackend(document, ActiveRendererBackend());
    ExpectVisibleBitmap(actual, "wrap_input_margin_visible");
    EXPECT_THAT(PixelAt(actual, 0, 32),
                test::Rgba(testing::Eq(0), testing::Gt(0), testing::Eq(0), testing::Gt(0)))
        << "the strip must wrap into view";
    EXPECT_THAT(PixelAt(actual, 32, 32), test::RgbaEq(0, 0, 0, 0));
  }
}

// An empty edge entry means the case does not discriminate on that edge and is not run there.
constexpr std::array<SpreadingFilterCase, 15> kSpreadingFilterCases = {{
    {"Blur", R"svg(<feGaussianBlur stdDeviation="3"/>)svg",
     R"svg(<feGaussianBlur stdDeviation="3"/>)svg", ""},
    {"BlurDuplicateEdges", R"svg(<feGaussianBlur stdDeviation="3" edgeMode="duplicate"/>)svg",
     R"svg(<feGaussianBlur stdDeviation="3" edgeMode="duplicate"/>)svg", ""},
    {"BlurWrapEdges", R"svg(<feGaussianBlur stdDeviation="3" edgeMode="wrap"/>)svg",
     R"svg(<feGaussianBlur stdDeviation="3" edgeMode="wrap"/>)svg", ""},
    {"ObjectBoundingBoxBlur", R"svg(<feGaussianBlur stdDeviation="0.03"/>)svg",
     R"svg(<feGaussianBlur stdDeviation="0.03"/>)svg",
     R"svg(primitiveUnits="objectBoundingBox")svg"},
    {"Erode", R"svg(<feMorphology operator="erode" radius="4"/>)svg",
     R"svg(<feMorphology operator="erode" radius="4"/>)svg", ""},
    {"Convolve",
     R"svg(<feConvolveMatrix order="5" edgeMode="none"
        kernelMatrix="1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1"/>)svg",
     R"svg(<feConvolveMatrix order="5" edgeMode="none"
        kernelMatrix="1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1"/>)svg",
     ""},
    {"ConvolveDuplicateEdges",
     R"svg(<feConvolveMatrix order="5" edgeMode="duplicate"
        kernelMatrix="1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1"/>)svg",
     R"svg(<feConvolveMatrix order="5" edgeMode="duplicate"
        kernelMatrix="1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1"/>)svg",
     ""},
    {"ConvolveWrapEdges",
     R"svg(<feConvolveMatrix order="5" edgeMode="wrap"
        kernelMatrix="1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1"/>)svg",
     R"svg(<feConvolveMatrix order="5" edgeMode="wrap"
        kernelMatrix="1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1"/>)svg",
     ""},
    {"DropShadow", R"svg(<feDropShadow dx="6" dy="6" stdDeviation="2"/>)svg",
     R"svg(<feDropShadow dx="-6" dy="-6" stdDeviation="2"/>)svg", ""},
    {"Offset", R"svg(<feOffset dx="10" dy="10"/>)svg", R"svg(<feOffset dx="-10" dy="-10"/>)svg",
     ""},
    {"BlurThenOffset", R"svg(<feGaussianBlur stdDeviation="2"/><feOffset dx="8" dy="8"/>)svg",
     R"svg(<feGaussianBlur stdDeviation="2"/><feOffset dx="-8" dy="-8"/>)svg", ""},
    // Red is 0 and green is 1, so the red channel displaces toward the top-left and the green
    // channel toward the bottom-right.
    {"Displacement",
     R"svg(<feDisplacementMap in="SourceGraphic" in2="SourceGraphic" scale="20"
        xChannelSelector="R" yChannelSelector="R"/>)svg",
     R"svg(<feDisplacementMap in="SourceGraphic" in2="SourceGraphic" scale="20"
        xChannelSelector="G" yChannelSelector="G"/>)svg",
     ""},
    // Lighting at the bottom-right edges matches with or without the margin, so it runs only at
    // the top-left edges.
    {"DiffuseLighting",
     R"svg(<feDiffuseLighting surfaceScale="10"><feDistantLight azimuth="45" elevation="30"/>
        </feDiffuseLighting>)svg",
     "", ""},
    // The tile source lies entirely beyond the canvas.
    {"Tile",
     R"svg(<feOffset x="-60" y="-60" width="20" height="20" result="t"/><feTile in="t"/>)svg",
     R"svg(<feOffset x="100" y="100" width="20" height="20" result="t"/><feTile in="t"/>)svg", ""},
    // The tile source subregion is shifted content, so the capture must also cover what the
    // offset reads beyond that subregion.
    {"OffsetTile",
     R"svg(<feOffset dx="30" dy="30" x="-60" y="-60" width="20" height="20" result="t"/>
        <feTile in="t"/>)svg",
     R"svg(<feOffset dx="-30" dy="-30" x="100" y="100" width="20" height="20" result="t"/>
        <feTile in="t"/>)svg",
     ""},
}};

/// Every case on every edge it has markup for.
std::vector<FilterInputMarginParam> SpreadingFilterParams() {
  std::vector<FilterInputMarginParam> params;
  for (const SpreadingFilterCase& filter : kSpreadingFilterCases) {
    if (!filter.topLeft.empty()) {
      params.emplace_back(filter, OffCanvasEdges::TopLeft);
    }
    if (!filter.bottomRight.empty()) {
      params.emplace_back(filter, OffCanvasEdges::BottomRight);
    }
  }
  return params;
}

INSTANTIATE_TEST_SUITE_P(SpreadingPrimitives, FilterInputMarginTest,
                         testing::ValuesIn(SpreadingFilterParams()),
                         [](const testing::TestParamInfo<FilterInputMarginParam>& info) {
                           return std::string(std::get<0>(info.param).name) +
                                  (std::get<1>(info.param) == OffCanvasEdges::TopLeft
                                       ? "TopLeft"
                                       : "BottomRight");
                         });

class ClipContourClosureTest : public testing::TestWithParam<std::string_view> {};

TEST_P(ClipContourClosureTest, OpenAndExplicitClosureProduceIdenticalMasks) {
  const std::string data(GetParam());
  SCOPED_TRACE(data);
  const auto render = [](std::string_view path) {
    auto document = instantiateSubtree(
        "<clipPath id='clip'><path d='" + std::string(path) +
            "'/></clipPath><rect width='64' height='64' fill='red' clip-path='url(#clip)'/>",
        {}, Vector2i(64, 64));
    return RenderDocumentWithBackend(document, ActiveRendererBackend());
  };
  const auto actual = render(data);
  const auto expected = render(data + "Z");
  editor::tests::CompareBitmapToBitmap(actual, expected, "clip_contour_implicit_close",
                                       editor::tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(CurvesAndContours, ClipContourClosureTest,
                         testing::Values("M8 32Q32 0 56 32T8 32",
                                         "M8 32A24 20 0 1 1 56 32A24 20 0 1 1 8 32", "M8 8H56V56H8",
                                         "M8 8H56V56H8 M20 20H44V44H20"));

TEST(ClipContourClosure, NormalOpenStrokeKeepsItsEndpoints) {
  auto actualDocument = instantiateSubtree(
      "<path d='M8 8H48V48' fill='none' stroke='red' stroke-width='8'/>", {}, Vector2i(64, 64));
  auto expectedDocument = instantiateSubtree(
      "<rect x='8' y='4' width='44' height='8' fill='red'/>"
      "<rect x='44' y='4' width='8' height='44' fill='red'/>",
      {}, Vector2i(64, 64));
  const auto actual = RenderDocumentWithBackend(actualDocument, ActiveRendererBackend());
  const auto expected = RenderDocumentWithBackend(expectedDocument, ActiveRendererBackend());
  editor::tests::CompareBitmapToBitmap(actual, expected, "clip_closure_preserves_open_stroke",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
