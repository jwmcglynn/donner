#include "donner/base/PathOps.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/BezierUtils.h"

namespace donner {
namespace {

using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;

Path RectPath(double x, double y, double width, double height) {
  return PathBuilder().addRect(Box2d::FromXYWH(x, y, width, height)).build();
}

Path CirclePath(Vector2d center, double radius) {
  return PathBuilder().addCircle(center, radius).build();
}

Path CircleDonutPath(Vector2d center, double outerRadius, double innerRadius) {
  return PathBuilder().addCircle(center, outerRadius).addCircle(center, innerRadius).build();
}

Path NonZeroRectDonutPath() {
  return PathBuilder()
      .addRect(Box2d::FromXYWH(0, 0, 100, 100))
      .moveTo({25, 25})
      .lineTo({25, 75})
      .lineTo({75, 75})
      .lineTo({75, 25})
      .closePath()
      .build();
}

PathBooleanInput Input(Path path) {
  return PathBooleanInput{.path = std::move(path)};
}

PathBooleanResult Boolean(PathBooleanOp op, std::initializer_list<PathBooleanInput> inputs) {
  const std::vector<PathBooleanInput> inputVector(inputs);
  return ApplyPathBoolean(op, inputVector);
}

std::string PathData(const PathBooleanResult& result) {
  if (result.paths.empty()) {
    return "";
  }
  return std::string(result.paths.front().toSVGPathData());
}

std::string Diagnostics(const PathBooleanResult& result) {
  std::string diagnostics;
  for (std::string_view diagnostic : result.diagnostics) {
    if (!diagnostics.empty()) {
      diagnostics += "; ";
    }
    diagnostics += diagnostic;
  }
  return diagnostics;
}

std::size_t CommandCount(const Path& path, Path::Verb verb) {
  std::size_t count = 0;
  for (const Path::Command& command : path.commands()) {
    if (command.verb == verb) {
      ++count;
    }
  }
  return count;
}

MATCHER_P(HasCommandVerb, expectedVerb, "") {
  return testing::ExplainMatchResult(
      testing::Contains(testing::Field("verb", &Path::Command::verb, testing::Eq(expectedVerb))),
      arg.commands(), result_listener);
}

MATCHER_P2(CommandCountIs, expectedVerb, expectedCount, "") {
  *result_listener << "path data: " << arg.toSVGPathData();
  return testing::ExplainMatchResult(testing::Eq(expectedCount), CommandCount(arg, expectedVerb),
                                     result_listener);
}

MATCHER_P(IsInside, point, "") {
  *result_listener << "path data: " << arg.toSVGPathData();
  return arg.isInside(point);
}

MATCHER_P(IsOutside, point, "") {
  *result_listener << "path data: " << arg.toSVGPathData();
  return !arg.isInside(point);
}

Path QuadraticCapPath() {
  return PathBuilder()
      .moveTo({0, 0})
      .quadTo({50, 50}, {100, 0})
      .lineTo({100, 80})
      .lineTo({0, 80})
      .closePath()
      .build();
}

Path CubicCapPath() {
  return PathBuilder()
      .moveTo({0, 0})
      .curveTo({25, 70}, {75, 70}, {100, 0})
      .lineTo({100, 90})
      .lineTo({0, 90})
      .closePath()
      .build();
}

Path ReversedCubicCapPath() {
  return PathBuilder()
      .moveTo({0, 90})
      .lineTo({100, 90})
      .lineTo({100, 0})
      .curveTo({75, 70}, {25, 70}, {0, 0})
      .closePath()
      .build();
}

Path RegionBelowLineBoundary() {
  return PathBuilder()
      .moveTo({0, 50})
      .lineTo({100, 50})
      .lineTo({100, 100})
      .lineTo({0, 100})
      .closePath()
      .build();
}

Path RegionBelowLinePrefixBoundary() {
  return PathBuilder()
      .moveTo({0, 50})
      .lineTo({75, 50})
      .lineTo({75, 100})
      .lineTo({0, 100})
      .closePath()
      .build();
}

Path RegionAboveReversedLineContainedBoundary() {
  return PathBuilder()
      .moveTo({75, 50})
      .lineTo({25, 50})
      .lineTo({25, 0})
      .lineTo({75, 0})
      .closePath()
      .build();
}

Path RegionAboveReversedLineSuffixBoundary() {
  return PathBuilder()
      .moveTo({100, 50})
      .lineTo({25, 50})
      .lineTo({25, 0})
      .lineTo({100, 0})
      .closePath()
      .build();
}

Path RegionAboveStraightQuadraticBoundary() {
  return PathBuilder()
      .moveTo({0, 50})
      .quadTo({50, 50}, {100, 50})
      .lineTo({100, 0})
      .lineTo({0, 0})
      .closePath()
      .build();
}

Path RegionAboveStraightQuadraticSuffixBoundary() {
  return PathBuilder()
      .moveTo({25, 50})
      .quadTo({62.5, 50}, {100, 50})
      .lineTo({100, 0})
      .lineTo({25, 0})
      .closePath()
      .build();
}

Path RegionAboveStraightCubicBoundary() {
  return PathBuilder()
      .moveTo({0, 50})
      .curveTo({25, 50}, {75, 50}, {100, 50})
      .lineTo({100, 0})
      .lineTo({0, 0})
      .closePath()
      .build();
}

Path RegionAboveStraightCubicSuffixBoundary() {
  return PathBuilder()
      .moveTo({25, 50})
      .curveTo({43.75, 50}, {81.25, 50}, {100, 50})
      .lineTo({100, 0})
      .lineTo({25, 0})
      .closePath()
      .build();
}

Path FuzzerDegenerateCurveSearchLhsPath() {
  constexpr double kPoint = -19.215686274509807;
  constexpr double kNearPoint1 = -19.20042725261311;
  constexpr double kNearPoint2 = -19.191271839475093;
  return PathBuilder()
      .moveTo({kPoint, kPoint})
      .quadTo({kPoint, kPoint}, {kPoint, kNearPoint1})
      .curveTo({kPoint, kPoint}, {kPoint, kPoint}, {kPoint, kPoint})
      .quadTo({kPoint, kPoint}, {kPoint, kPoint})
      .quadTo({kPoint, kPoint}, {kPoint, kNearPoint2})
      .quadTo({kPoint, kPoint}, {kPoint, kPoint})
      .quadTo({kPoint, kPoint}, {kPoint, kPoint})
      .quadTo({kPoint, kPoint}, {kPoint, kPoint})
      .quadTo({kPoint, kPoint}, {kPoint, kPoint})
      .closePath()
      .build();
}

Path FuzzerDegenerateCurveSearchRhsPath() {
  constexpr double kPoint = -19.215686274509807;
  return PathBuilder()
      .moveTo({kPoint, kPoint})
      .quadTo({kPoint, kPoint}, {kPoint, kPoint})
      .closePath()
      .build();
}

Path FuzzerCubicDifferenceSearchLhsPath() {
  constexpr double kLow = -96.07843137254902;
  constexpr double kHigh = 96.078431372549;
  constexpr double kNearX = -96.06317235065232;
  constexpr double kNearY = 96.85969329365986;
  return PathBuilder()
      .moveTo({kLow, kLow})
      .curveTo({kLow, kLow}, {kLow, kLow}, {kLow, kLow})
      .curveTo({kLow, kLow}, {kLow, kNearY}, {kHigh, kLow})
      .curveTo({kLow, kLow}, {kLow, kLow}, {kLow, kLow})
      .curveTo({kLow, kLow}, {kLow, kLow}, {kLow, kLow})
      .curveTo({kLow, kLow}, {kLow, kLow}, {kLow, kLow})
      .curveTo({kLow, kLow}, {kLow, kLow}, {kNearX, kLow})
      .closePath()
      .build();
}

Path FuzzerCubicDifferenceSearchRhsPath() {
  constexpr double kLow = -96.07843137254902;
  constexpr double kNearY = -95.96246280613413;
  return PathBuilder()
      .moveTo({kLow, kLow})
      .curveTo({kLow, kNearY}, {kLow, kLow}, {kLow, kLow})
      .closePath()
      .build();
}

std::array<Vector2d, 3> ExtractQuadraticSpan(const std::array<Vector2d, 3>& points, double t0,
                                             double t1) {
  const auto firstSplit = SplitQuadratic(points[0], points[1], points[2], t1);
  if (t0 <= 0.0) {
    return firstSplit.first;
  }

  const double relativeT = t0 / t1;
  const auto secondSplit =
      SplitQuadratic(firstSplit.first[0], firstSplit.first[1], firstSplit.first[2], relativeT);
  return secondSplit.second;
}

std::array<Vector2d, 4> ExtractCubicSpan(const std::array<Vector2d, 4>& points, double t0,
                                         double t1) {
  const auto firstSplit = SplitCubic(points[0], points[1], points[2], points[3], t1);
  if (t0 <= 0.0) {
    return firstSplit.first;
  }

  const double relativeT = t0 / t1;
  const auto secondSplit = SplitCubic(firstSplit.first[0], firstSplit.first[1], firstSplit.first[2],
                                      firstSplit.first[3], relativeT);
  return secondSplit.second;
}

Path RegionBelowQuadraticArch() {
  return PathBuilder()
      .moveTo({0, 50})
      .quadTo({50, 0}, {100, 50})
      .lineTo({100, 100})
      .lineTo({0, 100})
      .closePath()
      .build();
}

Path RegionAboveQuadraticArch() {
  return PathBuilder()
      .moveTo({0, 30})
      .quadTo({60, 100}, {100, 30})
      .lineTo({100, 0})
      .lineTo({0, 0})
      .closePath()
      .build();
}

Path RegionAboveSharedQuadraticArch() {
  return PathBuilder()
      .moveTo({0, 50})
      .quadTo({50, 0}, {100, 50})
      .lineTo({100, 0})
      .lineTo({0, 0})
      .closePath()
      .build();
}

Path RegionAbovePartialQuadraticArch() {
  const std::array<Vector2d, 3> shared =
      ExtractQuadraticSpan({Vector2d(0, 50), Vector2d(50, 0), Vector2d(100, 50)}, 0.25, 0.75);
  return PathBuilder()
      .moveTo(shared[0])
      .quadTo(shared[1], shared[2])
      .lineTo({shared[2].x, 0})
      .lineTo({shared[0].x, 0})
      .closePath()
      .build();
}

Path RegionBelowQuadraticPrefixArch() {
  const std::array<Vector2d, 3> shared =
      ExtractQuadraticSpan({Vector2d(0, 50), Vector2d(50, 0), Vector2d(100, 50)}, 0.0, 0.75);
  return PathBuilder()
      .moveTo(shared[0])
      .quadTo(shared[1], shared[2])
      .lineTo({shared[2].x, 100})
      .lineTo({shared[0].x, 100})
      .closePath()
      .build();
}

Path RegionAboveQuadraticSuffixArch() {
  const std::array<Vector2d, 3> shared =
      ExtractQuadraticSpan({Vector2d(0, 50), Vector2d(50, 0), Vector2d(100, 50)}, 0.25, 1.0);
  return PathBuilder()
      .moveTo(shared[0])
      .quadTo(shared[1], shared[2])
      .lineTo({shared[2].x, 0})
      .lineTo({shared[0].x, 0})
      .closePath()
      .build();
}

Path RegionAboveReversedQuadraticSuffixArch() {
  const std::array<Vector2d, 3> shared =
      ExtractQuadraticSpan({Vector2d(0, 50), Vector2d(50, 0), Vector2d(100, 50)}, 0.25, 1.0);
  return PathBuilder()
      .moveTo(shared[2])
      .quadTo(shared[1], shared[0])
      .lineTo({shared[0].x, 0})
      .lineTo({shared[2].x, 0})
      .closePath()
      .build();
}

Path RegionBelowCubicArch() {
  return PathBuilder()
      .moveTo({0, 50})
      .curveTo({25, 0}, {75, 0}, {100, 50})
      .lineTo({100, 100})
      .lineTo({0, 100})
      .closePath()
      .build();
}

Path RegionAbovePartialCubicArch() {
  const std::array<Vector2d, 4> shared = ExtractCubicSpan(
      {Vector2d(0, 50), Vector2d(25, 0), Vector2d(75, 0), Vector2d(100, 50)}, 0.25, 0.75);
  return PathBuilder()
      .moveTo(shared[0])
      .curveTo(shared[1], shared[2], shared[3])
      .lineTo({shared[3].x, 0})
      .lineTo({shared[0].x, 0})
      .closePath()
      .build();
}

Path RegionBelowCubicPrefixArch() {
  const std::array<Vector2d, 4> shared = ExtractCubicSpan(
      {Vector2d(0, 50), Vector2d(25, 0), Vector2d(75, 0), Vector2d(100, 50)}, 0.0, 0.75);
  return PathBuilder()
      .moveTo(shared[0])
      .curveTo(shared[1], shared[2], shared[3])
      .lineTo({shared[3].x, 100})
      .lineTo({shared[0].x, 100})
      .closePath()
      .build();
}

Path RegionAboveCubicSuffixArch() {
  const std::array<Vector2d, 4> shared = ExtractCubicSpan(
      {Vector2d(0, 50), Vector2d(25, 0), Vector2d(75, 0), Vector2d(100, 50)}, 0.25, 1.0);
  return PathBuilder()
      .moveTo(shared[0])
      .curveTo(shared[1], shared[2], shared[3])
      .lineTo({shared[3].x, 0})
      .lineTo({shared[0].x, 0})
      .closePath()
      .build();
}

Path RegionAboveReversedCubicSuffixArch() {
  const std::array<Vector2d, 4> shared = ExtractCubicSpan(
      {Vector2d(0, 50), Vector2d(25, 0), Vector2d(75, 0), Vector2d(100, 50)}, 0.25, 1.0);
  return PathBuilder()
      .moveTo(shared[3])
      .curveTo(shared[2], shared[1], shared[0])
      .lineTo({shared[0].x, 0})
      .lineTo({shared[3].x, 0})
      .closePath()
      .build();
}

Path RegionAboveCubicArch() {
  return PathBuilder()
      .moveTo({0, 30})
      .curveTo({25, 100}, {75, 100}, {100, 30})
      .lineTo({100, 0})
      .lineTo({0, 0})
      .closePath()
      .build();
}

Path RegionAboveSharedCubicArch() {
  return PathBuilder()
      .moveTo({0, 50})
      .curveTo({25, 0}, {75, 0}, {100, 50})
      .lineTo({100, 0})
      .lineTo({0, 0})
      .closePath()
      .build();
}

}  // namespace

TEST(PathOpsTest, IntersectsOverlappingRectangles) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(RectPath(10, 10, 40, 40)), Input(RectPath(30, 25, 40, 20))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  EXPECT_EQ(PathData(result), "M 30 25 L 50 25 L 50 45 L 30 45 Z");
}

TEST(PathOpsTest, UnionsDisjointRectanglesAsCompoundPath) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Union, {Input(RectPath(10, 10, 20, 20)), Input(RectPath(50, 50, 20, 20))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  EXPECT_EQ(PathData(result),
            "M 10 10 L 30 10 L 30 30 L 10 30 Z M 50 50 L 70 50 L 70 70 L 50 70 Z");
}

TEST(PathOpsTest, DifferenceKeepsFirstInputMinusLaterInputs) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RectPath(10, 10, 40, 40)), Input(RectPath(30, 25, 40, 20))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(20, 20)));
  EXPECT_THAT(path, IsOutside(Vector2d(35, 30)));
  EXPECT_THAT(path, IsOutside(Vector2d(60, 30)));
}

TEST(PathOpsTest, DifferenceCreatesHoleForNestedContour) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RectPath(0, 0, 100, 100)), Input(RectPath(25, 25, 50, 50))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(10, 10)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
}

TEST(PathOpsTest, UnionPreservesNonZeroCompoundPathHole) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Union, {Input(NonZeroRectDonutPath()), Input(RectPath(150, 0, 20, 20))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(10, 10)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsInside(Vector2d(160, 10)));
}

TEST(PathOpsTest, DifferencePreservesNonOverlappingNonZeroCompoundPathLobes) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(NonZeroRectDonutPath()), Input(RectPath(75, -10, 50, 120))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(10, 10)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(90, 10)));
}

TEST(PathOpsTest, DifferenceCreatesCurvedHoleForNestedContour) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(CirclePath({50, 50}, 40)), Input(CirclePath({50, 50}, 15))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_EQ(result.paths.size(), 1u);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, CommandCountIs(Path::Verb::MoveTo, 2u));
  EXPECT_THAT(path, CommandCountIs(Path::Verb::ClosePath, 2u));
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(5, 50)));
}

TEST(PathOpsTest, IntersectOfNestedCirclesKeepsInnerCircle) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(CirclePath({50, 50}, 40)), Input(CirclePath({50, 50}, 15))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_EQ(result.paths.size(), 1u);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, CommandCountIs(Path::Verb::MoveTo, 1u));
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(5, 50)));
}

TEST(PathOpsTest, UnionOfNestedCirclesKeepsOuterCircle) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Union, {Input(CirclePath({50, 50}, 40)), Input(CirclePath({50, 50}, 15))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_EQ(result.paths.size(), 1u);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, CommandCountIs(Path::Verb::MoveTo, 1u));
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(5, 50)));
}

TEST(PathOpsTest, LowIntersectionCapAllowsNestedCircleContainment) {
  PathBooleanOptions options;
  options.maxIntersections = 64;
  const std::array<PathBooleanInput, 2> inputs = {
      Input(CirclePath({50, 50}, 40)),
      Input(CirclePath({50, 50}, 15)),
  };

  const PathBooleanResult unionResult = ApplyPathBoolean(PathBooleanOp::Union, inputs, options);
  ASSERT_EQ(unionResult.status, PathBooleanStatus::Ok) << Diagnostics(unionResult);
  ASSERT_EQ(unionResult.paths.size(), 1u);
  EXPECT_THAT(unionResult.paths.front(), IsInside(Vector2d(50, 50)));
  EXPECT_THAT(unionResult.paths.front(), IsOutside(Vector2d(5, 50)));

  const PathBooleanResult intersectResult =
      ApplyPathBoolean(PathBooleanOp::Intersect, inputs, options);
  ASSERT_EQ(intersectResult.status, PathBooleanStatus::Ok) << Diagnostics(intersectResult);
  ASSERT_EQ(intersectResult.paths.size(), 1u);
  EXPECT_THAT(intersectResult.paths.front(), IsInside(Vector2d(50, 50)));
  EXPECT_THAT(intersectResult.paths.front(), IsOutside(Vector2d(50, 25)));
}

TEST(PathOpsTest, XorOfNestedCirclesCreatesCurvedRing) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Xor, {Input(CirclePath({50, 50}, 40)), Input(CirclePath({50, 50}, 15))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_EQ(result.paths.size(), 1u);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, CommandCountIs(Path::Verb::MoveTo, 2u));
  EXPECT_THAT(path, CommandCountIs(Path::Verb::ClosePath, 2u));
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(5, 50)));
}

TEST(PathOpsTest, DifferenceOfCircleCoveredByLargerCircleIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(CirclePath({50, 50}, 15)), Input(CirclePath({50, 50}, 40))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, IntersectRespectsEvenOddInputFillRule) {
  const Path donut = PathBuilder()
                         .addRect(Box2d::FromXYWH(0, 0, 100, 100))
                         .addRect(Box2d::FromXYWH(25, 25, 50, 50))
                         .build();
  PathBooleanInput donutInput = Input(donut);
  donutInput.fillRule = FillRule::EvenOdd;

  const std::array<PathBooleanInput, 2> inputs = {
      donutInput,
      Input(RectPath(40, 40, 10, 10)),
  };
  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Intersect, inputs);

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, IntersectRespectsEvenOddCurvedInputHole) {
  PathBooleanInput donutInput = Input(CircleDonutPath({50, 50}, 40, 15));
  donutInput.fillRule = FillRule::EvenOdd;

  const std::array<PathBooleanInput, 2> inputs = {
      donutInput,
      Input(CirclePath({50, 50}, 5)),
  };
  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Intersect, inputs);

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, IntersectKeepsCurvedDonutRingOutsideHole) {
  PathBooleanInput donutInput = Input(CircleDonutPath({50, 50}, 40, 15));
  donutInput.fillRule = FillRule::EvenOdd;

  const std::array<PathBooleanInput, 2> inputs = {
      donutInput,
      Input(RectPath(45, 20, 10, 20)),
  };
  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Intersect, inputs);

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 10)));
}

TEST(PathOpsTest, XorExcludesOverlap) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Xor, {Input(RectPath(10, 10, 40, 40)), Input(RectPath(30, 25, 40, 20))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(20, 20)));
  EXPECT_THAT(path, IsOutside(Vector2d(35, 30)));
  EXPECT_THAT(path, IsInside(Vector2d(60, 30)));
}

TEST(PathOpsTest, EmptyIntersectionReportsEmptyWithoutOutput) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(RectPath(10, 10, 20, 20)), Input(RectPath(50, 50, 20, 20))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, PointTouchingIntersectionIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(RectPath(0, 0, 10, 10)), Input(RectPath(10, 10, 10, 10))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, SharedEdgeIntersectionIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(RectPath(0, 0, 10, 10)), Input(RectPath(10, 0, 10, 10))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, SharedEdgeUnionProducesSingleExteriorContour) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Union, {Input(RectPath(0, 0, 10, 10)), Input(RectPath(10, 0, 10, 10))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  ASSERT_EQ(result.paths.size(), 1u);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, CommandCountIs(Path::Verb::MoveTo, 1u));
  EXPECT_THAT(path, CommandCountIs(Path::Verb::ClosePath, 1u));
  EXPECT_THAT(path, IsInside(Vector2d(5, 5)));
  EXPECT_THAT(path, IsInside(Vector2d(15, 5)));
  EXPECT_THAT(path, IsOutside(Vector2d(25, 5)));
}

TEST(PathOpsTest, IntersectOfLineAndStraightQuadraticSharedBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowLineBoundary()), Input(RegionAboveStraightQuadraticBoundary())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsLineAndStraightQuadraticSharedBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowLineBoundary()), Input(RegionAboveStraightQuadraticBoundary())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(50, 75)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 25)));
}

TEST(PathOpsTest, IntersectOfLineAndStraightCubicSharedBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowLineBoundary()), Input(RegionAboveStraightCubicBoundary())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsLineAndStraightCubicSharedBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowLineBoundary()), Input(RegionAboveStraightCubicBoundary())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(50, 75)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 25)));
}

TEST(PathOpsTest, IntersectOfPartiallySharedLineAndStraightQuadraticBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(RegionBelowLinePrefixBoundary()),
                                         Input(RegionAboveStraightQuadraticSuffixBoundary())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsPartiallySharedLineAndStraightQuadraticBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference, {Input(RegionBelowLinePrefixBoundary()),
                                          Input(RegionAboveStraightQuadraticSuffixBoundary())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(50, 75)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(90, 75)));
}

TEST(PathOpsTest, IntersectOfPartiallySharedLineAndStraightCubicBoundaryIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect,
      {Input(RegionBelowLinePrefixBoundary()), Input(RegionAboveStraightCubicSuffixBoundary())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsPartiallySharedLineAndStraightCubicBoundarySide) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Difference,
      {Input(RegionBelowLinePrefixBoundary()), Input(RegionAboveStraightCubicSuffixBoundary())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(50, 75)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(90, 75)));
}

TEST(PathOpsTest, IntersectOfOppositeDirectionContainedLineBoundaryIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect,
      {Input(RegionBelowLineBoundary()), Input(RegionAboveReversedLineContainedBoundary())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsOppositeDirectionContainedLineBoundarySide) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Difference,
      {Input(RegionBelowLineBoundary()), Input(RegionAboveReversedLineContainedBoundary())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(50, 75)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 25)));
}

TEST(PathOpsTest, IntersectOfOppositeDirectionOverlappingLineBoundaryIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect,
      {Input(RegionBelowLinePrefixBoundary()), Input(RegionAboveReversedLineSuffixBoundary())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsOppositeDirectionOverlappingLineBoundarySide) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Difference,
      {Input(RegionBelowLinePrefixBoundary()), Input(RegionAboveReversedLineSuffixBoundary())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(50, 75)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(90, 75)));
}

TEST(PathOpsTest, TransformedInputsParticipateInOutputCoordinates) {
  PathBooleanInput translated = Input(RectPath(0, 0, 20, 20));
  translated.outputFromPath = Transform2d::Translate({20, 15});
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(RectPath(10, 10, 40, 40)), translated});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  EXPECT_EQ(PathData(result), "M 20 15 L 40 15 L 40 35 L 20 35 Z");
}

TEST(PathOpsTest, RotatedAndScaledInputParticipatesInOutputCoordinates) {
  PathBooleanInput transformed = Input(RectPath(0, 0, 10, 10));
  transformed.outputFromPath = Transform2d::Scale({2, 1}) * Transform2d::Rotate(M_PI / 2.0);
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(RectPath(-20, 0, 20, 20)), transformed});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(-10, 5)));
  EXPECT_THAT(path, IsOutside(Vector2d(5, 5)));
}

TEST(PathOpsTest, TransformedCircleInputParticipatesInCurveIntersection) {
  PathBooleanInput translated = Input(CirclePath({0, 0}, 25));
  translated.outputFromPath = Transform2d::Translate({60, 50});
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(CirclePath({40, 50}, 25)), translated});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(25, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(75, 50)));
}

TEST(PathOpsTest, ScaledCircleInputParticipatesInContainment) {
  PathBooleanInput scaled = Input(CirclePath({0, 0}, 10));
  scaled.outputFromPath = Transform2d::Scale({1.5, 1.5}) * Transform2d::Translate({50, 50});
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference, {Input(CirclePath({50, 50}, 40)), scaled});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 25)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(5, 50)));
}

TEST(PathOpsTest, ReflectedInputParticipatesInOutputCoordinates) {
  PathBooleanInput reflected = Input(RectPath(0, 0, 10, 10));
  reflected.outputFromPath = Transform2d::Scale({-1, 1}) * Transform2d::Translate({20, 0});
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(RectPath(15, 0, 20, 10)), reflected});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(17, 5)));
  EXPECT_THAT(path, IsOutside(Vector2d(12, 5)));
  EXPECT_THAT(path, IsOutside(Vector2d(22, 5)));
}

TEST(PathOpsTest, SkewedInputParticipatesInOutputCoordinates) {
  PathBooleanInput skewed = Input(RectPath(0, 0, 20, 20));
  skewed.outputFromPath = Transform2d::SkewX(M_PI / 4.0);
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(RectPath(15, 5, 20, 10)), skewed});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(25, 10)));
  EXPECT_THAT(path, IsOutside(Vector2d(10, 10)));
  EXPECT_THAT(path, IsOutside(Vector2d(38, 10)));
}

TEST(PathOpsTest, RetainedCurvesStayBezierSegments) {
  const Path circle = PathBuilder().addCircle({20, 20}, 10).build();
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Union, {Input(circle), Input(RectPath(50, 50, 10, 10))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  EXPECT_THAT(result.paths.front(), HasCommandVerb(Path::Verb::CurveTo));
}

TEST(PathOpsTest, IntersectOverlappingCirclesProducesCurvedLens) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(CirclePath({40, 50}, 25)), Input(CirclePath({60, 50}, 25))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(25, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(75, 50)));
}

TEST(PathOpsTest, UnionOfOverlappingCirclesCoversBothLobes) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Union, {Input(CirclePath({40, 50}, 25)), Input(CirclePath({60, 50}, 25))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(25, 50)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsInside(Vector2d(75, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(5, 50)));
}

TEST(PathOpsTest, DifferenceOfOverlappingCirclesRemovesLens) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(CirclePath({40, 50}, 25)), Input(CirclePath({60, 50}, 25))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(25, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(75, 50)));
}

TEST(PathOpsTest, XorOfOverlappingCirclesExcludesLens) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Xor, {Input(CirclePath({40, 50}, 25)), Input(CirclePath({60, 50}, 25))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(25, 50)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 50)));
  EXPECT_THAT(path, IsInside(Vector2d(75, 50)));
}

TEST(PathOpsTest, TangentCurvedIntersectionIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(CirclePath({0, 0}, 10)), Input(CirclePath({20, 0}, 10))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DisjointCurvedIntersectionIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(CirclePath({0, 0}, 10)), Input(CirclePath({40, 0}, 10))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, UnionOfDuplicateCurvedContoursKeepsSingleContour) {
  const Path circle = CirclePath({20, 20}, 10);
  const PathBooleanResult result = Boolean(PathBooleanOp::Union, {Input(circle), Input(circle)});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_EQ(result.paths.size(), 1u);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, CommandCountIs(Path::Verb::MoveTo, 1u));
  EXPECT_THAT(path, CommandCountIs(Path::Verb::ClosePath, 1u));
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(20, 20)));
  EXPECT_THAT(path, IsOutside(Vector2d(40, 20)));
}

TEST(PathOpsTest, IntersectOfDuplicateCurvedContoursKeepsOriginalRegion) {
  const Path circle = CirclePath({20, 20}, 10);
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(circle), Input(circle)});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_EQ(result.paths.size(), 1u);
  const Path& path = result.paths.front();
  EXPECT_THAT(path, CommandCountIs(Path::Verb::MoveTo, 1u));
  EXPECT_THAT(path, CommandCountIs(Path::Verb::ClosePath, 1u));
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(20, 20)));
  EXPECT_THAT(path, IsOutside(Vector2d(40, 20)));
}

TEST(PathOpsTest, DifferenceOfDuplicateCurvedContoursIsEmpty) {
  const Path circle = CirclePath({20, 20}, 10);
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference, {Input(circle), Input(circle)});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, XorOfDuplicateCurvedContoursIsEmpty) {
  const Path circle = CirclePath({20, 20}, 10);
  const PathBooleanResult result = Boolean(PathBooleanOp::Xor, {Input(circle), Input(circle)});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, UnionOfOppositeDirectionCurvedContoursKeepsSingleRegion) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Union, {Input(CubicCapPath()), Input(ReversedCubicCapPath())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, -10)));
}

TEST(PathOpsTest, DifferenceOfOppositeDirectionCurvedContoursIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference, {Input(CubicCapPath()), Input(ReversedCubicCapPath())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, IntersectOfSharedQuadraticBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowQuadraticArch()), Input(RegionAboveSharedQuadraticArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, UnionOfSharedQuadraticBoundaryCoversBothSides) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Union,
              {Input(RegionBelowQuadraticArch()), Input(RegionAboveSharedQuadraticArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(50, 10)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, -10)));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 110)));
}

TEST(PathOpsTest, DifferenceRetainsSharedQuadraticBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowQuadraticArch()), Input(RegionAboveSharedQuadraticArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::QuadTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 10)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectOfSharedCubicBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowCubicArch()), Input(RegionAboveSharedCubicArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsSharedCubicBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowCubicArch()), Input(RegionAboveSharedCubicArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 5)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectOfPartialSharedQuadraticBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowQuadraticArch()), Input(RegionAbovePartialQuadraticArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsPartialSharedQuadraticBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowQuadraticArch()), Input(RegionAbovePartialQuadraticArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::QuadTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 10)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectOfOverlappingPartialSharedQuadraticBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowQuadraticPrefixArch()), Input(RegionAboveQuadraticSuffixArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsOverlappingPartialSharedQuadraticBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowQuadraticPrefixArch()), Input(RegionAboveQuadraticSuffixArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::QuadTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 10)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectOfOppositeDirectionOverlappingPartialSharedQuadraticBoundaryIsEmpty) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect,
      {Input(RegionBelowQuadraticPrefixArch()), Input(RegionAboveReversedQuadraticSuffixArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsOppositeDirectionOverlappingPartialSharedQuadraticBoundarySide) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Difference,
      {Input(RegionBelowQuadraticPrefixArch()), Input(RegionAboveReversedQuadraticSuffixArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::QuadTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 10)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectOfPartialSharedCubicBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowCubicArch()), Input(RegionAbovePartialCubicArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsPartialSharedCubicBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowCubicArch()), Input(RegionAbovePartialCubicArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 5)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectOfOverlappingPartialSharedCubicBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowCubicPrefixArch()), Input(RegionAboveCubicSuffixArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsOverlappingPartialSharedCubicBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowCubicPrefixArch()), Input(RegionAboveCubicSuffixArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 5)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectOfOppositeDirectionOverlappingPartialSharedCubicBoundaryIsEmpty) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowCubicPrefixArch()), Input(RegionAboveReversedCubicSuffixArch())});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult) << Diagnostics(result);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, DifferenceRetainsOppositeDirectionOverlappingPartialSharedCubicBoundarySide) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Difference,
              {Input(RegionBelowCubicPrefixArch()), Input(RegionAboveReversedCubicSuffixArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(path, IsOutside(Vector2d(50, 5)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 70)));
}

TEST(PathOpsTest, IntersectRetainsQuadraticBoundarySpan) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(QuadraticCapPath()), Input(RectPath(25, -10, 50, 100))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  EXPECT_THAT(result.paths.front(), HasCommandVerb(Path::Verb::QuadTo));
  EXPECT_THAT(result.paths.front(), IsInside(Vector2d(50, 40)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(10, 40)));
}

TEST(PathOpsTest, IntersectRetainsCubicBoundarySpan) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(CubicCapPath()), Input(RectPath(25, -10, 50, 110))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  EXPECT_THAT(result.paths.front(), HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(result.paths.front(), IsInside(Vector2d(50, 70)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(10, 50)));
}

TEST(PathOpsTest, IntersectSplitsInteriorQuadraticQuadraticCrossings) {
  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect,
              {Input(RegionBelowQuadraticArch()), Input(RegionAboveQuadraticArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  EXPECT_THAT(result.paths.front(), HasCommandVerb(Path::Verb::QuadTo));
  EXPECT_THAT(result.paths.front(), IsInside(Vector2d(50, 50)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(50, 20)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(50, 80)));
}

TEST(PathOpsTest, IntersectSplitsInteriorQuadraticCubicCrossings) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(RegionBelowQuadraticArch()), Input(RegionAboveCubicArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  EXPECT_THAT(result.paths.front(), HasCommandVerb(Path::Verb::QuadTo));
  EXPECT_THAT(result.paths.front(), HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(result.paths.front(), IsInside(Vector2d(50, 50)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(50, 20)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(50, 90)));
}

TEST(PathOpsTest, IntersectSplitsInteriorCubicCubicCrossings) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(RegionBelowCubicArch()), Input(RegionAboveCubicArch())});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  EXPECT_THAT(result.paths.front(), HasCommandVerb(Path::Verb::CurveTo));
  EXPECT_THAT(result.paths.front(), IsInside(Vector2d(50, 50)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(50, 10)));
  EXPECT_THAT(result.paths.front(), IsOutside(Vector2d(50, 90)));
}

TEST(PathOpsTest, MultiInputDifferenceSubtractsEveryLaterInput) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Difference, {Input(RectPath(0, 0, 100, 30)), Input(RectPath(20, 0, 20, 30)),
                                  Input(RectPath(60, 0, 20, 30))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  ASSERT_THAT(result.paths, Not(IsEmpty()));
  const Path& path = result.paths.front();
  EXPECT_THAT(path, IsInside(Vector2d(10, 15)));
  EXPECT_THAT(path, IsOutside(Vector2d(30, 15)));
  EXPECT_THAT(path, IsInside(Vector2d(50, 15)));
  EXPECT_THAT(path, IsOutside(Vector2d(70, 15)));
  EXPECT_THAT(path, IsInside(Vector2d(90, 15)));
}

TEST(PathOpsTest, MultiInputIntersectionRequiresEveryInput) {
  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(RectPath(0, 0, 40, 40)), Input(RectPath(20, 0, 40, 40)),
                                 Input(RectPath(10, 10, 40, 40))});

  ASSERT_EQ(result.status, PathBooleanStatus::Ok);
  EXPECT_EQ(PathData(result), "M 20 10 L 40 10 L 40 40 L 20 40 Z");
}

TEST(PathOpsTest, RequiresAtLeastTwoInputs) {
  const std::array<PathBooleanInput, 1> inputs = {
      Input(RectPath(0, 0, 20, 20)),
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Union, inputs);

  EXPECT_EQ(result.status, PathBooleanStatus::InvalidInput);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(result.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, OpenContoursAreClosedAtMoveToAndPathEnd) {
  const Path openTriangles = PathBuilder()
                                 .moveTo({0, 0})
                                 .lineTo({20, 0})
                                 .lineTo({0, 20})
                                 .moveTo({40, 0})
                                 .lineTo({60, 0})
                                 .lineTo({40, 20})
                                 .build();

  const PathBooleanResult result = Boolean(
      PathBooleanOp::Intersect, {Input(openTriangles), Input(RectPath(-10, -10, 100, 100))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(Diagnostics(result), HasSubstr("dropping"));
}

TEST(PathOpsTest, MoveOnlyContoursDoNotEmitSegments) {
  const Path path = PathBuilder()
                        .moveTo({0, 0})
                        .moveTo({10, 10})
                        .lineTo({30, 10})
                        .lineTo({10, 30})
                        .moveTo({80, 80})
                        .closePath()
                        .build();

  const PathBooleanResult result =
      Boolean(PathBooleanOp::Intersect, {Input(path), Input(RectPath(0, 0, 40, 40))});

  EXPECT_EQ(result.status, PathBooleanStatus::EmptyResult);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(Diagnostics(result), HasSubstr("dropping"));
}

TEST(PathOpsTest, RejectsMoveOnlyInputContours) {
  const Path moveOnly = PathBuilder().moveTo({0, 0}).moveTo({10, 10}).closePath().build();
  const std::array<PathBooleanInput, 2> inputs = {
      Input(moveOnly),
      Input(RectPath(0, 0, 20, 20)),
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Union, inputs);

  EXPECT_EQ(result.status, PathBooleanStatus::InvalidInput);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(result.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, RespectsOutputCommandCap) {
  PathBooleanOptions options;
  options.maxOutputCommands = 2;
  const std::array<PathBooleanInput, 2> inputs = {
      Input(RectPath(10, 10, 20, 20)),
      Input(RectPath(50, 50, 20, 20)),
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Union, inputs, options);

  EXPECT_EQ(result.status, PathBooleanStatus::TooComplex);
  EXPECT_THAT(result.paths, IsEmpty());
}

TEST(PathOpsTest, RespectsInputCurveCap) {
  PathBooleanOptions options;
  options.maxCurveCount = 3;
  const std::array<PathBooleanInput, 2> inputs = {
      Input(RectPath(0, 0, 20, 20)),
      Input(RectPath(30, 0, 20, 20)),
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Union, inputs, options);

  EXPECT_EQ(result.status, PathBooleanStatus::TooComplex);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(result.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, RespectsIntersectionCap) {
  PathBooleanOptions options;
  options.maxIntersections = 0;
  const std::array<PathBooleanInput, 2> inputs = {
      Input(RectPath(0, 0, 20, 20)),
      Input(RectPath(10, 10, 20, 20)),
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Union, inputs, options);

  EXPECT_EQ(result.status, PathBooleanStatus::TooComplex);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(result.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, FuzzerTimeoutDegenerateCurveSearchReturnsTooComplex) {
  PathBooleanOptions options;
  options.maxCurveCount = 64;
  options.maxIntersections = 256;
  options.maxOutputCommands = 512;
  const Transform2d outputFromPath = Transform2d::Rotate(0.25);
  const std::array<PathBooleanInput, 2> inputs = {
      PathBooleanInput{
          .path = FuzzerDegenerateCurveSearchLhsPath(),
          .fillRule = FillRule::EvenOdd,
          .outputFromPath = outputFromPath,
      },
      PathBooleanInput{
          .path = FuzzerDegenerateCurveSearchRhsPath(),
          .fillRule = FillRule::EvenOdd,
          .outputFromPath = outputFromPath,
      },
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Xor, inputs, options);

  EXPECT_EQ(result.status, PathBooleanStatus::TooComplex);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(result.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, FuzzerTimeoutCubicDifferenceReturnsTooComplex) {
  PathBooleanOptions options;
  options.maxCurveCount = 64;
  options.maxIntersections = 256;
  options.maxOutputCommands = 512;
  const std::array<PathBooleanInput, 2> inputs = {
      PathBooleanInput{
          .path = FuzzerCubicDifferenceSearchLhsPath(),
          .fillRule = FillRule::EvenOdd,
      },
      PathBooleanInput{
          .path = FuzzerCubicDifferenceSearchRhsPath(),
          .fillRule = FillRule::EvenOdd,
      },
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Difference, inputs, options);

  EXPECT_EQ(result.status, PathBooleanStatus::TooComplex);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(result.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, RejectsNonFiniteInputCoordinates) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const Path invalidPath = PathBuilder().moveTo({0, 0}).lineTo({nan, 10}).closePath().build();
  const std::array<PathBooleanInput, 2> inputs = {
      Input(invalidPath),
      Input(RectPath(0, 0, 20, 20)),
  };

  const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Union, inputs);

  EXPECT_EQ(result.status, PathBooleanStatus::InvalidInput);
  EXPECT_THAT(result.paths, IsEmpty());
  EXPECT_THAT(result.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, RejectsNonFiniteCurveCoordinates) {
  const double inf = std::numeric_limits<double>::infinity();
  const Path invalidQuadratic =
      PathBuilder().moveTo({0, 0}).quadTo({10, inf}, {20, 0}).closePath().build();
  const std::array<PathBooleanInput, 2> quadraticInputs = {
      Input(invalidQuadratic),
      Input(RectPath(0, 0, 20, 20)),
  };
  const PathBooleanResult quadraticResult = ApplyPathBoolean(PathBooleanOp::Union, quadraticInputs);
  EXPECT_EQ(quadraticResult.status, PathBooleanStatus::InvalidInput);
  EXPECT_THAT(quadraticResult.paths, IsEmpty());
  EXPECT_THAT(quadraticResult.diagnostics, Not(IsEmpty()));

  const Path invalidCubic =
      PathBuilder().moveTo({0, 0}).curveTo({10, 0}, {15, 10}, {inf, 20}).closePath().build();
  const std::array<PathBooleanInput, 2> cubicInputs = {
      Input(invalidCubic),
      Input(RectPath(0, 0, 20, 20)),
  };
  const PathBooleanResult cubicResult = ApplyPathBoolean(PathBooleanOp::Union, cubicInputs);
  EXPECT_EQ(cubicResult.status, PathBooleanStatus::InvalidInput);
  EXPECT_THAT(cubicResult.paths, IsEmpty());
  EXPECT_THAT(cubicResult.diagnostics, Not(IsEmpty()));
}

TEST(PathOpsTest, InvalidGeometricToleranceUsesDefaultTolerance) {
  for (double invalidTolerance : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()}) {
    PathBooleanOptions options;
    options.geometricTolerance = invalidTolerance;
    const std::array<PathBooleanInput, 2> inputs = {
        Input(RectPath(0, 0, 20, 20)),
        Input(RectPath(10, 10, 20, 20)),
    };

    const PathBooleanResult result = ApplyPathBoolean(PathBooleanOp::Intersect, inputs, options);

    ASSERT_EQ(result.status, PathBooleanStatus::Ok) << Diagnostics(result);
    EXPECT_EQ(PathData(result), "M 10 10 L 20 10 L 20 20 L 10 20 Z");
  }
}

TEST(PathOpsTest, DeterministicCommandOrder) {
  const std::array<PathBooleanInput, 2> inputs = {
      Input(RectPath(10, 10, 40, 40)),
      Input(RectPath(30, 25, 40, 20)),
  };

  const PathBooleanResult first = ApplyPathBoolean(PathBooleanOp::Intersect, inputs);
  const PathBooleanResult second = ApplyPathBoolean(PathBooleanOp::Intersect, inputs);

  ASSERT_EQ(first.status, PathBooleanStatus::Ok);
  ASSERT_EQ(second.status, PathBooleanStatus::Ok);
  EXPECT_EQ(PathData(first), PathData(second));
}

TEST(PathOpsTest, DeterministicCurvedCommandOrder) {
  const std::array<PathBooleanInput, 2> inputs = {
      Input(CirclePath({40, 50}, 25)),
      Input(CirclePath({60, 50}, 25)),
  };

  const PathBooleanResult first = ApplyPathBoolean(PathBooleanOp::Union, inputs);
  const PathBooleanResult second = ApplyPathBoolean(PathBooleanOp::Union, inputs);

  ASSERT_EQ(first.status, PathBooleanStatus::Ok);
  ASSERT_EQ(second.status, PathBooleanStatus::Ok);
  EXPECT_EQ(PathData(first), PathData(second));
}

}  // namespace donner
