#include "donner/svg/core/PathBooleanCustomEngine.h"

#include "donner/svg/core/PathBooleanSegmenter.h"
#include "donner/svg/core/PathSpline.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

SegmentedPath MakeSegmentedPath(std::initializer_list<Vector2d> points, bool closed = true) {
  PathSpline path;
  auto it = points.begin();
  if (it == points.end()) {
    return {};
  }

  path.moveTo(*it);
  ++it;
  for (; it != points.end(); ++it) {
    path.lineTo(*it);
  }
  if (closed) {
    path.closePath();
  }

  return SegmentPathForBoolean(path, 0.5);
}

TEST(PathBooleanCustomEngineTest, UnionConcatenatesSubpaths) {
  PathBooleanCustomEngine engine;
  PathBooleanRequest request{.op = PathBooleanOp::kUnion,
                             .subjectFillRule = FillRule::NonZero,
                             .clipFillRule = FillRule::EvenOdd,
                             .tolerance = 0.5,
                             .subject = MakeSegmentedPath({{0, 0}, {1, 0}, {1, 1}}),
                             .clip = MakeSegmentedPath({{2, 0}, {3, 0}, {3, 1}})};

  const SegmentedPath result = engine.compute(request);

  ASSERT_THAT(result.subpaths, testing::SizeIs(2));
  EXPECT_THAT(result.subpaths.front().spans, testing::SizeIs(3));
  EXPECT_THAT(result.subpaths.back().spans, testing::SizeIs(3));
}

TEST(PathBooleanCustomEngineTest, DifferenceReturnsSubjectOnly) {
  PathBooleanCustomEngine engine;
  PathBooleanRequest request{.op = PathBooleanOp::kDifference,
                             .subjectFillRule = FillRule::NonZero,
                             .clipFillRule = FillRule::NonZero,
                             .tolerance = 0.5,
                             .subject = MakeSegmentedPath({{0, 0}, {1, 0}, {1, 1}}),
                             .clip = MakeSegmentedPath({{2, 0}, {3, 0}, {3, 1}})};

  const SegmentedPath result = engine.compute(request);

  ASSERT_THAT(result.subpaths, testing::SizeIs(1));
  EXPECT_THAT(result.subpaths.front().spans, testing::SizeIs(3));
}

TEST(PathBooleanCustomEngineTest, ReverseDifferenceReturnsClipOnly) {
  PathBooleanCustomEngine engine;
  PathBooleanRequest request{.op = PathBooleanOp::kReverseDifference,
                             .subjectFillRule = FillRule::EvenOdd,
                             .clipFillRule = FillRule::NonZero,
                             .tolerance = 0.5,
                             .subject = MakeSegmentedPath({{0, 0}, {1, 0}, {1, 1}}),
                             .clip = MakeSegmentedPath({{2, 0}, {3, 0}, {3, 1}})};

  const SegmentedPath result = engine.compute(request);

  ASSERT_THAT(result.subpaths, testing::SizeIs(1));
  EXPECT_THAT(result.subpaths.front().spans.front().startPoint.x, testing::DoubleEq(2));
}

TEST(PathBooleanCustomEngineTest, IntersectionReturnsEmpty) {
  PathBooleanCustomEngine engine;
  PathBooleanRequest request{.op = PathBooleanOp::kIntersection,
                             .subjectFillRule = FillRule::NonZero,
                             .clipFillRule = FillRule::EvenOdd,
                             .tolerance = 0.5,
                             .subject = MakeSegmentedPath({{0, 0}, {1, 0}, {1, 1}}),
                             .clip = MakeSegmentedPath({{2, 0}, {3, 0}, {3, 1}})};

  const SegmentedPath result = engine.compute(request);

  EXPECT_TRUE(result.subpaths.empty());
}

}  // namespace
}  // namespace donner::svg

