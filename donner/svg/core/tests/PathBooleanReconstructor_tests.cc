#include "donner/svg/core/PathBooleanReconstructor.h"

#include "donner/svg/core/PathBooleanSegmenter.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

TEST(PathBooleanReconstructorTest, RebuildsSegmentedSubpaths) {
  PathSpline spline;
  spline.moveTo({0, 0});
  spline.curveTo({2, 3}, {4, 3}, {6, 0});
  spline.lineTo({6, 6});
  spline.closePath();

  const auto endPointForCommand = [](const PathSpline& path, size_t commandIndex) {
    const PathSpline::Command& command = path.commands()[commandIndex];
    switch (command.type) {
      case PathSpline::CommandType::MoveTo:
      case PathSpline::CommandType::LineTo:
      case PathSpline::CommandType::ClosePath: return path.points()[command.pointIndex];
      case PathSpline::CommandType::CurveTo: return path.points()[command.pointIndex + 2];
    }
    return Vector2d{};
  };

  const SegmentedPath segmented = SegmentPathForBoolean(spline, kDefaultSegmentationTolerance);
  const PathSpline rebuilt = PathBooleanReconstructor::Reconstruct(segmented);

  ASSERT_THAT(rebuilt.commands(), testing::SizeIs(testing::Ge(3)));
  EXPECT_EQ(rebuilt.commands().front().type, PathSpline::CommandType::MoveTo);
  EXPECT_EQ(rebuilt.commands().back().type, PathSpline::CommandType::ClosePath);
  EXPECT_EQ(rebuilt.points().front(), spline.points().front());
  const Vector2d curveEnd = endPointForCommand(spline, 1);
  bool foundCurveEnd = false;
  for (size_t i = 0; i < rebuilt.commands().size(); ++i) {
    if (rebuilt.commands()[i].type == PathSpline::CommandType::CurveTo &&
        endPointForCommand(rebuilt, i) == curveEnd) {
      foundCurveEnd = true;
      break;
    }
  }
  EXPECT_TRUE(foundCurveEnd);
}

TEST(PathBooleanReconstructorTest, ClosesSubpathWhenMissingExplicitClosure) {
  PathSubpathView subpath;
  subpath.moveTo = {0, 0};
  subpath.spans.push_back({PathSpline::CommandType::LineTo, 0, 0.0, 1.0, {0, 0}, {5, 0}, {}, {}});
  subpath.closed = true;

  SegmentedPath segmented;
  segmented.subpaths.push_back(subpath);

  const PathSpline rebuilt = PathBooleanReconstructor::Reconstruct(segmented);

  ASSERT_THAT(rebuilt.commands(), testing::SizeIs(3));
  EXPECT_EQ(rebuilt.commands()[0].type, PathSpline::CommandType::MoveTo);
  EXPECT_EQ(rebuilt.commands()[1].type, PathSpline::CommandType::LineTo);
  EXPECT_EQ(rebuilt.commands()[2].type, PathSpline::CommandType::ClosePath);
}

}  // namespace
}  // namespace donner::svg
