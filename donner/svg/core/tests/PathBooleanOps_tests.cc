#include "donner/svg/core/PathBooleanOps.h"

#include "donner/svg/core/PathSpline.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

class MockPathBooleanEngine : public PathBooleanEngine {
public:
  MOCK_METHOD(SegmentedPath, compute, (const PathBooleanRequest& request), (override));
};

TEST(PathBooleanOpsTest, InvokesEngineWithSegmentedInputs) {
  PathSpline subject;
  subject.moveTo({0, 0});
  subject.lineTo({10, 0});
  subject.closePath();

  PathSpline clip;
  clip.moveTo({0, 0});
  clip.lineTo({0, 10});
  clip.closePath();

  PathSpline expectedPath;
  expectedPath.moveTo({1, 1});
  expectedPath.lineTo({2, 2});
  expectedPath.closePath();
  const SegmentedPath expected = SegmentPathForBoolean(expectedPath, 0.75);

  MockPathBooleanEngine engine;
  EXPECT_CALL(engine,
              compute(testing::AllOf(
                  testing::Field(&PathBooleanRequest::op, PathBooleanOp::kIntersection),
                  testing::Field(&PathBooleanRequest::subjectFillRule, FillRule::NonZero),
                  testing::Field(&PathBooleanRequest::clipFillRule, FillRule::EvenOdd),
                  testing::Field(&PathBooleanRequest::tolerance, testing::DoubleEq(0.75)),
                  testing::Field(&PathBooleanRequest::subject,
                                 testing::Field(&SegmentedPath::subpaths, testing::SizeIs(1))),
                  testing::Field(&PathBooleanRequest::clip,
                                 testing::Field(&SegmentedPath::subpaths, testing::SizeIs(1))))))
      .WillOnce(testing::Return(expected));

  const PathSpline result =
      PathBooleanOps::Compute(subject, clip, PathBooleanOp::kIntersection, FillRule::NonZero,
                              FillRule::EvenOdd, engine, 0.75);

  EXPECT_THAT(result.commands(), testing::SizeIs(expectedPath.commands().size()));
}

TEST(PathBooleanOpsTest, SkipsEngineWhenBothInputsEmpty) {
  MockPathBooleanEngine engine;
  EXPECT_CALL(engine, compute(testing::_)).Times(0);

  const PathSpline empty;
  const PathSpline result = PathBooleanOps::Compute(
      empty, empty, PathBooleanOp::kUnion, FillRule::NonZero, FillRule::NonZero, engine, 0.5);

  EXPECT_TRUE(result.commands().empty());
}

TEST(PathBooleanOpsTest, ShortCircuitsWhenEitherInputEmpty) {
  MockPathBooleanEngine engine;
  EXPECT_CALL(engine, compute(testing::_)).Times(0);

  PathSpline subject;
  subject.moveTo({0, 0});
  subject.lineTo({5, 0});
  subject.closePath();

  PathSpline clip;
  clip.moveTo({1, 1});
  clip.lineTo({2, 2});
  clip.closePath();

  const PathSpline unionResult =
      PathBooleanOps::Compute(subject, PathSpline{}, PathBooleanOp::kUnion, FillRule::NonZero,
                              FillRule::EvenOdd, engine, 0.5);
  EXPECT_THAT(unionResult.commands(), testing::SizeIs(subject.commands().size()));

  const PathSpline xorResult = PathBooleanOps::Compute(
      PathSpline{}, clip, PathBooleanOp::kXor, FillRule::NonZero, FillRule::EvenOdd, engine, 0.5);
  EXPECT_THAT(xorResult.commands(), testing::SizeIs(clip.commands().size()));

  const PathSpline differenceResult =
      PathBooleanOps::Compute(subject, PathSpline{}, PathBooleanOp::kDifference, FillRule::NonZero,
                              FillRule::NonZero, engine, 0.5);
  EXPECT_THAT(differenceResult.commands(), testing::SizeIs(subject.commands().size()));

  const PathSpline reverseDifferenceResult =
      PathBooleanOps::Compute(PathSpline{}, clip, PathBooleanOp::kReverseDifference,
                              FillRule::EvenOdd, FillRule::EvenOdd, engine, 0.5);
  EXPECT_THAT(reverseDifferenceResult.commands(), testing::SizeIs(clip.commands().size()));

  const PathSpline intersectionResult =
      PathBooleanOps::Compute(subject, PathSpline{}, PathBooleanOp::kIntersection,
                              FillRule::NonZero, FillRule::NonZero, engine, 0.5);
  EXPECT_TRUE(intersectionResult.commands().empty());
}

TEST(PathBooleanOpsTest, UsesDefaultToleranceWhenNonPositive) {
  PathSpline path;
  path.moveTo({0, 0});
  path.lineTo({1, 1});

  MockPathBooleanEngine engine;
  EXPECT_CALL(engine, compute(testing::Field(&PathBooleanRequest::tolerance,
                                             testing::DoubleEq(kDefaultSegmentationTolerance))))
      .WillOnce(testing::Return(SegmentedPath{}));

  (void)PathBooleanOps::Compute(path, path, PathBooleanOp::kUnion, FillRule::EvenOdd,
                                FillRule::EvenOdd, engine, 0.0);
}

}  // namespace
}  // namespace donner::svg
