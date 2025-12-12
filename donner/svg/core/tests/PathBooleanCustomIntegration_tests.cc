#include "donner/svg/core/PathBooleanCustomEngine.h"
#include "donner/svg/core/PathBooleanOps.h"
#include "donner/svg/core/PathSpline.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

PathSpline MakeRectangle(double x0, double y0, double x1, double y1) {
  PathSpline path;
  path.moveTo({x0, y0});
  path.lineTo({x1, y0});
  path.lineTo({x1, y1});
  path.lineTo({x0, y1});
  path.closePath();
  return path;
}

TEST(PathBooleanCustomIntegrationTest, UnionPreservesBothSubpaths) {
  PathSpline subject = MakeRectangle(0.0, 0.0, 1.0, 1.0);
  PathSpline clip = MakeRectangle(2.0, 2.0, 3.0, 3.0);

  PathBooleanCustomEngine engine;
  const PathSpline result = PathSpline::BooleanOp(
      subject, clip, PathBooleanOp::kUnion, FillRule::NonZero, FillRule::EvenOdd, engine, 0.5);

  const size_t expectedCommandCount = subject.commands().size() + clip.commands().size();
  ASSERT_THAT(result.commands(), testing::SizeIs(expectedCommandCount));
  EXPECT_EQ(result.commands().front().type, PathSpline::CommandType::MoveTo);
  EXPECT_EQ(result.commands().at(subject.commands().size()).type, PathSpline::CommandType::MoveTo);
  EXPECT_EQ(result.points().front(), subject.points().front());
  EXPECT_EQ(result.points().at(subject.points().size()), clip.points().front());
}

TEST(PathBooleanCustomIntegrationTest, DifferenceKeepsSubjectGeometry) {
  PathSpline subject = MakeRectangle(-1.0, -1.0, 1.0, 1.0);
  PathSpline clip = MakeRectangle(0.0, 0.0, 0.5, 0.5);

  PathBooleanCustomEngine engine;
  const PathSpline result =
      PathSpline::BooleanOp(subject, clip, PathBooleanOp::kDifference, FillRule::NonZero,
                            FillRule::NonZero, engine, 0.25);

  EXPECT_THAT(result.commands(), testing::SizeIs(subject.commands().size()));
  EXPECT_THAT(result.points(), testing::ContainerEq(subject.points()));
  EXPECT_EQ(result.commands().back().type, PathSpline::CommandType::ClosePath);
}

TEST(PathBooleanCustomIntegrationTest, ReverseDifferenceKeepsClipGeometry) {
  PathSpline subject = MakeRectangle(-1.0, -1.0, 0.0, 0.0);
  PathSpline clip = MakeRectangle(0.0, 0.0, 1.0, 1.0);

  PathBooleanCustomEngine engine;
  const PathSpline result =
      PathSpline::BooleanOp(subject, clip, PathBooleanOp::kReverseDifference, FillRule::EvenOdd,
                            FillRule::NonZero, engine, 0.5);

  EXPECT_THAT(result.commands(), testing::SizeIs(clip.commands().size()));
  EXPECT_THAT(result.points(), testing::ContainerEq(clip.points()));
  EXPECT_EQ(result.commands().front().type, PathSpline::CommandType::MoveTo);
  EXPECT_EQ(result.commands().back().type, PathSpline::CommandType::ClosePath);
}

TEST(PathBooleanCustomIntegrationTest, IntersectionProducesEmptyResult) {
  PathSpline subject = MakeRectangle(0.0, 0.0, 1.0, 1.0);
  PathSpline clip = MakeRectangle(2.0, 2.0, 3.0, 3.0);

  PathBooleanCustomEngine engine;
  const PathSpline result =
      PathSpline::BooleanOp(subject, clip, PathBooleanOp::kIntersection, FillRule::NonZero,
                            FillRule::NonZero, engine, 0.1);

  EXPECT_TRUE(result.commands().empty());
  EXPECT_TRUE(result.points().empty());
}

}  // namespace
}  // namespace donner::svg
