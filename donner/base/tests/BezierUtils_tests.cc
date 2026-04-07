#include "donner/base/BezierUtils.h"

#include <gtest/gtest.h>

#include <cmath>

namespace donner {

namespace {

/// Helper: check that two Vector2d values are approximately equal.
void ExpectNear(const Vector2d& actual, const Vector2d& expected, double tolerance = 1e-9) {
  EXPECT_NEAR(actual.x, expected.x, tolerance) << "actual=" << actual << " expected=" << expected;
  EXPECT_NEAR(actual.y, expected.y, tolerance) << "actual=" << actual << " expected=" << expected;
}

}  // namespace

// =============================================================================
// EvalQuadratic
// =============================================================================

TEST(BezierUtils, EvalQuadraticAtT0ReturnsP0) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 0.0);
  ExpectNear(EvalQuadratic(p0, p1, p2, 0.0), p0);
}

TEST(BezierUtils, EvalQuadraticAtT1ReturnsP2) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 0.0);
  ExpectNear(EvalQuadratic(p0, p1, p2, 1.0), p2);
}

TEST(BezierUtils, EvalQuadraticAtT05) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(4.0, 0.0);
  // B(0.5) = 0.25*p0 + 0.5*p1 + 0.25*p2 = (0 + 0.5 + 1.0, 0 + 1.0 + 0) = (1.5, 1.0)
  ExpectNear(EvalQuadratic(p0, p1, p2, 0.5), Vector2d(1.5, 1.0));
}

TEST(BezierUtils, EvalQuadraticStraightLine) {
  // Collinear points: quadratic should be a straight line.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 1.0);
  const Vector2d p2(2.0, 2.0);
  ExpectNear(EvalQuadratic(p0, p1, p2, 0.25), Vector2d(0.5, 0.5));
  ExpectNear(EvalQuadratic(p0, p1, p2, 0.75), Vector2d(1.5, 1.5));
}

// =============================================================================
// EvalCubic
// =============================================================================

TEST(BezierUtils, EvalCubicAtT0ReturnsP0) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 2.0);
  const Vector2d p3(4.0, 0.0);
  ExpectNear(EvalCubic(p0, p1, p2, p3, 0.0), p0);
}

TEST(BezierUtils, EvalCubicAtT1ReturnsP3) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 2.0);
  const Vector2d p3(4.0, 0.0);
  ExpectNear(EvalCubic(p0, p1, p2, p3, 1.0), p3);
}

TEST(BezierUtils, EvalCubicAtT05) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(0.0, 1.0);
  const Vector2d p2(1.0, 1.0);
  const Vector2d p3(1.0, 0.0);
  // B(0.5) = (1/8)*p0 + (3/8)*p1 + (3/8)*p2 + (1/8)*p3
  //        = (0, 0) + (0, 3/8) + (3/8, 3/8) + (1/8, 0)
  //        = (0.5, 0.75)
  ExpectNear(EvalCubic(p0, p1, p2, p3, 0.5), Vector2d(0.5, 0.75));
}

TEST(BezierUtils, EvalCubicStraightLine) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 1.0);
  const Vector2d p2(2.0, 2.0);
  const Vector2d p3(3.0, 3.0);
  ExpectNear(EvalCubic(p0, p1, p2, p3, 0.25), Vector2d(0.75, 0.75));
  ExpectNear(EvalCubic(p0, p1, p2, p3, 0.5), Vector2d(1.5, 1.5));
}

// =============================================================================
// SplitQuadratic
// =============================================================================

TEST(BezierUtils, SplitQuadraticAtT0) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 0.0);

  auto [left, right] = SplitQuadratic(p0, p1, p2, 0.0);

  // Left: degenerate at p0.
  ExpectNear(left[0], p0);
  ExpectNear(left[1], p0);
  ExpectNear(left[2], p0);

  // Right: the full original curve.
  ExpectNear(right[0], p0);
  ExpectNear(right[1], p1);
  ExpectNear(right[2], p2);
}

TEST(BezierUtils, SplitQuadraticAtT1) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 0.0);

  auto [left, right] = SplitQuadratic(p0, p1, p2, 1.0);

  // Left: the full original curve.
  ExpectNear(left[0], p0);
  ExpectNear(left[1], p1);
  ExpectNear(left[2], p2);

  // Right: degenerate at p2.
  ExpectNear(right[0], p2);
  ExpectNear(right[1], p2);
  ExpectNear(right[2], p2);
}

TEST(BezierUtils, SplitQuadraticAtT05) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(2.0, 4.0);
  const Vector2d p2(4.0, 0.0);

  auto [left, right] = SplitQuadratic(p0, p1, p2, 0.5);

  // The split point should be EvalQuadratic at t=0.5.
  const Vector2d mid = EvalQuadratic(p0, p1, p2, 0.5);
  ExpectNear(left[2], mid);
  ExpectNear(right[0], mid);

  // Left starts at p0, right ends at p2.
  ExpectNear(left[0], p0);
  ExpectNear(right[2], p2);

  // Verify that evaluating the left half at t=0.5 gives the same as the original at t=0.25.
  ExpectNear(EvalQuadratic(left[0], left[1], left[2], 0.5),
             EvalQuadratic(p0, p1, p2, 0.25));

  // Verify that evaluating the right half at t=0.5 gives the same as the original at t=0.75.
  ExpectNear(EvalQuadratic(right[0], right[1], right[2], 0.5),
             EvalQuadratic(p0, p1, p2, 0.75));
}

TEST(BezierUtils, SplitQuadraticPreservesEvaluation) {
  // Split at an arbitrary parameter and verify the two halves produce the same curve.
  const Vector2d p0(1.0, 3.0);
  const Vector2d p1(5.0, 7.0);
  const Vector2d p2(9.0, 1.0);
  const double splitT = 0.3;

  auto [left, right] = SplitQuadratic(p0, p1, p2, splitT);

  // Points on the left half correspond to the original curve at t in [0, splitT].
  for (double u = 0.0; u <= 1.0; u += 0.1) {
    ExpectNear(EvalQuadratic(left[0], left[1], left[2], u),
               EvalQuadratic(p0, p1, p2, splitT * u), 1e-9);
  }

  // Points on the right half correspond to the original curve at t in [splitT, 1].
  for (double u = 0.0; u <= 1.0; u += 0.1) {
    ExpectNear(EvalQuadratic(right[0], right[1], right[2], u),
               EvalQuadratic(p0, p1, p2, splitT + (1.0 - splitT) * u), 1e-9);
  }
}

// =============================================================================
// SplitCubic
// =============================================================================

TEST(BezierUtils, SplitCubicAtT0) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 2.0);
  const Vector2d p3(4.0, 0.0);

  auto [left, right] = SplitCubic(p0, p1, p2, p3, 0.0);

  // Left: degenerate at p0.
  for (const auto& pt : left) {
    ExpectNear(pt, p0);
  }

  // Right: the original curve.
  ExpectNear(right[0], p0);
  ExpectNear(right[1], p1);
  ExpectNear(right[2], p2);
  ExpectNear(right[3], p3);
}

TEST(BezierUtils, SplitCubicAtT1) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 2.0);
  const Vector2d p3(4.0, 0.0);

  auto [left, right] = SplitCubic(p0, p1, p2, p3, 1.0);

  // Left: the original curve.
  ExpectNear(left[0], p0);
  ExpectNear(left[1], p1);
  ExpectNear(left[2], p2);
  ExpectNear(left[3], p3);

  // Right: degenerate at p3.
  for (const auto& pt : right) {
    ExpectNear(pt, p3);
  }
}

TEST(BezierUtils, SplitCubicAtT05) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(0.0, 4.0);
  const Vector2d p2(4.0, 4.0);
  const Vector2d p3(4.0, 0.0);

  auto [left, right] = SplitCubic(p0, p1, p2, p3, 0.5);

  // Split point should match EvalCubic at t=0.5.
  const Vector2d mid = EvalCubic(p0, p1, p2, p3, 0.5);
  ExpectNear(left[3], mid);
  ExpectNear(right[0], mid);

  // Endpoints preserved.
  ExpectNear(left[0], p0);
  ExpectNear(right[3], p3);
}

TEST(BezierUtils, SplitCubicPreservesEvaluation) {
  const Vector2d p0(1.0, 1.0);
  const Vector2d p1(2.0, 5.0);
  const Vector2d p2(6.0, 5.0);
  const Vector2d p3(8.0, 1.0);
  const double splitT = 0.7;

  auto [left, right] = SplitCubic(p0, p1, p2, p3, splitT);

  // Points on the left half correspond to the original curve at t in [0, splitT].
  for (double u = 0.0; u <= 1.0; u += 0.1) {
    ExpectNear(EvalCubic(left[0], left[1], left[2], left[3], u),
               EvalCubic(p0, p1, p2, p3, splitT * u), 1e-9);
  }

  // Points on the right half correspond to the original curve at t in [splitT, 1].
  for (double u = 0.0; u <= 1.0; u += 0.1) {
    ExpectNear(EvalCubic(right[0], right[1], right[2], right[3], u),
               EvalCubic(p0, p1, p2, p3, splitT + (1.0 - splitT) * u), 1e-9);
  }
}

// =============================================================================
// ApproximateCubicWithQuadratics
// =============================================================================

TEST(BezierUtils, ApproximateCubicStraightLine) {
  // A straight line should be approximated by a single quadratic.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 1.0);
  const Vector2d p2(2.0, 2.0);
  const Vector2d p3(3.0, 3.0);

  std::vector<Vector2d> out;
  ApproximateCubicWithQuadratics(p0, p1, p2, p3, 0.01, out);

  // Should produce exactly one quadratic: 2 elements (control, end).
  ASSERT_EQ(out.size(), 2u);
  ExpectNear(out[1], p3);  // End point is p3.
}

TEST(BezierUtils, ApproximateCubicSCurve) {
  // An S-curve should require multiple quadratics.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(0.0, 10.0);
  const Vector2d p2(10.0, -10.0);
  const Vector2d p3(10.0, 0.0);

  std::vector<Vector2d> out;
  ApproximateCubicWithQuadratics(p0, p1, p2, p3, 0.1, out);

  // Should produce more than one quadratic (more than 2 entries).
  EXPECT_GT(out.size(), 2u);
  // Output size should be even (pairs of control, end).
  EXPECT_EQ(out.size() % 2, 0u);
  // Last point should be close to p3.
  ExpectNear(out.back(), p3, 1e-6);
}

TEST(BezierUtils, ApproximateCubicToleranceAffectsCount) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(0.0, 10.0);
  const Vector2d p2(10.0, -10.0);
  const Vector2d p3(10.0, 0.0);

  std::vector<Vector2d> outFine;
  ApproximateCubicWithQuadratics(p0, p1, p2, p3, 0.01, outFine);

  std::vector<Vector2d> outCoarse;
  ApproximateCubicWithQuadratics(p0, p1, p2, p3, 10.0, outCoarse);

  // Finer tolerance should produce at least as many quadratics.
  EXPECT_GE(outFine.size(), outCoarse.size());
}

TEST(BezierUtils, ApproximateCubicChainContinuity) {
  // Verify that the quadratic chain is continuous: end of each segment is start of next.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 5.0);
  const Vector2d p2(5.0, -3.0);
  const Vector2d p3(8.0, 2.0);

  std::vector<Vector2d> out;
  ApproximateCubicWithQuadratics(p0, p1, p2, p3, 0.1, out);
  ASSERT_GE(out.size(), 2u);
  ASSERT_EQ(out.size() % 2, 0u);

  // The first quadratic starts at p0 and ends at out[1].
  // The second quadratic starts at out[1] and ends at out[3], etc.
  // Final endpoint should be p3.
  ExpectNear(out.back(), p3, 1e-6);
}

// =============================================================================
// QuadraticYExtrema
// =============================================================================

TEST(BezierUtils, QuadraticYExtremaMonotonic) {
  // Monotonically increasing Y: no extrema.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 1.0);
  const Vector2d p2(2.0, 2.0);

  auto result = QuadraticYExtrema(p0, p1, p2);
  EXPECT_EQ(result.size(), 0u);
}

TEST(BezierUtils, QuadraticYExtremaOneExtremum) {
  // Arch shape: Y goes up then down.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(2.0, 0.0);

  auto result = QuadraticYExtrema(p0, p1, p2);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_NEAR(result[0], 0.5, 1e-9);
}

TEST(BezierUtils, QuadraticYExtremaAsymmetric) {
  // Asymmetric arch: extremum is not at t=0.5.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 4.0);
  const Vector2d p2(2.0, 2.0);

  // t = (p0.y - p1.y) / (p0.y - 2*p1.y + p2.y) = (0 - 4) / (0 - 8 + 2) = -4 / -6 = 2/3
  auto result = QuadraticYExtrema(p0, p1, p2);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_NEAR(result[0], 2.0 / 3.0, 1e-9);
}

// =============================================================================
// CubicYExtrema
// =============================================================================

TEST(BezierUtils, CubicYExtremaMonotonic) {
  // Monotonically increasing Y: no extrema.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 1.0);
  const Vector2d p2(2.0, 2.0);
  const Vector2d p3(3.0, 3.0);

  auto result = CubicYExtrema(p0, p1, p2, p3);
  EXPECT_EQ(result.size(), 0u);
}

TEST(BezierUtils, CubicYExtremaOneExtremum) {
  // Symmetric arch: one Y extremum.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 3.0);
  const Vector2d p2(3.0, 3.0);
  const Vector2d p3(4.0, 0.0);

  auto result = CubicYExtrema(p0, p1, p2, p3);
  // Should have exactly 1 extremum due to the symmetric arch shape.
  // The Y derivative is 0 at the peak. With this symmetric curve, the peak is at t=0.5.
  ASSERT_EQ(result.size(), 1u);
  EXPECT_NEAR(result[0], 0.5, 1e-9);
}

TEST(BezierUtils, CubicYExtremaTwoExtrema) {
  // S-curve: Y goes up, down, then up. Should have 2 Y extrema.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(0.0, 10.0);
  const Vector2d p2(10.0, -10.0);
  const Vector2d p3(10.0, 0.0);

  auto result = CubicYExtrema(p0, p1, p2, p3);
  ASSERT_EQ(result.size(), 2u);
  // Both should be in (0, 1) and sorted.
  EXPECT_GT(result[0], 0.0);
  EXPECT_LT(result[0], result[1]);
  EXPECT_LT(result[1], 1.0);
}

// =============================================================================
// QuadraticBounds
// =============================================================================

TEST(BezierUtils, QuadraticBoundsContainsEndpoints) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 2.0);
  const Vector2d p2(3.0, 0.0);

  Box2d box = QuadraticBounds(p0, p1, p2);
  EXPECT_TRUE(box.contains(p0));
  EXPECT_TRUE(box.contains(p2));
}

TEST(BezierUtils, QuadraticBoundsTighterThanControlHull) {
  // The control point p1 is outside the curve, so the tight bounds should be smaller
  // than the control hull.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 4.0);
  const Vector2d p2(2.0, 0.0);

  Box2d tight = QuadraticBounds(p0, p1, p2);

  // Control hull would be (0,0) to (2,4).
  // The tight box should have a Y max less than 4.0 since the curve doesn't reach the control
  // point.
  EXPECT_LT(tight.bottomRight.y, 4.0);
  EXPECT_GT(tight.bottomRight.y, 0.0);  // But the curve does go above y=0.

  // The Y extremum is at t=0.5 for this symmetric case.
  // B(0.5) = 0.25*(0,0) + 0.5*(1,4) + 0.25*(2,0) = (1.0, 2.0)
  EXPECT_NEAR(tight.bottomRight.y, 2.0, 1e-9);
}

TEST(BezierUtils, QuadraticBoundsStraightLine) {
  // For a straight line, bounds should exactly contain the endpoints.
  const Vector2d p0(1.0, 2.0);
  const Vector2d p1(2.0, 3.0);
  const Vector2d p2(3.0, 4.0);

  Box2d box = QuadraticBounds(p0, p1, p2);
  EXPECT_NEAR(box.topLeft.x, 1.0, 1e-9);
  EXPECT_NEAR(box.topLeft.y, 2.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.x, 3.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.y, 4.0, 1e-9);
}

// =============================================================================
// CubicBounds
// =============================================================================

TEST(BezierUtils, CubicBoundsContainsEndpoints) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 3.0);
  const Vector2d p2(3.0, 3.0);
  const Vector2d p3(4.0, 0.0);

  Box2d box = CubicBounds(p0, p1, p2, p3);
  EXPECT_TRUE(box.contains(p0));
  EXPECT_TRUE(box.contains(p3));
}

TEST(BezierUtils, CubicBoundsTighterThanControlHull) {
  // Control hull would include (1,3) and (3,3) as corners.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(0.0, 4.0);
  const Vector2d p2(4.0, 4.0);
  const Vector2d p3(4.0, 0.0);

  Box2d tight = CubicBounds(p0, p1, p2, p3);

  // Control hull Y max is 4.0, but the curve peak is lower.
  // B(0.5).y = (1/8)*0 + (3/8)*4 + (3/8)*4 + (1/8)*0 = 3.0
  // So tight box Y max should be 3.0, which is less than 4.0.
  EXPECT_LT(tight.bottomRight.y, 4.0);
  EXPECT_NEAR(tight.bottomRight.y, 3.0, 1e-9);
}

TEST(BezierUtils, CubicBoundsSCurve) {
  // S-curve: bounds should capture extrema in both directions.
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(0.0, 10.0);
  const Vector2d p2(10.0, -10.0);
  const Vector2d p3(10.0, 0.0);

  Box2d box = CubicBounds(p0, p1, p2, p3);

  // The curve goes above Y=0 and below Y=0, so the box should span negative to positive Y.
  EXPECT_LT(box.topLeft.y, 0.0);
  EXPECT_GT(box.bottomRight.y, 0.0);

  // X should span from 0 to 10 since the endpoints are at x=0 and x=10.
  EXPECT_NEAR(box.topLeft.x, 0.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.x, 10.0, 1e-9);
}

TEST(BezierUtils, CubicBoundsStraightLine) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(1.0, 1.0);
  const Vector2d p2(2.0, 2.0);
  const Vector2d p3(3.0, 3.0);

  Box2d box = CubicBounds(p0, p1, p2, p3);
  EXPECT_NEAR(box.topLeft.x, 0.0, 1e-9);
  EXPECT_NEAR(box.topLeft.y, 0.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.x, 3.0, 1e-9);
  EXPECT_NEAR(box.bottomRight.y, 3.0, 1e-9);
}

// =============================================================================
// EvalQuadratic / EvalCubic consistency with SplitQuadratic / SplitCubic
// =============================================================================

TEST(BezierUtils, EvalQuadraticMatchesSplitMidpoint) {
  const Vector2d p0(2.0, 1.0);
  const Vector2d p1(4.0, 6.0);
  const Vector2d p2(8.0, 3.0);

  auto [left, right] = SplitQuadratic(p0, p1, p2, 0.5);
  const Vector2d evalMid = EvalQuadratic(p0, p1, p2, 0.5);
  ExpectNear(left[2], evalMid);
  ExpectNear(right[0], evalMid);
}

TEST(BezierUtils, EvalCubicMatchesSplitMidpoint) {
  const Vector2d p0(0.0, 0.0);
  const Vector2d p1(2.0, 8.0);
  const Vector2d p2(6.0, 8.0);
  const Vector2d p3(8.0, 0.0);

  auto [left, right] = SplitCubic(p0, p1, p2, p3, 0.5);
  const Vector2d evalMid = EvalCubic(p0, p1, p2, p3, 0.5);
  ExpectNear(left[3], evalMid);
  ExpectNear(right[0], evalMid);
}

// =============================================================================
// CubicBounds: contains all evaluated points
// =============================================================================

TEST(BezierUtils, CubicBoundsContainsAllEvaluatedPoints) {
  const Vector2d p0(0.0, 5.0);
  const Vector2d p1(3.0, -2.0);
  const Vector2d p2(7.0, 12.0);
  const Vector2d p3(10.0, 5.0);

  Box2d box = CubicBounds(p0, p1, p2, p3);

  // Sample many points on the curve and verify they're all inside the bounding box.
  for (int i = 0; i <= 100; ++i) {
    const double t = static_cast<double>(i) / 100.0;
    const Vector2d pt = EvalCubic(p0, p1, p2, p3, t);
    EXPECT_TRUE(box.contains(pt)) << "Point at t=" << t << " " << pt << " not in box " << box;
  }
}

TEST(BezierUtils, QuadraticBoundsContainsAllEvaluatedPoints) {
  const Vector2d p0(0.0, 5.0);
  const Vector2d p1(5.0, -3.0);
  const Vector2d p2(10.0, 7.0);

  Box2d box = QuadraticBounds(p0, p1, p2);

  for (int i = 0; i <= 100; ++i) {
    const double t = static_cast<double>(i) / 100.0;
    const Vector2d pt = EvalQuadratic(p0, p1, p2, t);
    EXPECT_TRUE(box.contains(pt)) << "Point at t=" << t << " " << pt << " not in box " << box;
  }
}

}  // namespace donner
