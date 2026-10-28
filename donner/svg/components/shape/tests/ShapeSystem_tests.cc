/**
 * Tests for ShapeSystem: computed path generation from SVG shape elements.
 */

#include "donner/svg/components/shape/ShapeSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGPathElement.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Gt;
using testing::Lt;
using testing::NotNull;

namespace donner::svg::components {

class ShapeSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  /// Parse, compute styles, then compute shapes.
  SVGDocument ParseAndComputeShapes(std::string_view input) {
    auto document = ParseSVG(input);
    ParseWarningSink warningSink;
    StyleSystem().computeAllStyles(document.registry(), warningSink);
    shapeSystem.instantiateAllComputedPaths(document.registry(), warningSink);
    return document;
  }

  ShapeSystem shapeSystem;
};

TEST_F(ShapeSystemTest, InstanceSharesOnlyIdenticalAvailableGeometry) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r" width="4" height="4"/></svg>)");
  auto sourceElement = document.querySelector("#r");
  ASSERT_THAT(sourceElement, testing::Ne(std::nullopt));
  const EntityHandle source = sourceElement->entityHandle();
  auto& registry = document.registry();
  const EntityHandle instance(registry, registry.create());
  instance.emplace<ShadowEntityComponent>(source.entity());
  instance.emplace<ComputedInstanceGeometryComponent>();
  const Path sourcePath = source.get<ComputedPathComponent>().spline;
  instance.emplace<ComputedPathComponent>(sourcePath);
  EXPECT_EQ(shapeSystem.geometryHandle(instance).entity(), source.entity());

  const Path adjacentPath =
      PathBuilder().addRect(Box2d::FromXYWH(std::nextafter(0.0, 1.0), 0, 4, 4)).build();
  instance.emplace_or_replace<ComputedPathComponent>(adjacentPath);
  EXPECT_EQ(shapeSystem.geometryHandle(instance).entity(), instance.entity());

  instance.remove<ComputedPathComponent>();
  EXPECT_EQ(shapeSystem.geometryHandle(instance).entity(), instance.entity());
  instance.emplace<ComputedPathComponent>(sourcePath);
  source.remove<ComputedPathComponent>();
  EXPECT_EQ(shapeSystem.geometryHandle(instance).entity(), instance.entity());
}

TEST_F(ShapeSystemTest, CircleFillReferenceAdmitsPathScanBeforeBounds) {
  std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
    <path id="p" d="M0 0)";
  for (int x = 1; x <= 100; ++x) {
    svg += " L" + std::to_string(x) + " 2";
  }
  svg += R"("/></svg>)";
  auto document = ParseAndComputeShapes(svg);
  auto element = document.querySelector("#p");
  ASSERT_THAT(element, testing::Optional(testing::_));
  CircleClipPath circle;
  circle.box = CircleClipPath::Box::Fill;
  circle.radiusType = CircleClipPath::Radius::Length;
  circle.radius = Lengthd(1, Lengthd::Unit::None);

  // Enough for one entity and a circle, but insufficient for the 101-segment reference path.
  CircleClipGeometryBudget limited;
  limited.remainingWork = 64;
  EXPECT_THAT(shapeSystem.circleClipPath(element->entityHandle(), circle, limited).commands(),
              testing::IsEmpty());
  EXPECT_THAT(limited.rejected, testing::IsTrue());
  EXPECT_EQ(limited.remainingWork, 0u);
  limited.remainingWork = CircleClipGeometryBudget::kMaximumWork;
  EXPECT_THAT(shapeSystem.circleClipPath(element->entityHandle(), circle, limited).commands(),
              testing::IsEmpty());

  CircleClipGeometryBudget admitted;
  const Path clip = shapeSystem.circleClipPath(element->entityHandle(), circle, admitted);
  ASSERT_THAT(clip.commands(), testing::Not(testing::IsEmpty()));
  EXPECT_THAT(admitted.rejected, testing::IsFalse());
  EXPECT_THAT(clip.bounds(), BoxEq(Vector2d(49, 0), Vector2d(51, 2)));
}

TEST_F(ShapeSystemTest, PathClipAdmitsBeforeCopyAndClosesSeparateContours) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 48">
      <rect id="r" x="8" y="12" width="40" height="32"/>
    </svg>)");
  const auto element = document.querySelector("#r");
  ASSERT_THAT(element, testing::Optional(testing::_));
  PathClipPath path;
  path.box = RadialClipPath::Box::View;
  path.data = PathBuilder()
                  .moveTo({8, 8})
                  .lineTo({32, 8})
                  .lineTo({32, 32})
                  .moveTo({16, 12})
                  .lineTo({24, 12})
                  .lineTo({24, 20})
                  .build();
  const std::size_t work = 2 * path.data.points().size() + 2 * path.data.commands().size() + 1;
  CircleClipGeometryBudget limited;
  limited.remainingWork = work - 1;
  EXPECT_THAT(shapeSystem.pathClipPath(element->entityHandle(), path, limited).commands(),
              testing::IsEmpty());
  EXPECT_THAT(limited.rejected, testing::IsTrue());
  CircleClipGeometryBudget admitted;
  admitted.remainingWork = work;
  const Path clip = shapeSystem.pathClipPath(element->entityHandle(), path, admitted);
  EXPECT_THAT(clip.commands(),
              testing::ElementsAre(testing::Field(&Path::Command::verb, Path::Verb::MoveTo),
                                   testing::Field(&Path::Command::verb, Path::Verb::LineTo),
                                   testing::Field(&Path::Command::verb, Path::Verb::LineTo),
                                   testing::Field(&Path::Command::verb, Path::Verb::ClosePath),
                                   testing::Field(&Path::Command::verb, Path::Verb::MoveTo),
                                   testing::Field(&Path::Command::verb, Path::Verb::LineTo),
                                   testing::Field(&Path::Command::verb, Path::Verb::LineTo),
                                   testing::Field(&Path::Command::verb, Path::Verb::ClosePath)));
  EXPECT_THAT(admitted.rejected, testing::IsFalse());
  EXPECT_EQ(admitted.remainingWork, 0u);
}

TEST_F(ShapeSystemTest, PathClipRejectsNonFiniteTranslatedPointsAndExtents) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 48"><rect id="r" width="64" height="48"/></svg>)");
  const auto element = document.querySelector("#r");
  ASSERT_THAT(element, testing::Optional(testing::_));
  for (const double value :
       {1e308, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        double(std::numeric_limits<float>::max())}) {
    SCOPED_TRACE(value);
    PathClipPath path;
    path.box = RadialClipPath::Box::View;
    path.data =
        PathBuilder().moveTo({-value, 0}).curveTo({0, value}, {value, value}, {value, 0}).build();
    CircleClipGeometryBudget budget;
    EXPECT_THAT(shapeSystem.pathClipPath(element->entityHandle(), path, budget).commands(),
                testing::IsEmpty());
  }
}

TEST_F(ShapeSystemTest, InsetGeometryUsesAxisExtentsAndAdmitsBeforeAllocation) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 48">
      <rect id="r" width="64" height="48"/>
    </svg>)");
  const auto element = document.querySelector("#r");
  ASSERT_THAT(element, testing::Optional(testing::_));
  InsetClipPath inset;
  inset.box = RadialClipPath::Box::View;
  inset.offsets = {Lengthd(25, Lengthd::Unit::Percent), Lengthd(12.5, Lengthd::Unit::Percent),
                   Lengthd(25, Lengthd::Unit::Percent), Lengthd(12.5, Lengthd::Unit::Percent)};
  CircleClipGeometryBudget limited;
  limited.remainingWork = 8;
  EXPECT_THAT(shapeSystem.insetClipPath(element->entityHandle(), inset, limited).commands(),
              testing::IsEmpty());
  EXPECT_THAT(limited.rejected, testing::IsTrue());
  CircleClipGeometryBudget admitted;
  admitted.remainingWork = 9;
  const Path clip = shapeSystem.insetClipPath(element->entityHandle(), inset, admitted);
  ASSERT_THAT(clip.commands(), testing::Not(testing::IsEmpty()));
  EXPECT_THAT(clip.bounds(), BoxEq(Vector2d(8, 12), Vector2d(56, 36)));
  EXPECT_THAT(admitted.rejected, testing::IsFalse());
  EXPECT_EQ(admitted.remainingWork, 0u);
}

TEST_F(ShapeSystemTest, InsetOversubscriptionAndUnrepresentableCoordinatesAreEmpty) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 48">
      <rect id="r" width="64" height="48"/>
    </svg>)");
  const auto element = document.querySelector("#r");
  ASSERT_THAT(element, testing::Optional(testing::_));
  for (const auto value : {32.0, 1e308, -1e308, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
    SCOPED_TRACE(value);
    InsetClipPath inset;
    inset.box = RadialClipPath::Box::View;
    inset.offsets.fill(Lengthd(value));
    CircleClipGeometryBudget budget;
    EXPECT_THAT(shapeSystem.insetClipPath(element->entityHandle(), inset, budget).commands(),
                testing::IsEmpty());
  }
}

TEST_F(ShapeSystemTest, PolygonAdmissionAndNonFiniteCoordinatesFailClosed) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
      <rect id="r" width="64" height="64"/>
    </svg>)");
  const auto element = document.querySelector("#r");
  ASSERT_THAT(element, testing::Optional(testing::_));
  PolygonClipPath polygon;
  polygon.box = RadialClipPath::Box::View;
  polygon.vertices = {
      {Lengthd(0), Lengthd(0)}, {Lengthd(32), Lengthd(0)}, {Lengthd(0), Lengthd(32)}};
  CircleClipGeometryBudget limited;
  limited.remainingWork = 6;
  EXPECT_THAT(shapeSystem.polygonClipPath(element->entityHandle(), polygon, limited).commands(),
              testing::IsEmpty());
  EXPECT_THAT(limited.rejected, testing::IsTrue());
  CircleClipGeometryBudget admitted;
  EXPECT_THAT(shapeSystem.polygonClipPath(element->entityHandle(), polygon, admitted).commands(),
              testing::Not(testing::IsEmpty()));
  polygon.vertices[0].x = Lengthd(1e308);
  CircleClipGeometryBudget huge;
  EXPECT_THAT(shapeSystem.polygonClipPath(element->entityHandle(), polygon, huge).commands(),
              testing::IsEmpty());
}

TEST_F(ShapeSystemTest, EllipseRejectsNonFinitePathBuilderIntermediates) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <rect id="r" width="200" height="200"/>
    </svg>)");
  auto element = document.querySelector("#r");
  ASSERT_THAT(element, testing::Optional(testing::_));
  EllipseClipPath ellipse;
  ellipse.box = RadialClipPath::Box::View;
  ellipse.radiusXType = RadialClipPath::Radius::Length;
  ellipse.radiusYType = RadialClipPath::Radius::Length;
  ellipse.radiusY = Lengthd(1);
  for (const auto [center, radius] :
       {std::pair{0.0, 1e308}, std::pair{1e308, 1.0}, std::pair{0.0, -1.0}}) {
    ellipse.x.offset = Lengthd(center);
    ellipse.radiusX = Lengthd(radius);
    CircleClipGeometryBudget budget;
    EXPECT_THAT(shapeSystem.ellipseClipPath(element->entityHandle(), ellipse, budget).commands(),
                testing::IsEmpty());
  }
  ellipse.x.offset = Lengthd(10);
  ellipse.radiusX = Lengthd(2);
  CircleClipGeometryBudget budget;
  EXPECT_THAT(shapeSystem.ellipseClipPath(element->entityHandle(), ellipse, budget).commands(),
              testing::Not(testing::IsEmpty()));
}

TEST_F(ShapeSystemTest, ImageShadowReferenceUsesLightTypeAndOccurrenceTransform) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400">
      <image id="source" x="10" y="20" width="30" height="40"/>
      <g id="instance"/>
    </svg>)");
  auto sourceElement = document.querySelector("#source");
  auto instanceElement = document.querySelector("#instance");
  ASSERT_THAT(sourceElement, testing::Optional(testing::_));
  ASSERT_THAT(instanceElement, testing::Optional(testing::_));
  const EntityHandle source = sourceElement->entityHandle();
  const EntityHandle instance = instanceElement->entityHandle();
  source.emplace_or_replace<ComputedSizedElementComponent>(Box2d::FromXYWH(10, 20, 30, 40),
                                                           Box2d::WithSize(Vector2d(400, 400)));
  instance.remove<ElementTypeComponent>();
  instance.emplace<ShadowEntityComponent>(source.entity());
  instance.emplace_or_replace<ComputedAbsoluteTransformComponent>(Transform2d::Translate(100, 200),
                                                                  true);

  const auto bounds = shapeSystem.getShapeWorldBounds(instance);
  ASSERT_THAT(bounds, testing::Optional(testing::_));
  EXPECT_THAT(*bounds, BoxEq(Vector2d(110, 220), Vector2d(140, 260)));

  // An instance's own dimensions take precedence without changing the light-tree image.
  instance.emplace<ComputedSizedElementComponent>(Box2d::FromXYWH(2, 3, 12, 14), Box2d());
  const auto sizedBounds = shapeSystem.getShapeWorldBounds(instance);
  ASSERT_THAT(sizedBounds, testing::Optional(testing::_));
  EXPECT_THAT(*sizedBounds, BoxEq(Vector2d(102, 203), Vector2d(114, 217)));

  instance.emplace<ComputedShadowSizedElementComponent>(Box2d::FromXYWH(4, 5, 16, 18));
  const auto shadowBounds = shapeSystem.getShapeWorldBounds(instance);
  ASSERT_THAT(shadowBounds, testing::Optional(testing::_));
  EXPECT_THAT(*shadowBounds, BoxEq(Vector2d(104, 205), Vector2d(120, 223)));
  EXPECT_THAT(source.get<ComputedSizedElementComponent>().bounds,
              BoxEq(Vector2d(10, 20), Vector2d(40, 60)));
}

// --- Circle ---

TEST_F(ShapeSystemTest, CircleComputedPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <circle id="c" cx="50" cy="50" r="25"/>
    </svg>
  )");

  auto element = document.querySelector("#c");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

TEST_F(ShapeSystemTest, CircleZeroRadius) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <circle id="c" cx="50" cy="50" r="0"/>
    </svg>
  )");

  auto element = document.querySelector("#c");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);
}

TEST_F(ShapeSystemTest, CircleNegativeRadius) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <circle id="c" cx="50" cy="50" r="-10"/>
    </svg>
  )");

  auto element = document.querySelector("#c");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);
}

// --- Ellipse ---

TEST_F(ShapeSystemTest, EllipseComputedPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <ellipse id="e" cx="50" cy="50" rx="30" ry="20"/>
    </svg>
  )");

  auto element = document.querySelector("#e");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

TEST_F(ShapeSystemTest, EllipseZeroRadii) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <ellipse id="e" cx="50" cy="50" rx="0" ry="0"/>
    </svg>
  )");

  auto element = document.querySelector("#e");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);
}

// --- Rect ---

TEST_F(ShapeSystemTest, RectComputedPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="60"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

TEST_F(ShapeSystemTest, RectRoundedCorners) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="60" rx="5" ry="5"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  // Rounded corners produce curves, so the spline should have CurveTo commands.
  EXPECT_GT(path->spline.commands().size(), 4u);
}

TEST_F(ShapeSystemTest, RectZeroSize) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="0" height="0"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);
}

TEST_F(ShapeSystemTest, RectZeroWidth) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="0" height="50"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);
}

// --- Line ---

TEST_F(ShapeSystemTest, LineComputedPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <line id="l" x1="10" y1="10" x2="90" y2="90"/>
    </svg>
  )");

  auto element = document.querySelector("#l");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

// --- Path ---

TEST_F(ShapeSystemTest, PathComputedPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 10 L90 90 Z"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

TEST_F(ShapeSystemTest, PathEmptyD) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d=""/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);
}

// --- Polygon ---

TEST_F(ShapeSystemTest, PolygonComputedPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <polygon id="p" points="50,5 90,90 10,90"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

// --- Polyline ---

TEST_F(ShapeSystemTest, PolylineComputedPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <polyline id="p" points="10,10 50,50 90,10"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

// --- Bounds and intersection ---

TEST_F(ShapeSystemTest, GetShapeBounds) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="20" width="30" height="40"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, 10.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 20.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.x, 40.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.y, 60.0, 1.0);
}

TEST_F(ShapeSystemTest, GetShapeBoundsDisplayNone) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="20" width="30" height="40" style="display:none"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  EXPECT_FALSE(bounds.has_value());
}

TEST_F(ShapeSystemTest, GetShapeBoundsDisplayNoneWithoutPrecomputedStyle) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="20" width="30" height="40" style="display:none"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  EXPECT_FALSE(bounds.has_value());
}

TEST_F(ShapeSystemTest, GetShapeBoundsComputesEmptyStylePlaceholder) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="20" width="30" height="40"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  element->entityHandle().emplace_or_replace<ComputedStyleComponent>();

  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, 10.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 20.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.x, 40.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.y, 60.0, 1.0);

  const auto* style = element->entityHandle().try_get<ComputedStyleComponent>();
  ASSERT_THAT(style, NotNull());
  EXPECT_TRUE(style->properties.has_value());
}

TEST_F(ShapeSystemTest, PathFillIntersects) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="80"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  EXPECT_TRUE(
      shapeSystem.pathFillIntersects(element->entityHandle(), Vector2d(50, 50), FillRule::NonZero));
  EXPECT_FALSE(
      shapeSystem.pathFillIntersects(element->entityHandle(), Vector2d(5, 5), FillRule::NonZero));
}

TEST_F(ShapeSystemTest, PathStrokeIntersects) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <line id="l" x1="10" y1="50" x2="90" y2="50"/>
    </svg>
  )");

  auto element = document.querySelector("#l");
  ASSERT_TRUE(element.has_value());
  // Point on the line with a stroke width of 4.
  EXPECT_TRUE(shapeSystem.pathStrokeIntersects(element->entityHandle(), Vector2d(50, 50), 4.0));
  // Point far from the line.
  EXPECT_FALSE(shapeSystem.pathStrokeIntersects(element->entityHandle(), Vector2d(50, 90), 4.0));
}

// --- Group bounds ---

TEST_F(ShapeSystemTest, GetShapeBoundsGroupChildren) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">
      <g id="grp">
        <rect x="10" y="10" width="20" height="20"/>
        <rect x="50" y="50" width="20" height="20"/>
      </g>
    </svg>
  )");

  auto element = document.querySelector("#grp");
  ASSERT_TRUE(element.has_value());
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  ASSERT_TRUE(bounds.has_value());
  // Group bounds should encompass both rects.
  EXPECT_NEAR(bounds->topLeft.x, 10.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 10.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.x, 70.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.y, 70.0, 1.0);
}

// --- CSS units ---

TEST_F(ShapeSystemTest, RectWithPercentageUnits) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100">
      <rect id="r" x="10%" y="20%" width="50%" height="50%"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

// --- Self-intersecting path ---

TEST_F(ShapeSystemTest, SelfIntersectingPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90 L90 10 L10 90 Z"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());

  // Verify bounds encompass the full path.
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, 10.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 10.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.x, 90.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.y, 90.0, 1.0);
}

// --- Arc path edge cases ---

TEST_F(ShapeSystemTest, ArcZeroRadius) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 A0 0 0 0 1 50 50"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());

  // Zero-radius arc degenerates to a line; bounds should still be valid.
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  ASSERT_TRUE(bounds.has_value());
  EXPECT_THAT(bounds->topLeft.x, Lt(bounds->bottomRight.x + 1.0));
  EXPECT_THAT(bounds->topLeft.y, Lt(bounds->bottomRight.y + 1.0));
}

TEST_F(ShapeSystemTest, Arc360Degrees) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M50 25 A25 25 0 1 1 50 75 A25 25 0 1 1 50 25"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());

  // Full circle arc should produce valid bounds.
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  ASSERT_TRUE(bounds.has_value());
  // Proper SVG arc decomposition gives accurate bounds for the full circle.
  EXPECT_NEAR(bounds->topLeft.x, 25.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 25.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.x, 75.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.y, 75.0, 1.0);
}

TEST_F(ShapeSystemTest, ArcVerySmall) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M50 50 A0.001 0.001 0 0 1 50.001 50.001"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());
}

// --- Polyline with duplicate consecutive points ---

TEST_F(ShapeSystemTest, PolylineDuplicateConsecutivePoints) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <polyline id="p" points="10,10 10,10 20,20"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());

  // Should have MoveTo + 2 LineTo commands despite duplicate point.
  EXPECT_EQ(path->spline.commands().size(), 3u);
}

// --- Polygon closure ---

TEST_F(ShapeSystemTest, PolygonIsClosed) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <polygon id="p" points="10,10 90,10 50,90"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());

  // Polygon spline must end with a ClosePath command.
  const auto& commands = path->spline.commands();
  ASSERT_FALSE(commands.empty());
  EXPECT_EQ(commands.back().verb, Path::Verb::ClosePath);
}

TEST_F(ShapeSystemTest, PolylineIsNotClosed) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <polyline id="p" points="10,10 90,10 50,90"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());

  // Polyline spline must NOT end with a ClosePath command.
  const auto& commands = path->spline.commands();
  ASSERT_FALSE(commands.empty());
  EXPECT_NE(commands.back().verb, Path::Verb::ClosePath);
}

// --- Rect with rx/ry exceeding half dimensions ---

TEST_F(ShapeSystemTest, RectRadiusClamped) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="40" height="20" rx="30" ry="15"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  ASSERT_THAT(path, NotNull());
  EXPECT_FALSE(path->spline.empty());

  // rx=30 should be clamped to width/2=20, ry=15 should be clamped to height/2=10.
  // Bounds should match the rect position and size exactly.
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  ASSERT_TRUE(bounds.has_value());
  EXPECT_NEAR(bounds->topLeft.x, 10.0, 1.0);
  EXPECT_NEAR(bounds->topLeft.y, 10.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.x, 50.0, 1.0);
  EXPECT_NEAR(bounds->bottomRight.y, 30.0, 1.0);
}

// --- Circle with zero radius ---

TEST_F(ShapeSystemTest, CircleZeroRadiusProducesNoPath) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <circle id="c" cx="10" cy="10" r="0"/>
    </svg>
  )");

  auto element = document.querySelector("#c");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);

  // Bounds should not exist for a zero-radius circle.
  auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
  EXPECT_FALSE(bounds.has_value());
}

// --- Ellipse with one zero radius ---

TEST_F(ShapeSystemTest, EllipseOneZeroRadius) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <ellipse id="e" cx="10" cy="10" rx="5" ry="0"/>
    </svg>
  )");

  auto element = document.querySelector("#e");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  // Per SVG spec, if either rx or ry is zero, the ellipse is not rendered.
  EXPECT_EQ(path, nullptr);
}

TEST_F(ShapeSystemTest, EllipseOtherZeroRadius) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <ellipse id="e" cx="10" cy="10" rx="0" ry="5"/>
    </svg>
  )");

  auto element = document.querySelector("#e");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  EXPECT_EQ(path, nullptr);
}

// --- Path with only moveTo ---

TEST_F(ShapeSystemTest, PathOnlyMoveTo) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M 10 10"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  // A path with only a moveTo and no drawing commands produces no renderable geometry.
  // The implementation may either return nullptr or a spline with a single MoveTo.
  if (path) {
    // If a spline exists, bounds should still be valid (degenerate point).
    auto bounds = shapeSystem.getShapeBounds(element->entityHandle());
    if (bounds.has_value()) {
      EXPECT_NEAR(bounds->topLeft.x, 10.0, 1.0);
      EXPECT_NEAR(bounds->topLeft.y, 10.0, 1.0);
    }
  }
}

TEST_F(ShapeSystemTest, PathMultipleMoveTo) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M 10 10 M 50 50"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  // Multiple moveTo without drawing commands should not crash.
  auto* path = element->entityHandle().try_get<ComputedPathComponent>();
  if (path) {
    EXPECT_FALSE(path->spline.empty());
  }
}

// --- Path data parse caching ---

namespace {

/// A spline that no shape in these tests produces. Writing it over a retained
/// `ComputedPathComponent` makes it observable whether the next shape pass rewrote the component
/// (marker gone, so the path data was parsed again) or reused it as-is (marker still present).
Path MarkerSpline() {
  return PathBuilder()
      .moveTo(Vector2d(-1234.0, -5678.0))
      .lineTo(Vector2d(-8765.0, -4321.0))
      .build();
}

}  // namespace

/**
 * Path data is parsed once and the parse is skipped while the resolved path-data string is
 * unchanged, so every way that string can change has to be shown to reach the parser again.
 */
class ShapeSystemPathCacheTest : public ShapeSystemTest {
protected:
  /// Run the style and shape passes again, the way a repeated render does.
  void recomputeShapes(SVGDocument& document) {
    StyleSystem().computeAllStyles(document.registry(), warningSink_);
    shapeSystem.instantiateAllComputedPaths(document.registry(), warningSink_);
  }

  static ComputedPathComponent* computedPath(SVGElement element) {
    return element.entityHandle().try_get<ComputedPathComponent>();
  }

  /// Overwrite the retained spline with \ref MarkerSpline.
  void markComputedPath(SVGElement element) {
    ComputedPathComponent* computed = computedPath(element);
    ASSERT_THAT(computed, NotNull());
    computed->spline = MarkerSpline();
    computed->cachedLocalBounds.reset();
  }

  static bool isMarked(SVGElement element) {
    const ComputedPathComponent* computed = computedPath(element);
    return computed != nullptr && computed->spline == MarkerSpline();
  }

  /// The spline that `#p` gets in a document parsed from scratch, which is what a mutated
  /// document has to converge on. Comparing against a separately parsed document is what makes
  /// the check meaningful: the retained parse lives on the mutated document, so only a document
  /// that never saw the mutation can show what the new value is supposed to produce.
  Path freshSpline(std::string_view svg) {
    SVGDocument fresh = ParseAndComputeShapes(svg);
    std::optional<SVGElement> element = fresh.querySelector("#p");
    EXPECT_TRUE(element.has_value());
    if (!element) {
      return Path();
    }

    const ComputedPathComponent* computed = computedPath(element.value());
    EXPECT_THAT(computed, NotNull());
    return computed ? computed->spline : Path();
  }

  ParseWarningSink warningSink_;
};

TEST_F(ShapeSystemPathCacheTest, UnchangedPathDataIsNotReparsed) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90 L90 10 Z"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  ASSERT_NO_FATAL_FAILURE(markComputedPath(element.value()));

  recomputeShapes(document);

  // Nothing changed, so the retained component has to be returned untouched instead of being
  // rebuilt from a fresh parse of the same string.
  EXPECT_TRUE(isMarked(element.value()));
}

TEST_F(ShapeSystemPathCacheTest, SetDReparses) {
  constexpr std::string_view kBefore = R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90"/>
    </svg>
  )";
  constexpr std::string_view kAfter = R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M20 20 L80 40 L60 80 Z"/>
    </svg>
  )";

  const Path expected = freshSpline(kAfter);
  ASSERT_FALSE(expected == freshSpline(kBefore));

  auto document = ParseAndComputeShapes(kBefore);
  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  ASSERT_NO_FATAL_FAILURE(markComputedPath(element.value()));

  element->cast<SVGPathElement>().setD(RcString("M20 20 L80 40 L60 80 Z"));
  recomputeShapes(document);

  EXPECT_FALSE(isMarked(element.value()));
  ASSERT_THAT(computedPath(element.value()), NotNull());
  EXPECT_TRUE(computedPath(element.value())->spline == expected);
}

TEST_F(ShapeSystemPathCacheTest, SetAttributeDReparses) {
  constexpr std::string_view kBefore = R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90"/>
    </svg>
  )";
  constexpr std::string_view kAfter = R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M30 30 L70 30 L70 70 Z"/>
    </svg>
  )";

  const Path expected = freshSpline(kAfter);
  ASSERT_FALSE(expected == freshSpline(kBefore));

  auto document = ParseAndComputeShapes(kBefore);
  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  ASSERT_NO_FATAL_FAILURE(markComputedPath(element.value()));

  element->setAttribute("d", "M30 30 L70 30 L70 70 Z");
  recomputeShapes(document);

  EXPECT_FALSE(isMarked(element.value()));
  ASSERT_THAT(computedPath(element.value()), NotNull());
  EXPECT_TRUE(computedPath(element.value())->spline == expected);
}

TEST_F(ShapeSystemPathCacheTest, RemoveAttributeDDropsGeometry) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90"/>
    </svg>
  )");

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  ASSERT_NO_FATAL_FAILURE(markComputedPath(element.value()));

  element->removeAttribute("d");
  recomputeShapes(document);

  // A path with no path data has no geometry at all, matching a document parsed without a `d`
  // attribute.
  EXPECT_THAT(computedPath(element.value()), testing::IsNull());
}

TEST_F(ShapeSystemPathCacheTest, StyleAttributeDReparses) {
  constexpr std::string_view kBefore = R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90"/>
    </svg>
  )";
  constexpr std::string_view kAfter = R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90" style="d: 'M40 40 L60 40 L60 60 Z'"/>
    </svg>
  )";

  const Path expected = freshSpline(kAfter);
  ASSERT_FALSE(expected == freshSpline(kBefore));

  auto document = ParseAndComputeShapes(kBefore);
  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  ASSERT_NO_FATAL_FAILURE(markComputedPath(element.value()));

  // A CSS `d` declaration is resolved by the shape pass itself and does not drop the computed
  // path, so this is the mutation class the retained parse has to notice on its own rather than
  // through an invalidation.
  element->setStyle("d: 'M40 40 L60 40 L60 60 Z'");
  recomputeShapes(document);

  EXPECT_FALSE(isMarked(element.value()));
  ASSERT_THAT(computedPath(element.value()), NotNull());
  EXPECT_TRUE(computedPath(element.value())->spline == expected);
}

TEST_F(ShapeSystemPathCacheTest, SplineOverrideAndBackReparses) {
  constexpr std::string_view kSvg = R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90"/>
    </svg>
  )";

  const Path expected = freshSpline(kSvg);
  const Path overrideSpline =
      PathBuilder().moveTo(Vector2d(5.0, 5.0)).lineTo(Vector2d(25.0, 45.0)).build();
  ASSERT_FALSE(expected == overrideSpline);

  auto document = ParseAndComputeShapes(kSvg);
  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());

  element->cast<SVGPathElement>().setSpline(overrideSpline);
  recomputeShapes(document);

  ASSERT_THAT(computedPath(element.value()), NotNull());
  EXPECT_TRUE(computedPath(element.value())->spline == overrideSpline);

  // Going back to path data has to parse again: the spline that is currently retained was never
  // produced by parsing a string, so it cannot be mistaken for a cached parse of one.
  element->cast<SVGPathElement>().setD(RcString("M10 10 L90 90"));
  recomputeShapes(document);

  ASSERT_THAT(computedPath(element.value()), NotNull());
  EXPECT_TRUE(computedPath(element.value())->spline == expected);
}

TEST_F(ShapeSystemPathCacheTest, MalformedPathDataReportsDiagnosticOnEveryPass) {
  // Trailing `Q` with no coordinates: the parser returns the commands it did read plus a
  // diagnostic, so there is a usable spline and a warning at the same time.
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <path id="p" d="M10 10 L90 90 Q"/>
    </svg>
  )");

  ParseWarningSink firstSink;
  StyleSystem().computeAllStyles(document.registry(), firstSink);
  shapeSystem.instantiateAllComputedPaths(document.registry(), firstSink);
  EXPECT_TRUE(firstSink.hasWarnings());

  auto element = document.querySelector("#p");
  ASSERT_TRUE(element.has_value());
  ASSERT_THAT(computedPath(element.value()), NotNull());

  // The diagnostic has to keep reaching the sink for as long as the malformed data is present,
  // so path data that did not parse cleanly is never treated as a settled parse.
  ParseWarningSink secondSink;
  StyleSystem().computeAllStyles(document.registry(), secondSink);
  shapeSystem.instantiateAllComputedPaths(document.registry(), secondSink);
  EXPECT_TRUE(secondSink.hasWarnings());
}

}  // namespace donner::svg::components
