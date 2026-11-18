#include "donner/svg/SVGMarkerElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGPolygonElement.h"
#include "donner/svg/SVGPolylineElement.h"
#include "donner/svg/core/PreserveAspectRatio.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::Optional;

namespace donner::svg::tests {

namespace {

// Helper matchers for verifying marker attributes. Lengths are stored as Lengthd; bare numbers in
// the tests are unitless (Lengthd::Unit::None).
auto MarkerWidthEq(double expected) {
  return testing::Property("markerWidth", &SVGMarkerElement::markerWidth,
                           Eq(Lengthd(expected, Lengthd::Unit::None)));
}

auto MarkerHeightEq(double expected) {
  return testing::Property("markerHeight", &SVGMarkerElement::markerHeight,
                           Eq(Lengthd(expected, Lengthd::Unit::None)));
}

auto RefXEq(double expected) {
  return testing::Property("refX", &SVGMarkerElement::refX,
                           Eq(Lengthd(expected, Lengthd::Unit::None)));
}

auto RefYEq(double expected) {
  return testing::Property("refY", &SVGMarkerElement::refY,
                           Eq(Lengthd(expected, Lengthd::Unit::None)));
}

auto OrientEq(MarkerOrient expected) {
  return testing::Property("orient", &SVGMarkerElement::orient, Eq(expected));
}

/// Matcher that extracts the element (wrapped in a ParsedFragment) and applies a given matcher.
MATCHER_P(ElementHasMarkerAttributes, matcher, "") {
  return testing::ExplainMatchResult(matcher, arg.element, result_listener);
}

}  // namespace

/// Test that a default `<marker>` element has the expected default attribute values.
/// (According to our implementation the defaults are assumed to be markerWidth=3, markerHeight=3,
/// refX=0, refY=0, and a fixed orientation of 0°.)
TEST(SVGMarkerElementTests, Defaults) {
  // Instantiate a marker element from an XML fragment.
  auto fragment = instantiateSubtreeElementAs<SVGMarkerElement>(R"(<marker />)");
  EXPECT_THAT(fragment, ElementHasMarkerAttributes(AllOf(MarkerWidthEq(3), MarkerHeightEq(3),
                                                         RefXEq(0), RefYEq(0),
                                                         OrientEq(MarkerOrient::AngleRadians(0)))));
}

/// Test that a <marker> element with explicitly provided attributes is parsed correctly.
TEST(SVGMarkerElementTests, Simple) {
  auto fragment = instantiateSubtreeElementAs<SVGMarkerElement>(R"(
      <marker markerWidth="10" markerHeight="10" refX="5" refY="5" orient="auto" />
  )");
  EXPECT_THAT(fragment,
              ElementHasMarkerAttributes(AllOf(MarkerWidthEq(10), MarkerHeightEq(10), RefXEq(5),
                                               RefYEq(5), OrientEq(MarkerOrient::Auto()))));
}

/// Test that updating attributes via setters correctly changes the element's state.
TEST(SVGMarkerElementTests, UpdateAttributes) {
  auto marker = instantiateSubtreeElementAs<SVGMarkerElement>(R"(
      <marker markerWidth="10" markerHeight="10" refX="5" refY="5" orient="auto" />
  )");

  // Update the attributes.
  marker->setMarkerWidth(Lengthd(15, Lengthd::Unit::None));
  marker->setMarkerHeight(Lengthd(20, Lengthd::Unit::None));
  marker->setRefX(Lengthd(7, Lengthd::Unit::None));
  marker->setRefY(Lengthd(8, Lengthd::Unit::None));
  marker->setOrient(MarkerOrient::AngleDegrees(45));  // Set to a fixed 45° rotation

  EXPECT_THAT(marker, ElementHasMarkerAttributes(AllOf(MarkerWidthEq(15), MarkerHeightEq(20),
                                                       RefXEq(7), RefYEq(8),
                                                       OrientEq(MarkerOrient::AngleDegrees(45)))));
}

TEST(SVGMarkerElementTests, ViewBoxPreserveAspectRatioAndMarkerUnits) {
  SVGDocument document;
  SVGMarkerElement marker = SVGMarkerElement::Create(document);

  const Box2d viewBox(Vector2d(1, 2), Vector2d(11, 22));
  marker.setViewBox(viewBox);
  marker.setPreserveAspectRatio(PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMid,
                                                    PreserveAspectRatio::MeetOrSlice::Slice});
  marker.setMarkerUnits(MarkerUnits::UserSpaceOnUse);

  EXPECT_THAT(marker.viewBox(), Optional(Eq(viewBox)));
  EXPECT_EQ(marker.preserveAspectRatio(),
            (PreserveAspectRatio{PreserveAspectRatio::Align::XMaxYMid,
                                 PreserveAspectRatio::MeetOrSlice::Slice}));
  EXPECT_EQ(marker.markerUnits(), MarkerUnits::UserSpaceOnUse);
}

/// Test that a marker defined in `<defs>` is applied at the start of a path.
/// The SVG uses a 16×16 coordinate system; the marker (a 4×4 triangle) is applied
/// to the first point (2,8) of a diamond-shaped polygon.
TEST(SVGMarkerElementTests, MarkerStartProperty) {
  SVGDocument document = instantiateSubtree(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <marker id="marker" markerWidth="4" markerHeight="4" refX="2" refY="2" orient="auto">
          <path d="M0,0 L4,2 L0,4 Z" fill="#AAA" />
        </marker>
      </defs>
      <polygon points="2,8 8,2 14,8 8,14" marker-start="url(#marker)" fill="white" />
    </svg>
  )-");

  // Per SVG 2 §11.6.2, the start marker on a closed polygon uses the
  // outgoing tangent (toward vertex 2) only - not a bisector with the
  // closing segment. Rotates the triangle to point up-right from (2,8).
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(document);
  EXPECT_TRUE(generatedAscii.matchBackend().defaultPattern(R"(
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    .::.............
    :::.............
    .:..............
    ................
    ................
    ................
    ................
    ................
    ................
  )"));
}

/// Test that a marker defined in `<defs>` is applied at the midpoints of a path.
/// This test uses a polyline (with the same diamond points) so that the two
/// mid-points (namely at 8,2 and 14,8) receive markers.
TEST(SVGMarkerElementTests, MarkerMidPropertyPolyline) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <marker id="marker" markerWidth="4" markerHeight="4" refX="2" refY="2" orient="auto">
          <path d="M0,0 L4,2 L0,4 Z" fill="#AAA" />
        </marker>
      </defs>
      <polyline points="2,8 8,2 14,8 8,14" marker-mid="url(#marker)" fill="none" stroke="white" />
    </svg>
  )-");

  EXPECT_TRUE(generatedAscii.matchBackend().defaultPattern(R"(
    ......:.........
    ......:::.......
    ......:::.......
    ......:.........
    ................
    ................
    ............::::
    .............::.
    .............::.
    ................
    ................
    ................
    ................
    ................
    ................
    ................
  )"));
}

/// Test that a marker defined is applied at the midpoints of a polygon, which should be the
/// everything besides the start/end of the closed shape (2,8).
TEST(SVGMarkerElementTests, MarkerMidPropertyPolygon) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <marker id="marker" markerWidth="4" markerHeight="4" refX="2" refY="2" orient="auto">
          <path d="M0,0 L4,2 L0,4 Z" fill="#AAA" />
        </marker>
      </defs>
      <polygon points="2,8 8,2 14,8 8,14" marker-mid="url(#marker)" fill="none" stroke="white" />
    </svg>
  )-");

  EXPECT_TRUE(generatedAscii.matchBackend().defaultPattern(R"(
    ......:.........
    ......:::.......
    ......:::.......
    ......:.........
    ................
    ................
    ............::::
    .............::.
    .............::.
    ................
    ................
    ................
    .........:......
    .......:::......
    .......:::......
    .........:......
  )"));
}

/// Test that a marker defined in `<defs>` is applied at the end of a path.
/// Here the same diamond-shaped polygon is used, so that the last point for the path close (2,8)
/// receives the marker.
TEST(SVGMarkerElementTests, MarkerEndProperty) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <marker id="marker" markerWidth="4" markerHeight="4" refX="2" refY="2" orient="auto">
          <path d="M0,0 L4,2 L0,4 Z" fill="#AAA" />
        </marker>
      </defs>
      <polygon points="2,8 8,2 14,8 8,14" marker-end="url(#marker)" fill="white" />
    </svg>
  )-");

  // Per SVG 2 §11.6.2, the end marker on a closed polygon uses the
  // incoming tangent (from vertex 3 toward 0) only - not a bisector
  // with the opening segment. Rotates the triangle to point down-right
  // from (2,8).
  EXPECT_TRUE(generatedAscii.matchBackend().defaultPattern(R"(
    ................
    ................
    ................
    ................
    ................
    ................
    ................
    .::.............
    .:::............
    ..:.............
    ................
    ................
    ................
    ................
    ................
    ................
  )"));
}

/// @test SVG2 §11.6.2: a marker's refX/refY percentage/keyword resolves against the marker's own
/// viewBox, not the referencing element's viewport. A `viewBox="0 0 4 4"` marker referenced from a
/// 16x16 root must place refX/refY="center" (50% of 4 = 2) identically to the explicit refX/refY=2
/// - even though the referencing viewport is 16 wide. Before the fix, "center" resolved to 50% of
/// 16 = 8, displacing the marker. Asserted differentially so the expected placement needs no
/// hand-derived golden.
TEST(SVGMarkerElementRenderingTests, RefKeywordResolvesAgainstMarkerViewBox) {
  const AsciiImage keyword = RendererTestUtils::renderToAsciiImage(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <marker id="m" viewBox="0 0 4 4" markerWidth="4" markerHeight="4"
                markerUnits="userSpaceOnUse" orient="0" refX="center" refY="center">
          <rect width="4" height="4" fill="black"/>
        </marker>
      </defs>
      <path d="M8,8 L14,8" stroke="white" fill="none" marker-start="url(#m)"/>
    </svg>
  )-");

  const AsciiImage explicitLength = RendererTestUtils::renderToAsciiImage(R"-(
    <svg viewBox="0 0 16 16">
      <defs>
        <marker id="m" viewBox="0 0 4 4" markerWidth="4" markerHeight="4"
                markerUnits="userSpaceOnUse" orient="0" refX="2" refY="2">
          <rect width="4" height="4" fill="black"/>
        </marker>
      </defs>
      <path d="M8,8 L14,8" stroke="white" fill="none" marker-start="url(#m)"/>
    </svg>
  )-");

  // Non-vacuous: the marker must actually render.
  EXPECT_NE(explicitLength.generated.find('@'), std::string::npos);
  // Keyword ("center" → 50% of the marker viewBox) must match the explicit-length placement.
  EXPECT_EQ(keyword.generated, explicitLength.generated);
}

/**
 * A marker whose content fills with a pattern draws that pattern only inside the marker, never as
 * ordinary content at the origin. A second path shares the marker (its branch is not inline for
 * that path), and the rect after both paths must still render.
 */
TEST(SVGMarkerElementRenderingTests, MarkerPatternContentStaysInsideTheMarker) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <path d="M 12 12 L 14 14" stroke="none" marker-start="url(#mk)" />
        <path d="M 4 4 L 6 6" stroke="none" marker-start="url(#mk)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ....@@@@........
        ....@@@@........
        ....@@@@........
        ....@@@@........
        ................
        ................
        ................
        ................
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        )"));
}

/**
 * The same with the pattern-filled rect inside a marker nested in the marker.
 */
TEST(SVGMarkerElementRenderingTests, NestedMarkerPatternContentStaysInsideTheMarker) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="inner" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <marker id="outer" markerUnits="userSpaceOnUse" markerWidth="8" markerHeight="8">
          <path d="M 0 0 L 1 1" stroke="none" marker-start="url(#inner)" />
        </marker>
        <path d="M 12 12 L 14 14" stroke="none" marker-start="url(#outer)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        )"));
}

/**
 * marker-mid and marker-end placements of a pattern-filled marker.
 */
TEST(SVGMarkerElementRenderingTests, MidAndEndMarkerPatternContentStaysInsideTheMarker) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <path d="M 4 1 L 12 1 L 12 12" fill="none" stroke="none" marker-mid="url(#mk)" marker-end="url(#mk)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ............@@@@
        ............@@@@
        ............@@@@
        ............@@@@
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        )"));
}

/**
 * Mask content inside a marker: the pattern-filled rect of the mask is drawn only into the mask.
 */
TEST(SVGMarkerElementRenderingTests, MarkerMaskContentStaysInsideTheMarker) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="white" />
        </pattern>
        <mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="4" height="4">
          <rect width="4" height="4" fill="url(#n)" />
        </mask>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="black" mask="url(#m)" />
        </marker>
        <path d="M 12 12 L 14 14" stroke="none" marker-start="url(#mk)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        )"));
}

/**
 * A marked path inside a pattern tile: the pattern content of the marker is drawn only inside the
 * marker.
 */
TEST(SVGMarkerElementRenderingTests, MarkerInsidePatternTileStaysInsideTheMarker) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <pattern id="tile" patternUnits="userSpaceOnUse" width="16" height="16">
          <path d="M 12 12 L 14 14" stroke="none" marker-start="url(#mk)" />
        </pattern>
        <rect width="16" height="16" fill="url(#tile)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        @@@@........@@@@
        )"));
}

/**
 * A one-command path draws no markers; its inline marker content, and the pattern content inside
 * it, must not be drawn as ordinary content either.
 */
TEST(SVGMarkerElementRenderingTests, OneCommandPathMarkerContentIsNotDrawn) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <path d="M 12 12" stroke="none" marker-start="url(#mk)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@............
        @@@@............
        @@@@............
        @@@@............
        )"));
}

/**
 * A path hidden by its filter draws no markers, and its marker content is not drawn as ordinary
 * content. The path is vertical, so its bounding box has no width and the filter region in
 * objectBoundingBox units is empty, which disables the element.
 */
TEST(SVGMarkerElementRenderingTests, FilterHiddenPathMarkerContentIsNotDrawn) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <filter id="empty">
          <feFlood flood-color="black" />
        </filter>
        <path d="M 12 12 L 12 14" stroke="none" filter="url(#empty)" marker-start="url(#mk)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@............
        @@@@............
        @@@@............
        @@@@............
        )"));
}

/**
 * A path with visibility hidden draws no markers, and its marker content is not drawn as ordinary
 * content.
 */
TEST(SVGMarkerElementRenderingTests, HiddenPathMarkerContentIsNotDrawn) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <path d="M 12 12 L 14 14" stroke="none" visibility="hidden" marker-start="url(#mk)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@............
        @@@@............
        @@@@............
        @@@@............
        )"));
}

/**
 * A path entirely off the canvas is culled and draws no markers; its marker content is not drawn as
 * ordinary content.
 */
TEST(SVGMarkerElementRenderingTests, CulledPathMarkerContentIsNotDrawn) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <pattern id="n" patternUnits="userSpaceOnUse" width="4" height="4">
          <rect width="4" height="4" fill="black" />
        </pattern>
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4">
          <rect width="4" height="4" fill="url(#n)" />
        </marker>
        <path d="M 100 100 L 102 102" stroke="none" marker-start="url(#mk)" />
        <rect y="12" width="4" height="4" fill="black" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        @@@@............
        @@@@............
        @@@@............
        @@@@............
        )"));
}

/**
 * A path just off the canvas whose marker reaches onto it: the marker must still be drawn, so the
 * path is not culled on its own bounds. refX="5" places the 4x4 marker content to the left of the
 * vertex at x = 18, covering x = 13 to 17.
 */
TEST(SVGMarkerElementRenderingTests, MarkerReachingOntoTheCanvasFromAnOffCanvasPathIsDrawn) {
  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
        <marker id="mk" markerUnits="userSpaceOnUse" markerWidth="4" markerHeight="4" refX="5">
          <rect width="4" height="4" fill="black" />
        </marker>
        <path d="M 18 12 L 19 13" stroke="none" marker-start="url(#mk)" />
        )-");

  EXPECT_TRUE(generatedAscii.matches(R"(
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        ................
        .............@@@
        .............@@@
        .............@@@
        .............@@@
        )"));
}

}  // namespace donner::svg::tests
