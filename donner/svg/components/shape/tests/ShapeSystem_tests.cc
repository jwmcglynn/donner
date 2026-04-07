/**
 * @file Tests for ShapeSystem: computed path generation from SVG shape elements.
 */

#include "donner/svg/components/shape/ShapeSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
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

TEST_F(ShapeSystemTest, PathFillIntersects) {
  auto document = ParseAndComputeShapes(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="r" x="10" y="10" width="80" height="80"/>
    </svg>
  )");

  auto element = document.querySelector("#r");
  ASSERT_TRUE(element.has_value());
  EXPECT_TRUE(shapeSystem.pathFillIntersects(element->entityHandle(), Vector2d(50, 50),
                                              FillRule::NonZero));
  EXPECT_FALSE(shapeSystem.pathFillIntersects(element->entityHandle(), Vector2d(5, 5),
                                               FillRule::NonZero));
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
  EXPECT_NEAR(bounds->topLeft.x, 25.0, 2.0);
  EXPECT_NEAR(bounds->topLeft.y, 25.0, 2.0);
  EXPECT_NEAR(bounds->bottomRight.x, 75.0, 2.0);
  EXPECT_NEAR(bounds->bottomRight.y, 75.0, 2.0);
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
  EXPECT_EQ(commands.back().type, PathSpline::CommandType::ClosePath);
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
  EXPECT_NE(commands.back().type, PathSpline::CommandType::ClosePath);
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

}  // namespace donner::svg::components
