#include "donner/svg/core/PathBooleanOps.h"
#include "donner/svg/core/PathBooleanSegmenter.h"
#include "donner/svg/core/PathSpline.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

class MockPathBooleanEngine : public PathBooleanEngine {
public:
  MOCK_METHOD(SegmentedPath, compute, (const PathBooleanRequest& request), (override));
};

TEST(PathSplineBooleanTest, StaticBooleanOpDelegatesToAdapter) {
  PathSpline subject;
  subject.moveTo({0, 0});
  subject.lineTo({1, 0});
  subject.lineTo({1, 1});
  subject.closePath();

  PathSpline clip;
  clip.moveTo({0, 0});
  clip.lineTo({0, 1});
  clip.lineTo({1, 1});
  clip.closePath();

  PathSpline enginePath;
  enginePath.moveTo({0.25, 0.25});
  enginePath.lineTo({0.75, 0.25});
  enginePath.lineTo({0.75, 0.75});
  enginePath.closePath();
  const SegmentedPath expectedResult = SegmentPathForBoolean(enginePath, 0.5);

  MockPathBooleanEngine engine;
  EXPECT_CALL(engine, compute(testing::AllOf(
                          testing::Field(&PathBooleanRequest::op, PathBooleanOp::kUnion),
                          testing::Field(&PathBooleanRequest::subjectFillRule, FillRule::EvenOdd),
                          testing::Field(&PathBooleanRequest::clipFillRule, FillRule::NonZero),
                          testing::Field(&PathBooleanRequest::tolerance, testing::DoubleEq(0.5)))))
      .WillOnce(testing::Return(expectedResult));

  const PathSpline result = PathSpline::BooleanOp(
      subject, clip, PathBooleanOp::kUnion, FillRule::EvenOdd, FillRule::NonZero, engine, 0.5);

  EXPECT_THAT(result.commands(), testing::SizeIs(enginePath.commands().size()));
}

TEST(PathSplineBooleanTest, ConvenienceWrappersUseExpectedOps) {
  PathSpline subject;
  subject.moveTo({0, 0});
  subject.lineTo({2, 0});
  subject.lineTo({2, 2});
  subject.closePath();

  PathSpline other;
  other.moveTo({0, 0});
  other.lineTo({0, 2});
  other.lineTo({2, 2});
  other.closePath();

  MockPathBooleanEngine engine;
  EXPECT_CALL(engine, compute(testing::Field(&PathBooleanRequest::op, PathBooleanOp::kUnion)))
      .WillOnce(testing::Return(SegmentedPath{}));
  EXPECT_TRUE(
      subject.booleanUnion(other, FillRule::NonZero, FillRule::NonZero, engine).commands().empty());

  EXPECT_CALL(engine,
              compute(testing::Field(&PathBooleanRequest::op, PathBooleanOp::kIntersection)))
      .WillOnce(testing::Return(SegmentedPath{}));
  EXPECT_TRUE(subject.booleanIntersection(other, FillRule::NonZero, FillRule::NonZero, engine)
                  .commands()
                  .empty());

  EXPECT_CALL(engine, compute(testing::Field(&PathBooleanRequest::op, PathBooleanOp::kDifference)))
      .WillOnce(testing::Return(SegmentedPath{}));
  EXPECT_TRUE(subject.booleanDifference(other, FillRule::NonZero, FillRule::NonZero, engine)
                  .commands()
                  .empty());

  EXPECT_CALL(engine,
              compute(testing::Field(&PathBooleanRequest::op, PathBooleanOp::kReverseDifference)))
      .WillOnce(testing::Return(SegmentedPath{}));
  EXPECT_TRUE(subject.booleanReverseDifference(other, FillRule::NonZero, FillRule::NonZero, engine)
                  .commands()
                  .empty());

  EXPECT_CALL(engine, compute(testing::Field(&PathBooleanRequest::op, PathBooleanOp::kXor)))
      .WillOnce(testing::Return(SegmentedPath{}));
  EXPECT_TRUE(
      subject.booleanXor(other, FillRule::NonZero, FillRule::NonZero, engine).commands().empty());
}

}  // namespace
}  // namespace donner::svg
