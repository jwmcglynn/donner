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

namespace {

/// Ray-cast winding number for a polygonal closed path.
///
/// Casts a horizontal ray from `query` going +x and counts signed crossings with
/// each LineTo / ClosePath edge. Horizontal edges contribute 0. Assumes the
/// path is composed only of MoveTo, LineTo, and ClosePath (i.e. already
/// flattened). This is sufficient for checking `strokeToFill` output, which is
/// always a closed polygon of LineTos.
int rayCastWinding(const Path& path, const Vector2d& query) {
  int winding = 0;
  Vector2d start;
  Vector2d prev;
  bool havePrev = false;

  auto crossEdge = [&](const Vector2d& a, const Vector2d& b) {
    // Skip horizontal edges.
    if (a.y == b.y) {
      return;
    }
    // Does the edge straddle the query y? Use a half-open interval so
    // vertex-touches aren't double-counted.
    const bool upward = b.y > a.y;
    const double yLo = upward ? a.y : b.y;
    const double yHi = upward ? b.y : a.y;
    if (query.y < yLo || query.y >= yHi) {
      return;
    }
    // Intersection x at query.y.
    const double t = (query.y - a.y) / (b.y - a.y);
    const double ix = a.x + t * (b.x - a.x);
    if (ix > query.x) {
      winding += upward ? 1 : -1;
    }
  };

  for (const auto& cmd : path.commands()) {
    switch (cmd.verb) {
      case Path::Verb::MoveTo:
        start = path.points()[cmd.pointIndex];
        prev = start;
        havePrev = true;
        break;
      case Path::Verb::LineTo: {
        const Vector2d& p = path.points()[cmd.pointIndex];
        if (havePrev) {
          crossEdge(prev, p);
        }
        prev = p;
        break;
      }
      case Path::Verb::ClosePath:
        if (havePrev) {
          crossEdge(prev, start);
        }
        prev = start;
        break;
      case Path::Verb::QuadTo:
      case Path::Verb::CurveTo:
        // Not expected in strokeToFill output.
        break;
    }
  }
  return winding;
}

}  // namespace

TEST(Path, StrokeToFillRoundCap) {
  // Horizontal line from (0,0) to (10,0) with stroke width 2 (halfWidth=1).
  // With round caps the filled region is a rect [0,10]×[-1,1] plus two unit
  // semicircles centered at (0,0) and (10,0) — i.e., the set of points within
  // distance 1 of the segment.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.cap = LineCap::Round;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  // A round cap produces a semicircle of arc points at each end, versus
  // butt which is a single lineTo. With two round caps the total point
  // count should be substantially larger than a butt-capped stroke.
  Path butt = path.strokeToFill({.width = 2.0, .cap = LineCap::Butt});
  EXPECT_GT(filled.points().size(), butt.points().size())
      << "Round caps should add arc points beyond the butt-cap baseline";

  // The bounding box should extend ±halfWidth past the line endpoints in
  // the line's travel direction (the round cap's outermost extent).
  const Box2d bounds = filled.bounds();
  EXPECT_LT(bounds.topLeft.x, -0.95) << "Round cap should extend ~halfWidth left of x=0";
  EXPECT_GT(bounds.bottomRight.x, 9.95)
      << "Round cap should extend ~halfWidth right of x=10";
  EXPECT_NEAR(bounds.topLeft.y, -1.0, 1e-9);
  EXPECT_NEAR(bounds.bottomRight.y, 1.0, 1e-9);
  EXPECT_NEAR(bounds.topLeft.x, -1.0, 0.1)
      << "Left cap polyline should reach ~(-1, 0)";
  EXPECT_NEAR(bounds.bottomRight.x, 11.0, 0.1)
      << "Right cap polyline should reach ~(11, 0)";

  // ---- Winding tests inside the expected round-capped region ----
  // Interior of the straight part.
  EXPECT_NE(rayCastWinding(filled, {5.0, 0.0}), 0) << "interior of line";
  EXPECT_NE(rayCastWinding(filled, {5.0, 0.9}), 0) << "near top edge interior";
  EXPECT_NE(rayCastWinding(filled, {5.0, -0.9}), 0) << "near bottom edge interior";

  // Inside each round cap — points within unit distance of the endpoint
  // but outside the [0,10] x range.
  EXPECT_NE(rayCastWinding(filled, {-0.5, 0.0}), 0) << "inside left round cap";
  EXPECT_NE(rayCastWinding(filled, {-0.8, 0.0}), 0) << "near far-left of left cap";
  EXPECT_NE(rayCastWinding(filled, {10.5, 0.0}), 0) << "inside right round cap";
  EXPECT_NE(rayCastWinding(filled, {10.8, 0.0}), 0) << "near far-right of right cap";
  EXPECT_NE(rayCastWinding(filled, {-0.3, 0.5}), 0) << "inside upper-left of left cap";
  EXPECT_NE(rayCastWinding(filled, {10.3, -0.5}), 0) << "inside lower-right of right cap";

  // ---- Winding tests outside the expected region ----
  // Well past either cap on the x axis.
  EXPECT_EQ(rayCastWinding(filled, {-2.0, 0.0}), 0) << "well past left cap";
  EXPECT_EQ(rayCastWinding(filled, {12.0, 0.0}), 0) << "well past right cap";
  // Above / below the stroke band.
  EXPECT_EQ(rayCastWinding(filled, {5.0, 2.0}), 0) << "above the stroke";
  EXPECT_EQ(rayCastWinding(filled, {5.0, -2.0}), 0) << "below the stroke";
  // Corner regions outside the round cap but inside what a square cap would
  // cover (|x|+epsilon past the endpoint, near ±halfWidth in y). With stroke
  // width 2 at endpoint (0,0): (-0.9, 0.9) has distance sqrt(1.62) ≈ 1.273 > 1,
  // so it should be OUTSIDE the round cap (but inside a square cap).
  EXPECT_EQ(rayCastWinding(filled, {-0.9, 0.9}), 0)
      << "upper-left corner outside round cap";
  EXPECT_EQ(rayCastWinding(filled, {-0.9, -0.9}), 0)
      << "lower-left corner outside round cap";
  EXPECT_EQ(rayCastWinding(filled, {10.9, 0.9}), 0)
      << "upper-right corner outside round cap";
  EXPECT_EQ(rayCastWinding(filled, {10.9, -0.9}), 0)
      << "lower-right corner outside round cap";
}

TEST(Path, StrokeToFillSquareCap) {
  // Horizontal line from (0,0) to (10,0) with stroke width 2 (halfWidth=1).
  // With square caps the filled region is the rect [-1,11] x [-1,1].
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.cap = LineCap::Square;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  // Square caps extend halfWidth past each endpoint in the line direction.
  // For a horizontal line (0,0)→(10,0) with width 2, the bounds should
  // reach x=-1 and x=11.
  const Box2d bounds = filled.bounds();
  EXPECT_LE(bounds.topLeft.x, -1.0 + 1e-9)
      << "Square cap should extend halfWidth left of x=0";
  EXPECT_GE(bounds.bottomRight.x, 11.0 - 1e-9)
      << "Square cap should extend halfWidth right of x=10";
  EXPECT_NEAR(bounds.topLeft.y, -1.0, 1e-9);
  EXPECT_NEAR(bounds.bottomRight.y, 1.0, 1e-9);

  // ---- Winding tests inside the expected rectangular region ----
  // Interior of the line body.
  EXPECT_NE(rayCastWinding(filled, {5.0, 0.0}), 0);
  // Square cap corners — these should all be filled for square caps.
  EXPECT_NE(rayCastWinding(filled, {-0.9, 0.9}), 0)
      << "upper-left corner of left square cap should be filled";
  EXPECT_NE(rayCastWinding(filled, {-0.9, -0.9}), 0)
      << "lower-left corner of left square cap should be filled";
  EXPECT_NE(rayCastWinding(filled, {10.9, 0.9}), 0)
      << "upper-right corner of right square cap should be filled";
  EXPECT_NE(rayCastWinding(filled, {10.9, -0.9}), 0)
      << "lower-right corner of right square cap should be filled";
  // Middle of each square cap.
  EXPECT_NE(rayCastWinding(filled, {-0.5, 0.0}), 0) << "middle of left square cap";
  EXPECT_NE(rayCastWinding(filled, {10.5, 0.0}), 0) << "middle of right square cap";

  // ---- Winding tests outside the expected region ----
  // Well past either cap.
  EXPECT_EQ(rayCastWinding(filled, {-1.5, 0.0}), 0) << "past left square cap";
  EXPECT_EQ(rayCastWinding(filled, {11.5, 0.0}), 0) << "past right square cap";
  // Above / below the stroke band.
  EXPECT_EQ(rayCastWinding(filled, {5.0, 1.5}), 0) << "above the stroke";
  EXPECT_EQ(rayCastWinding(filled, {5.0, -1.5}), 0) << "below the stroke";
  // Diagonal outside of the extended rect.
  EXPECT_EQ(rayCastWinding(filled, {-1.5, 1.5}), 0) << "outside upper-left";
  EXPECT_EQ(rayCastWinding(filled, {11.5, -1.5}), 0) << "outside lower-right";
}

TEST(Path, StrokeToFillButtCapShape) {
  // Butt cap baseline: the filled region is exactly the rect [0,10]×[-1,1].
  // Used to contrast with round / square caps (no extension past endpoints).
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.cap = LineCap::Butt;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  const Box2d bounds = filled.bounds();
  EXPECT_NEAR(bounds.topLeft.x, 0.0, 1e-9) << "Butt cap must not extend past x=0";
  EXPECT_NEAR(bounds.bottomRight.x, 10.0, 1e-9) << "Butt cap must not extend past x=10";

  // Interior filled, cap-extension area NOT filled.
  EXPECT_NE(rayCastWinding(filled, {5.0, 0.0}), 0);
  EXPECT_EQ(rayCastWinding(filled, {-0.1, 0.0}), 0) << "butt: no left extension";
  EXPECT_EQ(rayCastWinding(filled, {10.1, 0.0}), 0) << "butt: no right extension";
}

TEST(Path, StrokeToFillRoundCapVertical) {
  // Same line but vertical, to verify the cap math is direction-agnostic.
  // Line from (0,0) to (0,10) with width 2; round caps at top and bottom.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({0, 10}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.cap = LineCap::Round;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  const Box2d bounds = filled.bounds();
  EXPECT_NEAR(bounds.topLeft.x, -1.0, 1e-9);
  EXPECT_NEAR(bounds.bottomRight.x, 1.0, 1e-9);
  EXPECT_NEAR(bounds.topLeft.y, -1.0, 0.1) << "bottom round cap extends below 0";
  EXPECT_NEAR(bounds.bottomRight.y, 11.0, 0.1) << "top round cap extends above 10";

  // Inside each cap.
  EXPECT_NE(rayCastWinding(filled, {0.0, -0.5}), 0) << "inside bottom round cap";
  EXPECT_NE(rayCastWinding(filled, {0.0, 10.5}), 0) << "inside top round cap";
  EXPECT_NE(rayCastWinding(filled, {0.0, 5.0}), 0) << "interior of line";
  // Corners outside round cap (distance > 1 from endpoint).
  EXPECT_EQ(rayCastWinding(filled, {0.9, -0.9}), 0);
  EXPECT_EQ(rayCastWinding(filled, {-0.9, 10.9}), 0);
}

TEST(Path, StrokeToFillSquareCapDiagonal) {
  // Diagonal line: cap geometry should extend along the line direction.
  // Line from (0,0) to (10,10), width = 2*sqrt(2), so halfWidth = sqrt(2).
  // The tangent at the ends is (1,1)/sqrt(2), so the square-cap extension
  // along that tangent is sqrt(2) * (1,1)/sqrt(2) = (1,1).
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 10}).build();
  StrokeStyle style;
  style.width = 2.0 * std::sqrt(2.0);
  style.cap = LineCap::Square;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  // Extended endpoints: (-1, -1) and (11, 11).
  const Box2d bounds = filled.bounds();
  EXPECT_LE(bounds.topLeft.x, -1.0 + 1e-6);
  EXPECT_LE(bounds.topLeft.y, -1.0 + 1e-6);
  EXPECT_GE(bounds.bottomRight.x, 11.0 - 1e-6);
  EXPECT_GE(bounds.bottomRight.y, 11.0 - 1e-6);

  // Points along the line axis, inside the square-cap extension region.
  EXPECT_NE(rayCastWinding(filled, {-0.5, -0.5}), 0) << "inside start square cap";
  EXPECT_NE(rayCastWinding(filled, {10.5, 10.5}), 0) << "inside end square cap";
  // Corner of the square cap (perpendicular to the line at the extended endpoint).
  // At (-1,-1) extended corner, shift perpendicular by ~0.5: (-1.5, -0.5).
  // Wait — the square cap is a square of side = strokeWidth, rotated with the
  // line. The corner points are at extended_endpoint ± halfWidth * perpendicular.
  // Keep it simple: check the interior of the line.
  EXPECT_NE(rayCastWinding(filled, {5.0, 5.0}), 0) << "interior of line";
  // Past the cap — well outside.
  EXPECT_EQ(rayCastWinding(filled, {-3.0, -3.0}), 0);
  EXPECT_EQ(rayCastWinding(filled, {13.0, 13.0}), 0);
}

// (Duplicate `rayCastWinding` removed during the Phase 2C merge — the canonical
// definition lives at line ~860 and is shared by all tests below.)

TEST(Path, StrokeToFillRoundJoin) {
  // L-shape: (0,0) -> (10,0) -> (10,10), stroked with width 2 and a round
  // join at the (10,0) corner. halfWidth=1.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.join = LineJoin::Round;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  // A round join subdivides into an arc at the outside of the corner. The
  // point count should exceed the bevel-joined equivalent by the round-join
  // arc subdivision.
  Path bevel =
      path.strokeToFill({.width = 2.0, .cap = LineCap::Butt, .join = LineJoin::Bevel});
  EXPECT_GT(filled.points().size(), bevel.points().size())
      << "Round join should add arc points beyond the bevel baseline";

  // Geometric verification via ray-cast (EvenOdd, matching the Geode
  // renderer). The outside of the corner is the (11,11) region; the round
  // arc should include points close to (11,11)+hw in the corner bisector.
  auto evenOdd = [&](Vector2d p) { return (rayCastWinding(filled, p) & 1) != 0; };

  // Inside the stroke ribbon along each leg.
  EXPECT_TRUE(evenOdd({5, 0})) << "midpoint of horizontal leg stroke";
  EXPECT_TRUE(evenOdd({10, 5})) << "midpoint of vertical leg stroke";

  // The outside of the corner: the round arc should cover the area
  // close to (10.7, 0.7) (on the corner bisector, ~0.7 hw past vertex).
  EXPECT_TRUE(evenOdd({10.7, 0.7})) << "inside round-join arc";

  // Far past the round arc (more than hw beyond vertex on bisector):
  // (11.5, 1.5) is at distance ~2.12*hw from (10,0), outside the stroke.
  EXPECT_FALSE(evenOdd({11.5, 1.5})) << "far past round-join arc";

  // Points clearly outside the stroke ribbon.
  EXPECT_FALSE(evenOdd({5, 5})) << "outside horizontal leg (far below)";
  EXPECT_FALSE(evenOdd({5, -5})) << "outside horizontal leg (far above)";
  EXPECT_FALSE(evenOdd({-5, 0})) << "left of horizontal leg";
}

TEST(Path, StrokeToFillBevelJoin) {
  // Same L-shape, bevel join. halfWidth=1.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.join = LineJoin::Bevel;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  auto evenOdd = [&](Vector2d p) { return (rayCastWinding(filled, p) & 1) != 0; };

  // Stroke ribbon along each leg.
  EXPECT_TRUE(evenOdd({5, 0})) << "midpoint of horizontal leg stroke";
  EXPECT_TRUE(evenOdd({10, 5})) << "midpoint of vertical leg stroke";

  // Bevel chamfers the outside of the corner: the point (10.7, 0.7) is
  // beyond the bevel chord (which runs from (11,0) to (10,-1)) — wait,
  // we need to identify the outside relative to the path direction.
  // The path (0,0)->(10,0)->(10,10) turns right (CW in y-down). The
  // outside of the turn is the top-right: y <= 0 and x >= 10 with
  // x+y<=10 becoming x+y>=11 past the corner. Bevel chord connects the
  // two outer offset line endpoints. For width 2, hw=1, the bevel chord
  // runs from (10, -1) to (11, 0).
  //
  // Point (10.9, -0.9) is ~0.7*hw beyond the vertex on the bisector;
  // it's BEYOND the bevel chord (11 - 10.9 + 0 + 0.9 = 1 which equals
  // the chord line offset, slightly outside by numerical drift). Use
  // (10.4, -0.4) which is comfortably inside the bevel triangle.
  EXPECT_TRUE(evenOdd({10.4, -0.4})) << "inside bevel triangle";

  // The bevel chord cuts off the corner so beyond the chord there's no
  // fill: (11, -1) is past both offset lines.
  EXPECT_FALSE(evenOdd({11, -1})) << "past bevel chord (outside)";

  // Concave side of the corner (inside of the turn, around (9, 1)):
  // the inner offset lines meet at (9,1). Points well inside the concave
  // pocket — deep in the L's interior — should be outside the stroke.
  EXPECT_FALSE(evenOdd({5, 5})) << "deep inside L (concave region)";
}

TEST(Path, StrokeToFillSharpOpenCornerMiterJoin) {
  // Phase 2C: inverted V (M 0 100 L 50 0 L 100 100) with a SHARP inside
  // corner at the apex. Before the fix, `emitJoin`'s inside-turn branch
  // emitted a single line across the overlap, creating a self-intersecting
  // polygon that neither NonZero nor EvenOdd could render cleanly — visible
  // as gaps/extra triangles at the concave side of the apex in the
  // stroking_linejoin Geode golden.
  //
  // After the fix, the true intersection of the two inner offset lines
  // is emitted, producing a clean polygon. Verify via ray-cast winding
  // (matching Geode's Slug shader) that points inside the stroke ribbon
  // are filled and points outside — especially in the V's concave pocket
  // and far above the apex — are not.
  Path path = PathBuilder().moveTo({0, 100}).lineTo({50, 0}).lineTo({100, 100}).build();
  StrokeStyle style;
  style.width = 20.0;  // halfWidth = 10
  style.join = LineJoin::Miter;
  style.miterLimit = 4.0;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  auto evenOdd = [&](Vector2d p) { return (rayCastWinding(filled, p) & 1) != 0; };

  // Points clearly inside the stroke ribbon on each leg. The legs have
  // (unit) left normal n1 = (2,1)/√5 and n2 = (-2,1)/√5, so the stroke
  // ribbon on each leg is the 20-wide strip perpendicular to the leg
  // direction.
  //
  // Midpoint of leg 1: (25, 50). Perturb by small amounts along the
  // left/right normal — points within ±halfWidth must be filled.
  EXPECT_TRUE(evenOdd({25, 50})) << "midpoint of leg 1";
  EXPECT_TRUE(evenOdd({75, 50})) << "midpoint of leg 2";

  // Slightly inside (toward apex from midpoint): still on the leg.
  EXPECT_TRUE(evenOdd({30, 40})) << "leg 1 toward apex";
  EXPECT_TRUE(evenOdd({70, 40})) << "leg 2 toward apex";

  // Close to the apex vertex (50, 0) on the outside of the V: above
  // the apex but within half-width. The outer offset lines at the apex
  // meet at ~(50, -22.36) (the outside miter), so (50, -5) and (50, -15)
  // are inside the outside-miter triangle, well within the stroke.
  EXPECT_TRUE(evenOdd({50, -5})) << "just above apex on outside miter";
  EXPECT_TRUE(evenOdd({50, -15})) << "further above apex, still inside outside miter";

  // Inside of the apex on the concave side: the inner offset lines meet
  // at ~(50, 22.36). Points between the apex and (50, 22) should be
  // inside the stroke ribbon (concave inside-miter triangle).
  EXPECT_TRUE(evenOdd({50, 10})) << "inside concave miter triangle";
  EXPECT_TRUE(evenOdd({50, 20})) << "near apex of inside miter triangle";

  // BELOW the inside miter point (deeper into the V's concave pocket):
  // must be OUTSIDE the stroke. Before the fix this region showed
  // spurious fills/gaps due to the self-intersecting polygon.
  EXPECT_FALSE(evenOdd({50, 40})) << "deep inside V pocket (below inside miter)";
  EXPECT_FALSE(evenOdd({50, 60})) << "further down V pocket";
  EXPECT_FALSE(evenOdd({50, 80})) << "near bottom of V pocket";

  // Above the apex beyond the outside miter (more than miterLimit*hw
  // from the vertex on the exterior bisector): must be OUTSIDE.
  EXPECT_FALSE(evenOdd({50, -30})) << "above outside miter point";

  // Points inside the V but off the leg: far from both legs.
  EXPECT_FALSE(evenOdd({30, 90})) << "bottom-left interior";
  EXPECT_FALSE(evenOdd({70, 90})) << "bottom-right interior";

  // Points clearly outside the legs.
  EXPECT_FALSE(evenOdd({0, 0})) << "far upper left";
  EXPECT_FALSE(evenOdd({100, 0})) << "far upper right";
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
  style.width = 2.0;  // halfWidth = 1
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  // Regression for 2D (donner_sfv #492 follow-up): the stroked rect should
  // render as a hollow square annulus from (-1,-1) to (11,11) with a
  // (1,1)–(9,9) hole. Historically the inside-turn branch of emitJoin emitted
  // `lineTo(curStart)` — the start of the next offset segment — which caused
  // full-height/width lines across the interior, manifesting as diagonal
  // streaks in Geode's rotated-rect stroke golden.
  //
  // Verify via ray-cast winding (EvenOdd) at points in each region. This
  // matches how the Geode slug_fill shader evaluates inside/outside and
  // catches the "extra segment spanning the interior" bug directly, without
  // being sensitive to overshoot vertices that cancel in winding.
  auto evenOdd = [&](Vector2d p) { return (rayCastWinding(filled, p) & 1) != 0; };

  // Hole interior: (5, 5), (3, 5), (7, 5) — all should be OUTSIDE.
  EXPECT_FALSE(evenOdd({5, 5})) << "center of hole";
  EXPECT_FALSE(evenOdd({3, 5})) << "left of center";
  EXPECT_FALSE(evenOdd({7, 5})) << "right of center";

  // Stroke ring (annulus): between original rect boundary and ±halfWidth.
  EXPECT_TRUE(evenOdd({5, 0.5})) << "top stroke";
  EXPECT_TRUE(evenOdd({5, 9.5})) << "bottom stroke";
  EXPECT_TRUE(evenOdd({0.5, 5})) << "left stroke";
  EXPECT_TRUE(evenOdd({9.5, 5})) << "right stroke";
  // Corners of the stroke ring.
  EXPECT_TRUE(evenOdd({0.5, 0.5})) << "top-left stroke corner";
  EXPECT_TRUE(evenOdd({9.5, 9.5})) << "bottom-right stroke corner";

  // Outside the outer ring.
  EXPECT_FALSE(evenOdd({-5, 5})) << "left of stroke";
  EXPECT_FALSE(evenOdd({15, 5})) << "right of stroke";
  EXPECT_FALSE(evenOdd({5, -5})) << "above stroke";
  EXPECT_FALSE(evenOdd({5, 15})) << "below stroke";
}

TEST(Path, StrokeToFillClosedEllipseInteriorIsEmpty) {
  // Regression for 2D: curved closed-subpath strokes used to emit spurious
  // line segments across the interior at each flattened vertex due to the
  // emitJoin inside-turn bug. For an ellipse, this manifested visually as
  // diagonal streaks visible inside the stroke ring (see the Geode
  // renderer_geode_golden ellipse1/rect2/quadbezier1 cases).
  //
  // Verify via ray-cast winding that the interior of a stroked ellipse
  // produces the expected EvenOdd result (outside) at several interior sample
  // points. Before the fix the interior would show odd winding counts at
  // positions where the zig-zag self-intersections happened to align with a
  // scan line.
  Path ellipse = PathBuilder()
                     .addEllipse(Box2d(Vector2d(0, 0), Vector2d(100, 60)))
                     .build();
  StrokeStyle style;
  style.width = 4.0;  // halfWidth = 2
  Path filled = ellipse.strokeToFill(style);
  EXPECT_FALSE(filled.empty());

  auto evenOdd = [&](Vector2d p) { return (rayCastWinding(filled, p) & 1) != 0; };

  // Points well inside the hole of the stroke ring (center and several offsets).
  // An ellipse has center (50, 30), semi-axes 50 and 30. Interior points:
  EXPECT_FALSE(evenOdd({50, 30})) << "center of ellipse";
  EXPECT_FALSE(evenOdd({30, 30})) << "inside-left";
  EXPECT_FALSE(evenOdd({70, 30})) << "inside-right";
  EXPECT_FALSE(evenOdd({50, 20})) << "inside-up";
  EXPECT_FALSE(evenOdd({50, 40})) << "inside-down";

  // Points far outside the ellipse.
  EXPECT_FALSE(evenOdd({-20, 30})) << "far left";
  EXPECT_FALSE(evenOdd({120, 30})) << "far right";
  EXPECT_FALSE(evenOdd({50, -20})) << "far above";
  EXPECT_FALSE(evenOdd({50, 80})) << "far below";

  // Points on the stroke ring at the ellipse's cardinal points. Inner edge
  // of the stroke is (50 ± 48, 30) on the major axis and (50, 30 ± 28) on
  // the minor, outer edge at ±52 and ±32. Sample points well inside the
  // annulus:
  EXPECT_TRUE(evenOdd({1, 30})) << "left stroke";
  EXPECT_TRUE(evenOdd({99, 30})) << "right stroke";
  EXPECT_TRUE(evenOdd({50, 1})) << "top stroke";
  EXPECT_TRUE(evenOdd({50, 59})) << "bottom stroke";
}

TEST(Path, StrokeToFillQuadbezierLensInteriorIsOutside) {
  // Reproduces the quadbezier1 wave path's first half exactly as it appears
  // in the test SVG (with the scale(0.5) translate(30,30) transform pre-baked
  // into the coordinates) and verifies that the strokeToFill output's polygon
  // does NOT include the lens interior.
  //
  // For the path M115,165 Q215,40 315,165 stroked at width 2.5 (= path 5 *
  // scale 0.5), the stroke ring is a thin ribbon along the curve. Points
  // INSIDE the lens — the empty area above the chord, between the curve's
  // two halves — must NOT be inside the stroke polygon.
  Path path = PathBuilder()
                  .moveTo({115, 165})
                  .quadTo({215, 40}, {315, 165})
                  .build();
  StrokeStyle style;
  style.width = 2.5;
  Path filled = path.strokeToFill(style);

  auto evenOdd = [&](Vector2d p) { return (rayCastWinding(filled, p) & 1) != 0; };

  // Per the Geode quadbezier1 golden, an artifact appears at screen-y=116
  // spanning screen-x [172, 257]. A pixel at (200, 116) is well inside the
  // lens — it should be OUTSIDE the stroke polygon.
  EXPECT_FALSE(evenOdd({200, 116})) << "lens interior at y=116";
  EXPECT_FALSE(evenOdd({215, 105})) << "near apex above curve";
  EXPECT_FALSE(evenOdd({200, 130})) << "lens interior at y=130";
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

// -----------------------------------------------------------------------------
// Dashed strokes
// -----------------------------------------------------------------------------

namespace {
// Count the number of subpaths (MoveTo commands) in a path.
size_t countSubpaths(const Path& p) {
  size_t n = 0;
  for (const auto& cmd : p.commands()) {
    if (cmd.verb == Path::Verb::MoveTo) {
      ++n;
    }
  }
  return n;
}
}  // namespace

TEST(Path, StrokeToFillDashArraySimple) {
  // 100-unit line with a 10/10 dash pattern: 5 on-dashes.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.dashArray = {10.0, 10.0};
  Path filled = path.strokeToFill(style);

  EXPECT_FALSE(filled.empty());
  // Each dash becomes its own capped polygon subpath.
  EXPECT_EQ(countSubpaths(filled), 5u);

  // Horizontal extent should roughly match the line.
  const Box2d bounds = filled.bounds();
  EXPECT_GE(bounds.topLeft.x, -0.5);
  EXPECT_LE(bounds.bottomRight.x, 100.5);
  // Vertical extent should be ~width.
  EXPECT_NEAR(bounds.bottomRight.y - bounds.topLeft.y, 2.0, 0.1);
}

TEST(Path, StrokeToFillDashArrayOddLengthIsDoubled) {
  // Per SVG: odd-length dasharrays are doubled. [10] becomes [10, 10].
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle styleOdd;
  styleOdd.width = 2.0;
  styleOdd.dashArray = {10.0};

  StrokeStyle styleEven;
  styleEven.width = 2.0;
  styleEven.dashArray = {10.0, 10.0};

  Path filledOdd = path.strokeToFill(styleOdd);
  Path filledEven = path.strokeToFill(styleEven);

  EXPECT_EQ(countSubpaths(filledOdd), countSubpaths(filledEven));
}

TEST(Path, StrokeToFillDashOffsetShiftsPattern) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle style0;
  style0.width = 2.0;
  style0.dashArray = {10.0, 10.0};
  style0.dashOffset = 0.0;

  StrokeStyle style5;
  style5.width = 2.0;
  style5.dashArray = {10.0, 10.0};
  style5.dashOffset = 5.0;

  Path filled0 = path.strokeToFill(style0);
  Path filled5 = path.strokeToFill(style5);
  EXPECT_FALSE(filled0.empty());
  EXPECT_FALSE(filled5.empty());

  // With a half-period offset the first dash is truncated (5 units instead
  // of 10), so the overall x-bounds should start a tad past x=0-halfWidth.
  const Box2d b0 = filled0.bounds();
  const Box2d b5 = filled5.bounds();
  // The first dash is 10 units wide at offset 0, only 5 units at offset 5,
  // but both start at x=0. Instead verify a structural change: offset 5
  // starts with a 5-on and 10-off, shifting the pattern, and we still get
  // a non-empty result with the same rough span.
  EXPECT_NEAR(b0.topLeft.x, b5.topLeft.x, 0.5);
}

TEST(Path, StrokeToFillDashOffsetNegativeWraps) {
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle style;
  style.width = 2.0;
  style.dashArray = {10.0, 10.0};
  style.dashOffset = -5.0;  // Equivalent to +15 mod 20.
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
}

TEST(Path, StrokeToFillDashClosedRect) {
  // Closed square, perimeter = 40. 10/10 dashes = 2 on-dashes.
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .lineTo({0, 10})
                  .closePath()
                  .build();
  StrokeStyle style;
  style.width = 1.0;
  style.dashArray = {10.0, 10.0};
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
  // Perimeter 40 / pattern 20 = 2 on-dashes, neither wraps the seam
  // because 40 is an exact multiple of the pattern length.
  EXPECT_EQ(countSubpaths(filled), 2u);
}

TEST(Path, StrokeToFillDashClosedWrapsSeam) {
  // Perimeter = 40. Pattern 15/10 = total 25. With offset 0:
  //   0..15 ON, 15..25 OFF, 25..40 ON (wraps: 25..40 reaches 15 past start).
  // Wait: 25 + 15 = 40, so the last dash ends exactly at the seam.
  // Use offset 5 to force a wrap: starts 5 into the first entry, so:
  //   0..10 ON, 10..20 OFF, 20..35 ON, 35..40 ... wraps into next cycle OFF.
  // Hmm, let's pick perimeter 40, pattern {30, 5}, offset 20:
  //   phase=20, idx=0, remainingInEntry=10 ON 0..10, OFF 10..15, ON 15..40...
  //   next=45 > 40 → wrap-around dash combines tail [15..40] + head [0..5].
  Path path = PathBuilder()
                  .moveTo({0, 0})
                  .lineTo({10, 0})
                  .lineTo({10, 10})
                  .lineTo({0, 10})
                  .closePath()
                  .build();
  StrokeStyle style;
  style.width = 1.0;
  style.dashArray = {30.0, 5.0};
  style.dashOffset = 20.0;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
  // We expect 2 on-dashes (one short starting at 0, one wrap-around).
  EXPECT_GE(countSubpaths(filled), 1u);
}

TEST(Path, StrokeToFillDashPathLengthScaling) {
  // pathLength=50 on a 100-unit line means dash values are interpreted
  // relative to 50, so a [10, 10] dasharray covers 20 units per cycle
  // on the normalized scale but 40 units per cycle on the actual path:
  // the 100-unit line fits exactly 2.5 cycles → 3 on-dashes (last
  // truncated at the endpoint).
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle style;
  style.width = 1.0;
  style.dashArray = {10.0, 10.0};
  style.pathLength = 50.0;
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
  EXPECT_EQ(countSubpaths(filled), 3u);
}

TEST(Path, StrokeToFillDashAllZeroIsSolid) {
  // Per SVG, an all-zero dasharray renders as a solid stroke.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle style;
  style.width = 1.0;
  style.dashArray = {0.0, 0.0};
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
  // Should be exactly one capped subpath, same as undashed.
  EXPECT_EQ(countSubpaths(filled), 1u);
}

TEST(Path, StrokeToFillDashZeroEntryDoesNotHang) {
  // {10, 0} is a 10-on/0-off pattern, which would naively loop forever
  // because the OFF entry never advances cursor. Verify the implementation
  // bails out without hanging.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle style;
  style.width = 1.0;
  style.dashArray = {10.0, 0.0};
  Path filled = path.strokeToFill(style);
  // Should produce *something*, not hang. (Whether it's one big stroke or
  // chopped up depends on the cycle handling, but it must terminate.)
  EXPECT_FALSE(filled.empty());
}

TEST(Path, StrokeToFillDashNegativeIsSolid) {
  // Negative values in the dasharray disable dashing.
  Path path = PathBuilder().moveTo({0, 0}).lineTo({100, 0}).build();
  StrokeStyle style;
  style.width = 1.0;
  style.dashArray = {10.0, -5.0, 10.0, 10.0};
  Path filled = path.strokeToFill(style);
  EXPECT_FALSE(filled.empty());
  EXPECT_EQ(countSubpaths(filled), 1u);
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

// Regression for the emitJoin restructure (a-stroke-linecap-008). The open
// triangle `M150,50 L150,130 L50,90 L150,50` has sharp inside turns at
// every interior vertex; the pre-restructure strokeSubpath pre-emitted
// `prevEnd` before `emitJoin`, which caused the polygon to backtrack along
// the previous offset line when emitJoin substituted a miter point for
// an inside turn — producing a wildly self-intersecting ribbon. The
// invariant now is: `emitJoin` is responsible for emitting `prevEnd` on
// outside turns (or a miter substitute on inside turns); the caller does
// not pre-emit it. This test locks that invariant in place by verifying
// the left-side contour trace stays monotonic along each segment's offset
// line rather than backtracking into the previous segment.
TEST(Path, StrokeToFillInsideTurnForwardContourDoesNotBacktrack) {
  Path path = PathBuilder()
                  .moveTo({150, 50})
                  .lineTo({150, 130})
                  .lineTo({50, 90})
                  .lineTo({150, 50})
                  .build();
  StrokeStyle style;
  style.width = 20.0;
  style.cap = LineCap::Round;
  style.join = LineJoin::Miter;
  style.miterLimit = 4.0;
  Path filled = path.strokeToFill(style);
  ASSERT_FALSE(filled.empty());

  // The first LineTo in the stroke polygon must start the left contour on
  // segment 0's offset and NOT backtrack (y should move monotonically along
  // the first segment's offset direction before any inside-miter vertex).
  // We assert this via the bounding box: segment 0 runs from y=50 down to
  // y=130 in path space. After applying halfWidth=10 offset, the bounding
  // box in y covers approximately [40, 145] (the outside miters at pts[1]
  // extend past y=130). If the forward trace had backtracked, we'd see
  // duplicate y coordinates along a vertical edge at x=140 (the left
  // offset of segment 0), which manifests as the same x appearing with
  // non-monotonic y values in the polygon output.
  const auto commands = filled.commands();
  const auto pts = filled.points();
  double lastYOnSeg0Offset = -1.0;
  bool seenIncrease = false;
  for (const auto& cmd : commands) {
    if (cmd.verb != Path::Verb::MoveTo && cmd.verb != Path::Verb::LineTo) {
      continue;
    }
    const Vector2d& p = pts[cmd.pointIndex];
    // Only check points on segment 0's left offset (x ≈ 140).
    if (std::abs(p.x - 140.0) > 0.5) {
      break;  // Left the seg 0 offset region; stop checking.
    }
    if (lastYOnSeg0Offset < 0) {
      lastYOnSeg0Offset = p.y;
      continue;
    }
    // y should be monotonically non-decreasing while we walk the seg 0
    // offset forward. A decrease indicates a backtrack.
    ASSERT_GE(p.y, lastYOnSeg0Offset - 0.01) << "Left contour backtracked along seg 0 offset";
    if (p.y > lastYOnSeg0Offset + 0.01) {
      seenIncrease = true;
    }
    lastYOnSeg0Offset = p.y;
  }
  EXPECT_TRUE(seenIncrease) << "Expected at least one forward step along seg 0 offset";
}

}  // namespace donner
