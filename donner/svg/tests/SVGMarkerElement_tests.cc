#include "donner/svg/SVGMarkerElement.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGPolygonElement.h"
#include "donner/svg/SVGPolylineElement.h"
#include "donner/svg/renderer/RendererSkia.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::Optional;

namespace donner::svg::tests {

namespace {

// Helper matchers for verifying marker attributes.
auto MarkerWidthEq(double expected) {
  return testing::Property("markerWidth", &SVGMarkerElement::markerWidth, Eq(expected));
}

auto MarkerHeightEq(double expected) {
  return testing::Property("markerHeight", &SVGMarkerElement::markerHeight, Eq(expected));
}

auto RefXEq(double expected) {
  return testing::Property("refX", &SVGMarkerElement::refX, Eq(expected));
}

auto RefYEq(double expected) {
  return testing::Property("refY", &SVGMarkerElement::refY, Eq(expected));
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
  marker->setMarkerWidth(15);
  marker->setMarkerHeight(20);
  marker->setRefX(7);
  marker->setRefY(8);
  marker->setOrient(MarkerOrient::AngleDegrees(45));  // Set to a fixed 45° rotation

  EXPECT_THAT(marker, ElementHasMarkerAttributes(AllOf(MarkerWidthEq(15), MarkerHeightEq(20),
                                                       RefXEq(7), RefYEq(8),
                                                       OrientEq(MarkerOrient::AngleDegrees(45)))));
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

  const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(document);
  EXPECT_TRUE(generatedAscii.matches(R"(
    ................
    ................
    ........@.......
    .......@@@......
    ......@@@@@.....
    .....@@@@@@@....
    ....@@@@@@@@@...
    .**@@@@@@@@@@@..
    .**@@@@@@@@@@@..
    ****@@@@@@@@@...
    .....@@@@@@@....
    ......@@@@@.....
    .......@@@......
    ........@.......
    ................
    ................
  )"));
}

/// Test that a marker defined in `<defs>` is applied at the midpoints of a path.
/// This test uses a polyline (with the same diamond points) so that the two
/// mid–points (namely at 8,2 and 14,8) receive markers.
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

  EXPECT_TRUE(generatedAscii.matches(R"(
    ......*.........
    ......***.......
    ......***.......
    ......*..@......
    .....@....@.....
    ....@......@....
    ...@........****
    ..@..........**.
    .............**.
    ............@...
    ...........@....
    ..........@.....
    .........@......
    ........@.......
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

  EXPECT_TRUE(generatedAscii.matches(R"(
    ......*.........
    ......***.......
    ......***.......
    ......*..@......
    .....@....@.....
    ....@......@....
    ...@........****
    ..@..........**.
    ..@..........**.
    ...@........@...
    ....@......@....
    .....@....@.....
    ......@..*......
    .......***......
    .......***......
    .........*......
  )"));
}

/// Test that a marker defined in `<defs>` is applied at the end of a path.
/// Here the same diamond–shaped polygon is used, so that the last point for the path close (2,8)
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

  EXPECT_TRUE(generatedAscii.matches(R"(
    ................
    ................
    ........@.......
    .......@@@......
    ......@@@@@.....
    .....@@@@@@@....
    ....@@@@@@@@@...
    .**@@@@@@@@@@@..
    .**@@@@@@@@@@@..
    ****@@@@@@@@@...
    .....@@@@@@@....
    ......@@@@@.....
    .......@@@......
    ........@.......
    ................
    ................
  )"));
}

}  // namespace donner::svg::tests
