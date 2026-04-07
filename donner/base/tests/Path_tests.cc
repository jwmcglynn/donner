#include "donner/base/Path.h"

#include <gtest/gtest.h>

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

}  // namespace donner
