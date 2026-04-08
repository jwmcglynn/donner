#include "donner/base/Path.h"

#include <gtest/gtest.h>

#include <sstream>

#include "donner/base/MathUtils.h"

namespace donner {

namespace {

void ExpectNear(const Vector2d& actual, const Vector2d& expected, double tolerance = 1e-9) {
  EXPECT_NEAR(actual.x, expected.x, tolerance) << "actual=" << actual << " expected=" << expected;
  EXPECT_NEAR(actual.y, expected.y, tolerance) << "actual=" << actual << " expected=" << expected;
}

}  // namespace

// =============================================================================
// PathBuilder basics
// =============================================================================

TEST(PathBuilder, EmptyBuilder) {
  PathBuilder builder;
  EXPECT_TRUE(builder.empty());

  Path path = builder.build();
  EXPECT_TRUE(path.empty());
  EXPECT_EQ(path.verbCount(), 0u);
}

TEST(PathBuilder, MoveToLineTo) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({100, 0})
                  .lineTo({100, 100})
                  .build();

  EXPECT_EQ(path.verbCount(), 3u);
  EXPECT_EQ(path.points().size(), 3u);
  EXPECT_EQ(path.commands()[0].verb, Path::Verb::MoveTo);
  EXPECT_EQ(path.commands()[1].verb, Path::Verb::LineTo);
  EXPECT_EQ(path.commands()[2].verb, Path::Verb::LineTo);
}

TEST(PathBuilder, QuadTo) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .quadTo({50, 100}, {100, 0})
                  .build();

  EXPECT_EQ(path.verbCount(), 2u);
  EXPECT_EQ(path.commands()[1].verb, Path::Verb::QuadTo);
  // QuadTo consumes 2 points (control + end).
  EXPECT_EQ(path.points().size(), 3u);  // 1 moveTo + 2 quadTo
}

TEST(PathBuilder, CurveTo) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 100}, {100, 100}, {100, 0})
                  .build();

  EXPECT_EQ(path.verbCount(), 2u);
  EXPECT_EQ(path.commands()[1].verb, Path::Verb::CurveTo);
  EXPECT_EQ(path.points().size(), 4u);  // 1 moveTo + 3 curveTo
}

TEST(PathBuilder, ClosePath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({100, 0})
                  .lineTo({100, 100})
                  .closePath()
                  .build();

  EXPECT_EQ(path.verbCount(), 4u);
  EXPECT_EQ(path.commands()[3].verb, Path::Verb::ClosePath);
}

TEST(PathBuilder, BuildResetsState) {
  PathBuilder builder;
  builder.moveTo({0, 0}).lineTo({1, 1});
  Path path1 = builder.build();

  EXPECT_TRUE(builder.empty());
  Path path2 = builder.build();
  EXPECT_TRUE(path2.empty());
  EXPECT_FALSE(path1.empty());
}

TEST(PathBuilder, ImplicitMoveToOnLineTo) {
  // lineTo without moveTo should auto-insert moveTo at (0,0).
  Path path = PathBuilder().lineTo({100, 0}).build();

  EXPECT_EQ(path.verbCount(), 2u);
  EXPECT_EQ(path.commands()[0].verb, Path::Verb::MoveTo);
  EXPECT_EQ(path.commands()[1].verb, Path::Verb::LineTo);
  ExpectNear(path.points()[0], Vector2d(0, 0));
}

TEST(PathBuilder, CurrentPoint) {
  PathBuilder builder;
  ExpectNear(builder.currentPoint(), Vector2d(0, 0));

  builder.moveTo({10, 20});
  ExpectNear(builder.currentPoint(), Vector2d(10, 20));

  builder.lineTo({30, 40});
  ExpectNear(builder.currentPoint(), Vector2d(30, 40));
}

// =============================================================================
// Shape helpers
// =============================================================================

TEST(PathBuilder, AddRect) {
  Path path = PathBuilder().addRect(Box2d(Vector2d(0, 0), Vector2d(10, 10))).build();

  // moveTo + 3 lineTo + closePath = 5 commands
  EXPECT_EQ(path.verbCount(), 5u);
  EXPECT_EQ(path.commands()[0].verb, Path::Verb::MoveTo);
  EXPECT_EQ(path.commands()[4].verb, Path::Verb::ClosePath);
}

TEST(PathBuilder, AddCircle) {
  Path path = PathBuilder().addCircle({50, 50}, 25).build();

  // moveTo + 4 curveTo + closePath = 6 commands
  EXPECT_EQ(path.verbCount(), 6u);
}

TEST(PathBuilder, AddEllipse) {
  Path path = PathBuilder()
                  .addEllipse(Box2d(Vector2d(0, 0), Vector2d(100, 50)))
                  .build();

  EXPECT_EQ(path.verbCount(), 6u);  // moveTo + 4 curveTo + closePath
}

TEST(PathBuilder, AddPath) {
  Path sub = PathBuilder().moveTo({0, 0}).lineTo({10, 10}).build();
  Path combined = PathBuilder().moveTo({-5, -5}).addPath(sub).build();

  // moveTo(-5,-5) + moveTo(0,0) + lineTo(10,10) = 3
  EXPECT_EQ(combined.verbCount(), 3u);
}

// =============================================================================
// Path::bounds
// =============================================================================

TEST(Path, BoundsEmpty) {
  Path path;
  Box2d box = path.bounds();
  EXPECT_NEAR(box.width(), 0.0, 1e-9);
  EXPECT_NEAR(box.height(), 0.0, 1e-9);
}

TEST(Path, BoundsLinePath) {
  Path path = PathBuilder()
                  .moveTo({10, 20})
                  .lineTo({50, 80})
                  .lineTo({30, 40})
                  .build();

  Box2d box = path.bounds();
  EXPECT_NEAR(box.topLeft.x, 10.0, 1e-9);
  EXPECT_NEAR(box.topLeft.y, 20.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.x, 50.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.y, 80.0, 1e-9);
}

TEST(Path, BoundsCurvePath) {
  // Cubic arch: control points above the line.
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 100}, {100, 100}, {100, 0})
                  .build();

  Box2d box = path.bounds();
  // The curve peaks at ~75% of the control point height.
  EXPECT_NEAR(box.topLeft.x, 0.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.x, 100.0, 1e-9);
  EXPECT_GT(box.bottomRight.y, 50.0);  // Curve goes above y=0.
  EXPECT_LT(box.bottomRight.y, 100.0);  // But not as high as control points.
}

TEST(Path, BoundsQuadPath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .quadTo({50, 100}, {100, 0})
                  .build();

  Box2d box = path.bounds();
  EXPECT_NEAR(box.topLeft.x, 0.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.x, 100.0, 1e-9);
  EXPECT_GT(box.bottomRight.y, 0.0);
  EXPECT_LT(box.bottomRight.y, 100.0);
}

// =============================================================================
// Path::forEach
// =============================================================================

TEST(Path, ForEachVisitsAllCommands) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .quadTo({15, 5}, {20, 0})
                  .curveTo({25, 10}, {30, 10}, {35, 0})
                  .closePath()
                  .build();

  std::vector<Path::Verb> verbs;
  std::vector<size_t> pointCounts;

  path.forEach([&](Path::Verb verb, std::span<const Vector2d> pts) {
    verbs.push_back(verb);
    pointCounts.push_back(pts.size());
  });

  ASSERT_EQ(verbs.size(), 5u);
  EXPECT_EQ(verbs[0], Path::Verb::MoveTo);
  EXPECT_EQ(verbs[1], Path::Verb::LineTo);
  EXPECT_EQ(verbs[2], Path::Verb::QuadTo);
  EXPECT_EQ(verbs[3], Path::Verb::CurveTo);
  EXPECT_EQ(verbs[4], Path::Verb::ClosePath);

  EXPECT_EQ(pointCounts[0], 1u);
  EXPECT_EQ(pointCounts[1], 1u);
  EXPECT_EQ(pointCounts[2], 2u);
  EXPECT_EQ(pointCounts[3], 3u);
  EXPECT_EQ(pointCounts[4], 0u);
}

// =============================================================================
// Path::cubicToQuadratic
// =============================================================================

TEST(Path, CubicToQuadraticStraightLine) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({1, 1}, {2, 2}, {3, 3})
                  .build();

  Path result = path.cubicToQuadratic(0.01);

  // Straight-line cubic should become a single quadratic.
  bool hasQuadTo = false;
  bool hasCurveTo = false;
  result.forEach([&](Path::Verb verb, std::span<const Vector2d>) {
    if (verb == Path::Verb::QuadTo) {
      hasQuadTo = true;
    }
    if (verb == Path::Verb::CurveTo) {
      hasCurveTo = true;
    }
  });

  EXPECT_TRUE(hasQuadTo);
  EXPECT_FALSE(hasCurveTo);  // No cubics should remain.
}

TEST(Path, CubicToQuadraticPreservesNonCubics) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .quadTo({15, 10}, {20, 0})
                  .closePath()
                  .build();

  Path result = path.cubicToQuadratic();

  // No cubics to convert — should be identical.
  EXPECT_EQ(result.verbCount(), path.verbCount());
}

TEST(Path, CubicToQuadraticSCurve) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 10}, {10, -10}, {10, 0})
                  .build();

  Path result = path.cubicToQuadratic(0.1);

  // S-curve should produce multiple quadratics.
  size_t quadCount = 0;
  result.forEach([&](Path::Verb verb, std::span<const Vector2d>) {
    if (verb == Path::Verb::QuadTo) {
      ++quadCount;
    }
  });
  EXPECT_GT(quadCount, 1u);
}

// =============================================================================
// Path::toMonotonic
// =============================================================================

TEST(Path, ToMonotonicAlreadyMonotonic) {
  // A line is always monotonic.
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 10})
                  .build();

  Path result = path.toMonotonic();
  EXPECT_EQ(result.verbCount(), path.verbCount());
}

TEST(Path, ToMonotonicSplitsQuadratic) {
  // An arch: Y goes up then down. Should be split at the Y-extremum.
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .quadTo({50, 100}, {100, 0})
                  .build();

  Path result = path.toMonotonic();

  // The single quadratic should become two quadratics.
  size_t quadCount = 0;
  result.forEach([&](Path::Verb verb, std::span<const Vector2d>) {
    if (verb == Path::Verb::QuadTo) {
      ++quadCount;
    }
  });
  EXPECT_EQ(quadCount, 2u);
}

TEST(Path, ToMonotonicSplitsCubic) {
  // S-curve with two Y-extrema.
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 10}, {10, -10}, {10, 0})
                  .build();

  Path result = path.toMonotonic();

  // Should produce 3 cubic segments (split at 2 Y-extrema).
  size_t cubicCount = 0;
  result.forEach([&](Path::Verb verb, std::span<const Vector2d>) {
    if (verb == Path::Verb::CurveTo) {
      ++cubicCount;
    }
  });
  EXPECT_EQ(cubicCount, 3u);
}

// =============================================================================
// Path::flatten
// =============================================================================

TEST(Path, FlattenLinesUnchanged) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .closePath()
                  .build();

  Path result = path.flatten();

  // All-lines path should be unchanged.
  EXPECT_EQ(result.verbCount(), path.verbCount());
}

TEST(Path, FlattenProducesOnlyLines) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 100}, {100, 100}, {100, 0})
                  .quadTo({50, -50}, {0, 0})
                  .closePath()
                  .build();

  Path result = path.flatten();

  // No curves should remain.
  result.forEach([](Path::Verb verb, std::span<const Vector2d>) {
    EXPECT_NE(verb, Path::Verb::QuadTo);
    EXPECT_NE(verb, Path::Verb::CurveTo);
  });

  // Should have multiple line segments.
  size_t lineCount = 0;
  result.forEach([&](Path::Verb verb, std::span<const Vector2d>) {
    if (verb == Path::Verb::LineTo) {
      ++lineCount;
    }
  });
  EXPECT_GT(lineCount, 2u);
}

TEST(Path, FlattenToleranceAffectsSegmentCount) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 100}, {100, 100}, {100, 0})
                  .build();

  Path fine = path.flatten(0.01);
  Path coarse = path.flatten(10.0);

  EXPECT_GE(fine.verbCount(), coarse.verbCount());
}

// =============================================================================
// pointsPerVerb
// =============================================================================

TEST(Path, PointsPerVerb) {
  EXPECT_EQ(Path::pointsPerVerb(Path::Verb::MoveTo), 1u);
  EXPECT_EQ(Path::pointsPerVerb(Path::Verb::LineTo), 1u);
  EXPECT_EQ(Path::pointsPerVerb(Path::Verb::QuadTo), 2u);
  EXPECT_EQ(Path::pointsPerVerb(Path::Verb::CurveTo), 3u);
  EXPECT_EQ(Path::pointsPerVerb(Path::Verb::ClosePath), 0u);
}

// =============================================================================
// Immutability
// =============================================================================

TEST(Path, CopyPreservesContent) {
  Path original = PathBuilder()
                      .moveTo({1, 2})
                      .lineTo({3, 4})
                      .build();

  Path copy = original;  // NOLINT(performance-unnecessary-copy-initialization)

  EXPECT_EQ(copy.verbCount(), original.verbCount());
  EXPECT_EQ(copy.points().size(), original.points().size());
  ExpectNear(copy.points()[0], original.points()[0]);
  ExpectNear(copy.points()[1], original.points()[1]);
}

TEST(Path, MoveTransfersOwnership) {
  Path original = PathBuilder()
                      .moveTo({1, 2})
                      .lineTo({3, 4})
                      .build();

  Path moved = std::move(original);

  EXPECT_EQ(moved.verbCount(), 2u);
  // original is in a valid but unspecified state after move.
}

// =============================================================================
// Path::transformedBounds
// =============================================================================

TEST(Path, TransformedBoundsEmpty) {
  Path path;
  Box2d bounds = path.transformedBounds(Transform2d::Translate({10, 20}));
  EXPECT_EQ(bounds.topLeft, Vector2d(0, 0));
  EXPECT_EQ(bounds.bottomRight, Vector2d(0, 0));
}

TEST(Path, TransformedBoundsIdentity) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 20}).build();
  Box2d untransformed = path.bounds();
  Box2d transformed = path.transformedBounds(Transform2d());
  ExpectNear(transformed.topLeft, untransformed.topLeft);
  ExpectNear(transformed.bottomRight, untransformed.bottomRight);
}

TEST(Path, TransformedBoundsTranslate) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 20}).build();
  Box2d transformed = path.transformedBounds(Transform2d::Translate({5, 7}));
  ExpectNear(transformed.topLeft, Vector2d(5, 7));
  ExpectNear(transformed.bottomRight, Vector2d(15, 27));
}

TEST(Path, TransformedBoundsScale) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 20}).build();
  Box2d transformed = path.transformedBounds(Transform2d::Scale({2, 3}));
  ExpectNear(transformed.topLeft, Vector2d(0, 0));
  ExpectNear(transformed.bottomRight, Vector2d(20, 60));
}

// =============================================================================
// Path::pathLength
// =============================================================================

TEST(Path, PathLengthEmpty) {
  Path path;
  EXPECT_DOUBLE_EQ(path.pathLength(), 0.0);
}

TEST(Path, PathLengthSingleLine) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({3, 4}).build();
  EXPECT_NEAR(path.pathLength(), 5.0, 1e-9);  // 3-4-5 triangle
}

TEST(Path, PathLengthMultipleLines) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).build();
  EXPECT_NEAR(path.pathLength(), 20.0, 1e-9);
}

TEST(Path, PathLengthClosePath) {
  // 10x10 square: 4 sides of 10 = 40
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .lineTo({0, 10})
                  .closePath()
                  .build();
  EXPECT_NEAR(path.pathLength(), 40.0, 1e-9);
}

TEST(Path, PathLengthQuadratic) {
  // Degenerate quadratic (control on the line) — length equals chord.
  Path path = PathBuilder().moveTo({0, 0}).quadTo({5, 0}, {10, 0}).build();
  EXPECT_NEAR(path.pathLength(), 10.0, 1e-3);
}

TEST(Path, PathLengthCubic) {
  // Degenerate cubic (collinear control points) — length equals chord.
  Path path = PathBuilder().moveTo({0, 0}).curveTo({3, 0}, {7, 0}, {10, 0}).build();
  EXPECT_NEAR(path.pathLength(), 10.0, 1e-3);
}

// =============================================================================
// Path::pointAtArcLength
// =============================================================================

TEST(Path, PointAtArcLengthEmpty) {
  Path path;
  Path::PointOnPath result = path.pointAtArcLength(1.0);
  EXPECT_FALSE(result.valid);
}

TEST(Path, PointAtArcLengthNegative) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Path::PointOnPath result = path.pointAtArcLength(-1.0);
  EXPECT_FALSE(result.valid);
}

TEST(Path, PointAtArcLengthAtZero) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Path::PointOnPath result = path.pointAtArcLength(0.0);
  EXPECT_TRUE(result.valid);
  ExpectNear(result.point, Vector2d(0, 0));
}

TEST(Path, PointAtArcLengthMidpoint) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Path::PointOnPath result = path.pointAtArcLength(5.0);
  EXPECT_TRUE(result.valid);
  ExpectNear(result.point, Vector2d(5, 0), 1e-6);
}

TEST(Path, PointAtArcLengthAtEnd) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Path::PointOnPath result = path.pointAtArcLength(10.0);
  EXPECT_TRUE(result.valid);
  ExpectNear(result.point, Vector2d(10, 0), 1e-6);
}

TEST(Path, PointAtArcLengthBeyondEnd) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Path::PointOnPath result = path.pointAtArcLength(100.0);
  EXPECT_FALSE(result.valid);
}

TEST(Path, PointAtArcLengthCrossesSegments) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).build();
  // 5 units past start of second segment.
  Path::PointOnPath result = path.pointAtArcLength(15.0);
  EXPECT_TRUE(result.valid);
  ExpectNear(result.point, Vector2d(10, 5), 1e-6);
}

TEST(Path, PointAtArcLengthOnQuadratic) {
  Path path = PathBuilder().moveTo({0, 0}).quadTo({5, 0}, {10, 0}).build();
  Path::PointOnPath result = path.pointAtArcLength(5.0);
  EXPECT_TRUE(result.valid);
  ExpectNear(result.point, Vector2d(5, 0), 1e-3);
}

TEST(Path, PointAtArcLengthOnCubic) {
  Path path = PathBuilder().moveTo({0, 0}).curveTo({3, 0}, {7, 0}, {10, 0}).build();
  Path::PointOnPath result = path.pointAtArcLength(5.0);
  EXPECT_TRUE(result.valid);
  ExpectNear(result.point, Vector2d(5, 0), 1e-3);
}

TEST(Path, PointAtArcLengthOnClosePath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .lineTo({0, 10})
                  .closePath()
                  .build();
  // Total length = 40. Distance 35 = 5 units along close-path edge from (0,10) toward (0,0).
  Path::PointOnPath result = path.pointAtArcLength(35.0);
  EXPECT_TRUE(result.valid);
  ExpectNear(result.point, Vector2d(0, 5), 1e-6);
}

// =============================================================================
// Path::pointAt / tangentAt / normalAt
// =============================================================================

TEST(Path, PointAtMoveTo) {
  Path path = PathBuilder().moveTo({3, 4}).lineTo({10, 0}).build();
  ExpectNear(path.pointAt(0, 0.0), Vector2d(3, 4));
  ExpectNear(path.pointAt(0, 1.0), Vector2d(3, 4));
}

TEST(Path, PointAtLineTo) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 20}).build();
  ExpectNear(path.pointAt(1, 0.0), Vector2d(0, 0));
  ExpectNear(path.pointAt(1, 0.5), Vector2d(5, 10));
  ExpectNear(path.pointAt(1, 1.0), Vector2d(10, 20));
}

TEST(Path, PointAtQuadTo) {
  Path path = PathBuilder().moveTo({0, 0}).quadTo({10, 10}, {20, 0}).build();
  ExpectNear(path.pointAt(1, 0.0), Vector2d(0, 0));
  ExpectNear(path.pointAt(1, 0.5), Vector2d(10, 5));
  ExpectNear(path.pointAt(1, 1.0), Vector2d(20, 0));
}

TEST(Path, PointAtCurveTo) {
  Path path = PathBuilder().moveTo({0, 0}).curveTo({0, 10}, {10, 10}, {10, 0}).build();
  ExpectNear(path.pointAt(1, 0.0), Vector2d(0, 0));
  ExpectNear(path.pointAt(1, 1.0), Vector2d(10, 0));
  // Midpoint of this symmetric S-shape: (5, 7.5)
  ExpectNear(path.pointAt(1, 0.5), Vector2d(5, 7.5));
}

TEST(Path, PointAtClosePath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .closePath()
                  .build();
  // ClosePath segment goes from (10, 10) back to (0, 0).
  ExpectNear(path.pointAt(3, 0.0), Vector2d(10, 10));
  ExpectNear(path.pointAt(3, 0.5), Vector2d(5, 5));
  ExpectNear(path.pointAt(3, 1.0), Vector2d(0, 0));
}

TEST(Path, TangentAtLineTo) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Vector2d tangent = path.tangentAt(1, 0.5);
  ExpectNear(tangent, Vector2d(10, 0));
}

TEST(Path, TangentAtMoveToForwardsToNext) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Vector2d tangent = path.tangentAt(0, 0.0);
  ExpectNear(tangent, Vector2d(10, 0));
}

TEST(Path, TangentAtMoveToOnlyReturnsZero) {
  Path path = PathBuilder().moveTo({0, 0}).build();
  Vector2d tangent = path.tangentAt(0, 0.0);
  ExpectNear(tangent, Vector2d(0, 0));
}

TEST(Path, TangentAtQuadTo) {
  Path path = PathBuilder().moveTo({0, 0}).quadTo({10, 10}, {20, 0}).build();
  // At t=0.5, derivative of symmetric quad: tangent is horizontal.
  Vector2d tangent = path.tangentAt(1, 0.5);
  EXPECT_NEAR(tangent.y, 0.0, 1e-9);
  EXPECT_GT(tangent.x, 0.0);
}

TEST(Path, TangentAtCurveTo) {
  Path path = PathBuilder().moveTo({0, 0}).curveTo({10, 0}, {10, 10}, {20, 10}).build();
  Vector2d tangent = path.tangentAt(1, 0.0);
  // Initial tangent points toward c1.
  EXPECT_GT(tangent.x, 0.0);
  EXPECT_NEAR(tangent.y, 0.0, 1e-9);
}

TEST(Path, TangentAtClosePath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .closePath()
                  .build();
  Vector2d tangent = path.tangentAt(2, 0.0);
  ExpectNear(tangent, Vector2d(-10, 0));
}

TEST(Path, NormalAtLineTo) {
  // For a line going right, the 90deg-CCW normal points up (negative y in screen coords; here +y).
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Vector2d normal = path.normalAt(1, 0.5);
  ExpectNear(normal, Vector2d(0, 10));
}

// =============================================================================
// Path::isInside
// =============================================================================

TEST(Path, IsInsideEmptyPath) {
  Path path;
  EXPECT_FALSE(path.isInside({0, 0}));
}

TEST(Path, IsInsideRectInterior) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {10, 10})).closePath().build();
  EXPECT_TRUE(path.isInside({5, 5}));
}

TEST(Path, IsInsideRectExterior) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {10, 10})).closePath().build();
  EXPECT_FALSE(path.isInside({20, 20}));
}

TEST(Path, IsInsideOnBoundary) {
  Path path = PathBuilder().addRect(Box2d({0, 0}, {10, 10})).closePath().build();
  // Points on the boundary are considered inside.
  EXPECT_TRUE(path.isInside({0, 5}));
}

TEST(Path, IsInsideQuadraticPath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .quadTo({50, 100}, {100, 0})
                  .closePath()
                  .build();
  EXPECT_TRUE(path.isInside({50, 20}));
  EXPECT_FALSE(path.isInside({200, 200}));
}

TEST(Path, IsInsideCubicPath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 100}, {100, 100}, {100, 0})
                  .closePath()
                  .build();
  EXPECT_TRUE(path.isInside({50, 30}));
  EXPECT_FALSE(path.isInside({200, 200}));
}

TEST(Path, IsInsideEvenOddRule) {
  // Two nested rects with the same winding direction. Even-odd: inner is OUT, NonZero: inner is IN.
  Path path = PathBuilder()
                  .addRect(Box2d({0, 0}, {30, 30}))
                  .closePath()
                  .addRect(Box2d({10, 10}, {20, 20}))
                  .closePath()
                  .build();
  EXPECT_TRUE(path.isInside({15, 15}, FillRule::NonZero));
  EXPECT_FALSE(path.isInside({15, 15}, FillRule::EvenOdd));
}

// =============================================================================
// Path::isOnPath
// =============================================================================

TEST(Path, IsOnPathOnLineSegment) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  EXPECT_TRUE(path.isOnPath({5, 0}, 1.0));
  EXPECT_TRUE(path.isOnPath({5, 0.4}, 1.0));
}

TEST(Path, IsOnPathOffSegment) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  EXPECT_FALSE(path.isOnPath({5, 5}, 1.0));
}

TEST(Path, IsOnPathStrokeWidthAffectsResult) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  EXPECT_FALSE(path.isOnPath({5, 3}, 1.0));
  EXPECT_TRUE(path.isOnPath({5, 3}, 5.0));
}

TEST(Path, IsOnPathQuadratic) {
  Path path = PathBuilder().moveTo({0, 0}).quadTo({5, 0}, {10, 0}).build();
  EXPECT_TRUE(path.isOnPath({5, 0}, 1.0));
}

TEST(Path, IsOnPathCubic) {
  Path path = PathBuilder().moveTo({0, 0}).curveTo({3, 0}, {7, 0}, {10, 0}).build();
  EXPECT_TRUE(path.isOnPath({5, 0}, 1.0));
}

TEST(Path, IsOnPathClosePath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .closePath()
                  .build();
  // Closing edge goes diagonally from (10,10) to (0,0).
  EXPECT_TRUE(path.isOnPath({5, 5}, 1.0));
}

// =============================================================================
// Path::strokeMiterBounds
// =============================================================================

TEST(Path, StrokeMiterBoundsLine) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  Box2d bounds = path.strokeMiterBounds(2.0, 4.0);
  // Bounds should at least contain the line endpoints.
  EXPECT_LE(bounds.topLeft.x, 0.0);
  EXPECT_GE(bounds.bottomRight.x, 10.0);
}

TEST(Path, StrokeMiterBoundsClosedRect) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .lineTo({0, 10})
                  .closePath()
                  .build();
  Box2d bounds = path.strokeMiterBounds(2.0, 4.0);
  // Miter joins extend slightly past the corners.
  EXPECT_LE(bounds.topLeft.x, 0.0);
  EXPECT_LE(bounds.topLeft.y, 0.0);
  EXPECT_GE(bounds.bottomRight.x, 10.0);
  EXPECT_GE(bounds.bottomRight.y, 10.0);
}

TEST(Path, StrokeMiterBoundsAcuteAngle) {
  // Sharp angle: miter would extend far, but is clipped by miter limit.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).lineTo({0, 1}).build();
  Box2d bounds = path.strokeMiterBounds(2.0, 4.0);
  // Just verify it doesn't crash and returns a sensible (non-empty) box.
  EXPECT_GE(bounds.bottomRight.x, bounds.topLeft.x);
  EXPECT_GE(bounds.bottomRight.y, bounds.topLeft.y);
}

// =============================================================================
// Path::strokeToFill
// =============================================================================

TEST(Path, StrokeToFillSingleLine) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  Path filled = path.strokeToFill(style);
  // Should produce a non-empty filled region around the line.
  EXPECT_FALSE(filled.empty());
  Box2d filledBounds = filled.bounds();
  EXPECT_GE(filledBounds.bottomRight.x - filledBounds.topLeft.x, 10.0);
}

TEST(Path, StrokeToFillRoundCap) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.cap = LineCap::Round;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
}

TEST(Path, StrokeToFillSquareCap) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.cap = LineCap::Square;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
}

TEST(Path, StrokeToFillRoundJoin) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.join = LineJoin::Round;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
}

TEST(Path, StrokeToFillBevelJoin) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.join = LineJoin::Bevel;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
}

TEST(Path, StrokeToFillClosedPath) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .lineTo({0, 10})
                  .closePath()
                  .build();
  StrokeStyle style;
  style.width = 2.0;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
}

TEST(Path, StrokeToFillCurves) {
  // Curves are flattened then offset.
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .curveTo({0, 10}, {10, 10}, {10, 0})
                  .build();
  StrokeStyle style;
  style.width = 1.0;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
}

// =============================================================================
// Path::vertices
// =============================================================================

TEST(Path, VerticesEmpty) {
  Path path;
  std::vector<Path::Vertex> verts = path.vertices();
  EXPECT_TRUE(verts.empty());
}

TEST(Path, VerticesSingleLine) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  std::vector<Path::Vertex> verts = path.vertices();
  EXPECT_GE(verts.size(), 2u);
  ExpectNear(verts.front().point, Vector2d(0, 0));
  ExpectNear(verts.back().point, Vector2d(10, 0));
}

TEST(Path, VerticesPolyline) {
  Path path =
      PathBuilder().moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).lineTo({0, 10}).build();
  std::vector<Path::Vertex> verts = path.vertices();
  EXPECT_GE(verts.size(), 4u);
}

TEST(Path, VerticesClosedRect) {
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .lineTo({0, 10})
                  .closePath()
                  .build();
  std::vector<Path::Vertex> verts = path.vertices();
  EXPECT_GE(verts.size(), 4u);
}

// =============================================================================
// PathBuilder::arcTo
// =============================================================================

TEST(PathBuilder, ArcTo) {
  // Quarter-circle arc from (10, 0) to (0, 10) with radius 10.
  Path path = PathBuilder().moveTo({10, 0}).arcTo({10, 10}, 0.0, false, false, {0, 10}).build();
  EXPECT_GT(path.verbCount(), 1u);
  // Endpoints should be exact.
  ExpectNear(path.points().front(), Vector2d(10, 0));
  // Last command should end at the arc endpoint.
  Vector2d endPoint = path.points().back();
  ExpectNear(endPoint, Vector2d(0, 10), 1e-6);
}

TEST(PathBuilder, ArcToLargeArc) {
  Path path = PathBuilder().moveTo({10, 0}).arcTo({10, 10}, 0.0, true, true, {0, 10}).build();
  EXPECT_GT(path.verbCount(), 1u);
}

TEST(PathBuilder, ArcToImplicitMoveTo) {
  // arcTo with no preceding moveTo should work — implicit moveTo at (0,0).
  Path path = PathBuilder().arcTo({10, 10}, 0.0, false, false, {10, 10}).build();
  EXPECT_GT(path.verbCount(), 1u);
}

TEST(PathBuilder, ArcToZeroRadius) {
  // Zero radius arc degenerates to a line.
  Path path = PathBuilder().moveTo({0, 0}).arcTo({0, 0}, 0.0, false, false, {10, 10}).build();
  EXPECT_GT(path.verbCount(), 1u);
}

TEST(PathBuilder, ArcToCoincidentEndpoints) {
  // SVG spec: if endpoints are the same, the arc is omitted entirely.
  Path path = PathBuilder().moveTo({5, 5}).arcTo({10, 10}, 0.0, false, false, {5, 5}).build();
  // No arc segment added.
  EXPECT_EQ(path.verbCount(), 1u);
}

TEST(PathBuilder, ArcToWithRotation) {
  Path path = PathBuilder()
                  .moveTo({10, 0})
                  .arcTo({10, 5}, MathConstants<double>::kPi / 4, false, true, {0, 10})
                  .build();
  EXPECT_GT(path.verbCount(), 1u);
}

// =============================================================================
// PathBuilder::addRoundedRect
// =============================================================================

TEST(PathBuilder, AddRoundedRect) {
  Path path = PathBuilder().addRoundedRect(Box2d({0, 0}, {100, 50}), 10.0, 10.0).build();
  EXPECT_GT(path.verbCount(), 4u);
  // Bounds match the underlying rect.
  Box2d bounds = path.bounds();
  ExpectNear(bounds.topLeft, Vector2d(0, 0), 1e-6);
  ExpectNear(bounds.bottomRight, Vector2d(100, 50), 1e-6);
}

TEST(PathBuilder, AddRoundedRectZeroCornerRadius) {
  Path path = PathBuilder().addRoundedRect(Box2d({0, 0}, {100, 50}), 0.0, 0.0).build();
  // With zero radius, falls back to a regular rect.
  EXPECT_GT(path.verbCount(), 0u);
}

// =============================================================================
// Ostream operators
// =============================================================================

TEST(Path, OstreamVerb) {
  std::ostringstream oss;
  oss << Path::Verb::MoveTo << "," << Path::Verb::LineTo << "," << Path::Verb::QuadTo << ","
      << Path::Verb::CurveTo << "," << Path::Verb::ClosePath;
  EXPECT_EQ(oss.str(), "MoveTo,LineTo,QuadTo,CurveTo,ClosePath");
}

TEST(Path, OstreamCommand) {
  Path::Command cmd{Path::Verb::LineTo, 5};
  std::ostringstream oss;
  oss << cmd;
  EXPECT_NE(oss.str().find("LineTo"), std::string::npos);
  EXPECT_NE(oss.str().find("5"), std::string::npos);
}

TEST(Path, OstreamPath) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  std::ostringstream oss;
  oss << path;
  EXPECT_FALSE(oss.str().empty());
}

TEST(LineCap, OstreamOperator) {
  std::ostringstream oss;
  oss << LineCap::Butt << "," << LineCap::Round << "," << LineCap::Square;
  EXPECT_EQ(oss.str(), "Butt,Round,Square");
}

TEST(LineJoin, OstreamOperator) {
  std::ostringstream oss;
  oss << LineJoin::Miter << "," << LineJoin::Round << "," << LineJoin::Bevel;
  EXPECT_EQ(oss.str(), "Miter,Round,Bevel");
}

TEST(Path, OstreamVertex) {
  Path::Vertex v{{1, 2}, {3, 4}};
  std::ostringstream oss;
  oss << v;
  EXPECT_NE(oss.str().find("Vertex"), std::string::npos);
}

}  // namespace donner
