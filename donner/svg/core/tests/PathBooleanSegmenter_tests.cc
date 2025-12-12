#include "donner/svg/core/PathBooleanSegmenter.h"

#include "donner/base/Vector2.h"
#include "donner/svg/core/PathSpline.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

using ::testing::DoubleEq;
using ::testing::Gt;
using ::testing::Le;
using ::testing::SizeIs;

TEST(PathBooleanSegmenterTest, SegmentsLinesCurvesAndClosure) {
  PathSpline spline;
  spline.moveTo({0, 0});
  spline.lineTo({10, 0});
  spline.curveTo({10, 0}, {10, 10}, {0, 10});
  spline.closePath();

  const SegmentedPath segmented = SegmentPathForBoolean(spline, kDefaultSegmentationTolerance);

  ASSERT_THAT(segmented.subpaths, SizeIs(1));
  const PathSubpathView& subpath = segmented.subpaths.front();
  EXPECT_TRUE(subpath.closed);
  EXPECT_THAT(subpath.moveTo, Vector2d(0, 0));
  ASSERT_THAT(subpath.spans, SizeIs(Gt<size_t>(2)));

  const PathCurveSpan& line = subpath.spans[0];
  EXPECT_EQ(line.type, PathSpline::CommandType::LineTo);
  EXPECT_THAT(line.startPoint, Vector2d(0, 0));
  EXPECT_THAT(line.endPoint, Vector2d(10, 0));
  EXPECT_DOUBLE_EQ(line.startT, 0.0);
  EXPECT_DOUBLE_EQ(line.endT, 1.0);

  const PathCurveSpan& firstCurve = subpath.spans[1];
  const PathCurveSpan& lastCurve = subpath.spans[subpath.spans.size() - 2];
  EXPECT_EQ(firstCurve.type, PathSpline::CommandType::CurveTo);
  EXPECT_EQ(lastCurve.type, PathSpline::CommandType::CurveTo);
  EXPECT_THAT(firstCurve.startPoint, Vector2d(10, 0));
  EXPECT_THAT(lastCurve.endPoint, Vector2d(0, 10));

  for (size_t i = 1; i + 1 < subpath.spans.size(); ++i) {
    const PathCurveSpan& span = subpath.spans[i];
    EXPECT_EQ(span.type, PathSpline::CommandType::CurveTo);
    if (i > 1) {
      EXPECT_THAT(span.startPoint, subpath.spans[i - 1].endPoint);
    }
  }

  const PathCurveSpan& closure = subpath.spans.back();
  EXPECT_EQ(closure.type, PathSpline::CommandType::ClosePath);
  EXPECT_THAT(closure.startPoint, lastCurve.endPoint);
  EXPECT_THAT(closure.endPoint, Vector2d(0, 0));
}

TEST(PathBooleanSegmenterTest, SplitsCurvyCubicWhenToleranceTight) {
  PathSpline spline;
  spline.moveTo({0, 0});
  spline.curveTo({50, 100}, {-50, 100}, {0, 0});

  const SegmentedPath segmented = SegmentPathForBoolean(spline, 0.01);

  ASSERT_THAT(segmented.subpaths, SizeIs(1));
  const PathSubpathView& subpath = segmented.subpaths.front();
  ASSERT_THAT(subpath.spans, SizeIs(Gt<size_t>(1)));

  double lastT = 0.0;
  for (const PathCurveSpan& span : subpath.spans) {
    EXPECT_EQ(span.type, PathSpline::CommandType::CurveTo);
    EXPECT_THAT(span.startT, DoubleEq(lastT));
    EXPECT_THAT(span.endT, Gt(span.startT));
    EXPECT_THAT(span.endT, Le(1.0));
    lastT = span.endT;
  }
  EXPECT_THAT(lastT, DoubleEq(1.0));
}

TEST(PathBooleanSegmenterTest, ClosePathUsesMoveToPoint) {
  PathSpline spline;
  spline.moveTo({1, 2});
  spline.lineTo({5, 2});
  spline.closePath();

  const SegmentedPath segmented = SegmentPathForBoolean(spline, kDefaultSegmentationTolerance);
  ASSERT_THAT(segmented.subpaths, SizeIs(1));
  const PathSubpathView& subpath = segmented.subpaths.front();
  ASSERT_THAT(subpath.spans, SizeIs(2));

  const PathCurveSpan& closing = subpath.spans.back();
  EXPECT_EQ(closing.type, PathSpline::CommandType::ClosePath);
  EXPECT_THAT(closing.startPoint, Vector2d(5, 2));
  EXPECT_THAT(closing.endPoint, Vector2d(1, 2));
  EXPECT_DOUBLE_EQ(closing.startT, 0.0);
  EXPECT_DOUBLE_EQ(closing.endT, 1.0);
}

}  // namespace
}  // namespace donner::svg
