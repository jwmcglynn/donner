#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGPolygonElement.h"
#include "donner/svg/SVGPolylineElement.h"
#include "donner/svg/core/tests/PathTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::ElementsAre;
using testing::Ne;

namespace donner::svg {
namespace {

using Command = Path::Command;
using CommandType = Path::Verb;

MATCHER_P(ComputedSplineIs, matchers, "") {
  const auto maybeSpline = arg.element.computedSpline();
  if (!maybeSpline) {
    *result_listener << "spline is empty";
    return false;
  }
  return testing::ExplainMatchResult(matchers, *maybeSpline, result_listener);
}

TEST(SVGPolygonPolylineElementTests, PolygonDefaultsAndSetPoints) {
  SVGDocument document;
  SVGPolygonElement polygon = SVGPolygonElement::Create(document);

  EXPECT_TRUE(polygon.points().empty());
  polygon.setPoints({Vector2d(1, 2), Vector2d(3, 4), Vector2d(5, 6)});
  EXPECT_THAT(polygon.points(), ElementsAre(Vector2d(1, 2), Vector2d(3, 4), Vector2d(5, 6)));
  EXPECT_THAT(polygon.tryCast<SVGGeometryElement>(), Ne(std::nullopt));
}

TEST(SVGPolygonPolylineElementTests, PolylineDefaultsAndSetPoints) {
  SVGDocument document;
  SVGPolylineElement polyline = SVGPolylineElement::Create(document);

  EXPECT_TRUE(polyline.points().empty());
  polyline.setPoints({Vector2d(1, 2), Vector2d(3, 4), Vector2d(5, 6)});
  EXPECT_THAT(polyline.points(), ElementsAre(Vector2d(1, 2), Vector2d(3, 4), Vector2d(5, 6)));
  EXPECT_THAT(polyline.tryCast<SVGGeometryElement>(), Ne(std::nullopt));
}

TEST(SVGPolygonPolylineElementTests, GenericPointsAttributePopulatesDetachedPolygon) {
  SVGDocument document;
  auto polygon = SVGPolygonElement::Create(document);
  polygon.setAttribute("points", "10,10 40,10 10,40");
  EXPECT_THAT(polygon.points(), ElementsAre(Vector2d(10, 10), Vector2d(40, 10), Vector2d(10, 40)));
}

TEST(SVGPolygonPolylineElementTests, GenericPointsAttributePopulatesDetachedPolyline) {
  SVGDocument document;
  auto polyline = SVGPolylineElement::Create(document);
  polyline.setAttribute("points", "10,10 40,10 10,40");
  EXPECT_THAT(polyline.points(), ElementsAre(Vector2d(10, 10), Vector2d(40, 10), Vector2d(10, 40)));
}

TEST(SVGPolygonPolylineElementTests, GenericPointsMutationUpdatesAndRemovesParsedGeometry) {
  auto parsed =
      instantiateSubtreeElementAs<SVGPolygonElement>(R"(<polygon points="0,0 20,0 0,20"/>)");
  auto& polygon = parsed.element;
  (void)polygon.computedSpline();
  polygon.setAttribute("points", "10,10 40,10 10,40");
  EXPECT_THAT(polygon.points(), ElementsAre(Vector2d(10, 10), Vector2d(40, 10), Vector2d(10, 40)));
  polygon.removeAttribute("points");
  EXPECT_THAT(polygon.points(), testing::IsEmpty());
}

TEST(SVGPolygonPolylineElementTests, GenericPointsMutationReportsErrorsAndKeepsValidPrefix) {
  SVGDocument document;
  auto polyline = SVGPolylineElement::Create(document);
  auto result = document.setElementAttribute(polyline, "points", "10,10 40,10 20,");
  EXPECT_THAT(result.diagnostic, Ne(std::nullopt));
  EXPECT_THAT(polyline.points(), ElementsAre(Vector2d(10, 10), Vector2d(40, 10)));
  polyline.removeAttribute("points");
  EXPECT_THAT(polyline.points(), testing::IsEmpty());
}

TEST(SVGPolygonPolylineElementTests, PolygonComputedSplineClosesPath) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGPolygonElement>(R"(<polygon points="10,10 20,20 30,10"/>)"),
      ComputedSplineIs(PointsAndCommandsAre(
          ElementsAre(Vector2d(10, 10), Vector2d(20, 20), Vector2d(30, 10)),
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                      Command{CommandType::LineTo, 2}, Command{CommandType::ClosePath, 0}))));
}

TEST(SVGPolygonPolylineElementTests, PolylineComputedSplineStaysOpen) {
  EXPECT_THAT(
      instantiateSubtreeElementAs<SVGPolylineElement>(R"(<polyline points="10,10 20,20 30,10"/>)"),
      ComputedSplineIs(PointsAndCommandsAre(
          ElementsAre(Vector2d(10, 10), Vector2d(20, 20), Vector2d(30, 10)),
          ElementsAre(Command{CommandType::MoveTo, 0}, Command{CommandType::LineTo, 1},
                      Command{CommandType::LineTo, 2}))));
}

}  // namespace
}  // namespace donner::svg
