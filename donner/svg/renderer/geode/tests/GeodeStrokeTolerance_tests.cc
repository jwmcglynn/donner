#include "donner/svg/renderer/geode/GeodeStrokeTolerance.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::geode {
namespace {

const Vector2d kCenter = Vector2d(40.0, 40.0);
constexpr double kRadius = 16.0;
constexpr double kStrokeWidth = 3.0;

/// A circle built the way `PathBuilder::addCircle` builds one: four kappa
/// cubics. This is the exact shape the editor's selection chrome draws, and the
/// shape that showed as a visible polygon before stroke flattening became
/// device-aware.
Path Circle() {
  return PathBuilder().addCircle(kCenter, kRadius).build();
}

StrokeStyle CircleStroke() {
  StrokeStyle style;
  style.width = kStrokeWidth;
  return style;
}

/// One edge of a flattened outline polygon.
struct Segment {
  Vector2d from;
  Vector2d to;
};

/// Expand a `strokeToFill` result (MoveTo / LineTo / ClosePath only) into its
/// polygon edges, including the implicit closing edge of each subpath.
std::vector<Segment> OutlineSegments(const Path& outline) {
  std::vector<Segment> segments;
  Vector2d subpathStart;
  Vector2d previous;
  bool open = false;
  for (const Path::Command& command : outline.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        subpathStart = outline.points()[command.pointIndex];
        previous = subpathStart;
        open = true;
        break;
      case Path::Verb::LineTo: {
        const Vector2d point = outline.points()[command.pointIndex];
        if (open) {
          segments.push_back({previous, point});
        }
        previous = point;
        break;
      }
      case Path::Verb::ClosePath:
        if (open) {
          segments.push_back({previous, subpathStart});
        }
        open = false;
        break;
      default:
        // `strokeToFill` never emits curve verbs; a curve here would mean the
        // stroker stopped flattening, which the assertions below would miss.
        ADD_FAILURE() << "Stroke outline contained an unexpected curve verb.";
        break;
    }
  }
  return segments;
}

/// Distance from \p point to the closest point on \p segment.
double DistanceToSegment(const Vector2d& point, const Segment& segment) {
  const Vector2d delta = segment.to - segment.from;
  const double lengthSquared = delta.lengthSquared();
  if (lengthSquared <= 0.0) {
    return (point - segment.from).length();
  }
  const double t = std::clamp(((point - segment.from).dot(delta)) / lengthSquared, 0.0, 1.0);
  return (point - (segment.from + delta * t)).length();
}

/**
 * Largest faceting error of \p outline, in DEVICE pixels, measured against
 * \p reference: an outline of the same geometry flattened far more finely than
 * any assertion here needs.
 *
 * The reference is the right yardstick because the true target shape is the
 * offset of the source path's cubics, not an ideal circle: `PathBuilder`
 * approximates a circle with four kappa cubics, which are themselves off a true
 * circle by ~2.7e-4 of the radius. Measuring against a converged flattening of
 * the same cubics isolates the flattening error, which is the quantity the
 * tolerance controls.
 *
 * The error is sampled at segment MIDPOINTS. A midpoint sits at the deepest
 * point of the chord's sagitta, which is exactly the faceting a viewer sees,
 * whereas a miter-join vertex sits slightly off the reference for reasons that
 * have nothing to do with flattening.
 */
double MaxDeviceFacetError(const Path& outline, const Path& reference, double scale) {
  const std::vector<Segment> outlineSegments = OutlineSegments(outline);
  const std::vector<Segment> referenceSegments = OutlineSegments(reference);
  if (outlineSegments.empty() || referenceSegments.empty()) {
    ADD_FAILURE() << "Outline or reference had no segments to compare.";
    return 0.0;
  }

  double worst = 0.0;
  for (const Segment& segment : outlineSegments) {
    const Vector2d midpoint = (segment.from + segment.to) * 0.5;
    double closest = std::numeric_limits<double>::max();
    for (const Segment& referenceSegment : referenceSegments) {
      closest = std::min(closest, DistanceToSegment(midpoint, referenceSegment));
    }
    worst = std::max(worst, closest * scale);
  }
  return worst;
}

/// Converged stroke outline of `Circle()`: flattened an order of magnitude
/// finer than the tightest tolerance the derivation can produce, so it stands in
/// for the exact offset curve.
Path ConvergedCircleOutline() {
  return Circle().strokeToFill(CircleStroke(), kMinStrokeFlattenTolerance * 0.1);
}

// ---------------------------------------------------------------------------
// MaxAbsScaleFactor
// ---------------------------------------------------------------------------

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorOfIdentityIsOne) {
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d()), 1.0);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorOfUniformScale) {
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(8.0)), 8.0);
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(0.25)), 0.25);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorIgnoresTranslation) {
  const Transform2d transform =
      Transform2d::Scale(4.0) * Transform2d::Translate(Vector2d(1000.0, -2000.0));
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(transform), 4.0);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorIsRotationInvariant) {
  const Transform2d transform = Transform2d::Scale(6.0) * Transform2d::Rotate(0.7);
  EXPECT_NEAR(MaxAbsScaleFactor(transform), 6.0, 1e-9);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorTakesTheLargerAxisOfNonUniformScale) {
  // The bound must hold for every direction, so the LARGER axis wins: an error
  // vector aligned with the 32x axis is magnified 32x in device space.
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(Vector2d(1.0, 32.0))), 32.0);
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(Vector2d(32.0, 1.0))), 32.0);
}

TEST(GeodeStrokeTolerance, MaxAbsScaleFactorOfDegenerateTransformIsZero) {
  EXPECT_DOUBLE_EQ(MaxAbsScaleFactor(Transform2d::Scale(0.0)), 0.0);
}

// ---------------------------------------------------------------------------
// StrokeFlattenScaleBucket
// ---------------------------------------------------------------------------

TEST(GeodeStrokeTolerance, ScaleBucketRoundsUpToAPowerOfTwo) {
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(1.0), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(8.0), 8.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(8.5), 16.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(15.9), 16.0);
}

/// Refine-only: minified geometry keeps the path-local default rather than
/// being tessellated more coarsely. Coarsening would stay inside the
/// device-pixel budget but would perturb every downscaled render (filter
/// sub-renders, pattern tiles, thumbnails) for no correctness gain.
TEST(GeodeStrokeTolerance, ScaleBucketFloorsAtOneForMinifiedTransforms) {
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(0.3), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(0.99), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(1e-6), 1.0);
}

TEST(GeodeStrokeTolerance, ScaleBucketIsStableWithinABucket) {
  // The point of bucketing: a continuous zoom gesture must not re-derive (and
  // therefore re-flatten) the stroke outline on every frame.
  const double bucket = StrokeFlattenScaleBucket(9.0);
  for (double scale = 8.01; scale < 16.0; scale += 0.37) {
    EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(scale), bucket) << "scale = " << scale;
  }
}

TEST(GeodeStrokeTolerance, ScaleBucketRejectsDegenerateInput) {
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(0.0), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(-4.0), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(std::numeric_limits<double>::quiet_NaN()), 1.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenScaleBucket(std::numeric_limits<double>::infinity()), 1.0);
}

// ---------------------------------------------------------------------------
// StrokeFlattenToleranceFor
// ---------------------------------------------------------------------------

TEST(GeodeStrokeTolerance, ToleranceAtIdentityMatchesTheDevicePixelTarget) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d()), kStrokeFlattenDevicePixels);
}

TEST(GeodeStrokeTolerance, ToleranceShrinksInverselyWithScale) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(8.0)),
                   kStrokeFlattenDevicePixels / 8.0);
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(32.0)),
                   kStrokeFlattenDevicePixels / 32.0);
}

TEST(GeodeStrokeTolerance, ToleranceNeverCoarsensBelowTheLocalDefault) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(0.5)), kStrokeFlattenDevicePixels);
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(1e-12)),
                   kStrokeFlattenDevicePixels);
  EXPECT_DOUBLE_EQ(kMaxStrokeFlattenTolerance, Path::kLocalFlattenTolerance);
}

TEST(GeodeStrokeTolerance, ToleranceIsClampedForDegenerateAndExtremeTransforms) {
  EXPECT_DOUBLE_EQ(StrokeFlattenToleranceFor(Transform2d::Scale(0.0)), kStrokeFlattenDevicePixels);
  EXPECT_GE(StrokeFlattenToleranceFor(Transform2d::Scale(1e12)), kMinStrokeFlattenTolerance);
  EXPECT_LE(StrokeFlattenToleranceFor(Transform2d::Scale(1e-12)), kMaxStrokeFlattenTolerance);
}

// ---------------------------------------------------------------------------
// The invariant itself.
// ---------------------------------------------------------------------------

/// The contract every renderer-side stroke call site must satisfy: flattening a
/// curve with the derived tolerance keeps the chord error under
/// `kStrokeFlattenDevicePixels` DEVICE pixels at the transform the geometry is
/// actually drawn with. Curves rendering as visible line segments is a
/// correctness violation, so this bound is a gate, not a quality target.
TEST(GeodeStrokeTolerance, DerivedToleranceBoundsDeviceChordErrorAtEveryScale) {
  const Path circle = Circle();
  const Path reference = ConvergedCircleOutline();
  for (const double scale : {1.0, 1.7, 4.0, 8.0, 32.0, 512.0}) {
    const Transform2d deviceFromLocal = Transform2d::Scale(scale);
    const double tolerance = StrokeFlattenToleranceFor(deviceFromLocal);
    const Path stroked = circle.strokeToFill(CircleStroke(), tolerance);
    ASSERT_FALSE(stroked.empty()) << "scale = " << scale;

    EXPECT_LE(MaxDeviceFacetError(stroked, reference, scale), kStrokeFlattenDevicePixels)
        << "Stroke outline faceting exceeded the device-pixel bound at scale " << scale;
  }
}

/// Companion to the test above: it records WHY the derivation is needed. A
/// scale-blind path-local tolerance (the historical default) blows the same
/// device-pixel bound wide open once the view is zoomed in, which is exactly
/// how a circle turns into a visible chain of segments on screen.
TEST(GeodeStrokeTolerance, FixedLocalToleranceViolatesTheDeviceBoundWhenZoomed) {
  const Path circle = Circle();
  const Path reference = ConvergedCircleOutline();
  const Path stroked = circle.strokeToFill(CircleStroke(), Path::kLocalFlattenTolerance);
  ASSERT_FALSE(stroked.empty());

  EXPECT_LE(MaxDeviceFacetError(stroked, reference, 1.0), kStrokeFlattenDevicePixels)
      << "The local-space tolerance is correct at 1:1 - that is why the defect was invisible "
         "until the editor started drawing document-space chrome under a zoom transform.";
  EXPECT_GT(MaxDeviceFacetError(stroked, reference, 32.0), kStrokeFlattenDevicePixels * 4.0)
      << "A scale-blind tolerance must be shown to fail the bound, otherwise this suite could "
         "pass without the device-aware derivation doing anything.";
}

/// Finer flattening means more outline points. This is the property the
/// renderer-level counter assertions in `RendererGeode_tests.cc` lean on.
TEST(GeodeStrokeTolerance, DerivedToleranceProducesMoreOutlinePointsAsScaleGrows) {
  const Path circle = Circle();
  const Path atOne = circle.strokeToFill(CircleStroke(), StrokeFlattenToleranceFor(Transform2d()));
  const Path atThirtyTwo =
      circle.strokeToFill(CircleStroke(), StrokeFlattenToleranceFor(Transform2d::Scale(32.0)));

  EXPECT_GT(atThirtyTwo.points().size(), atOne.points().size() * 4u);
}

}  // namespace
}  // namespace donner::geode
