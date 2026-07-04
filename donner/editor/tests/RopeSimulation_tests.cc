#include "donner/editor/RopeSimulation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace donner::editor {
namespace {

RopeSimulationOptions TestOptions() {
  RopeSimulationOptions options;
  options.segmentCount = 8;
  options.constraintIterations = 10;
  options.idleSwayPxPerSec2 = 0.0;
  options.gravityPxPerSec2 = 80.0;
  return options;
}

RopeSimulation MakeStraightRope(const RopeSimulationOptions& options) {
  const std::vector<Vector2d> route = {
      Vector2d(0.0, 0.0),
      Vector2d(100.0, 0.0),
  };
  RopeSimulation rope;
  rope.reset(route, options);
  return rope;
}

double MaxDeviationFrom(std::span<const Vector2d> points, std::span<const Vector2d> reference) {
  double result = 0.0;
  const std::size_t count = std::min(points.size(), reference.size());
  for (std::size_t i = 0; i < count; ++i) {
    result = std::max(result, points[i].distance(reference[i]));
  }
  return result;
}

MATCHER_P2(Vector2dNear, expected, tolerance, "") {
  return testing::ExplainMatchResult(
      testing::AllOf(testing::Field("x", &Vector2d::x, testing::DoubleNear(expected.x, tolerance)),
                     testing::Field("y", &Vector2d::y, testing::DoubleNear(expected.y, tolerance))),
      arg, result_listener);
}

MATCHER_P2(EndpointsAre, expectedStart, expectedEnd, "") {
  if (arg.size() < 2u) {
    *result_listener << "point count is " << arg.size();
    return false;
  }

  *result_listener << "front=" << arg.front() << ", back=" << arg.back();
  bool ok = true;
  ok &= testing::ExplainMatchResult(testing::Eq(expectedStart), arg.front(), result_listener);
  ok &= testing::ExplainMatchResult(testing::Eq(expectedEnd), arg.back(), result_listener);
  return ok;
}

MATCHER_P3(EndpointsNear, expectedStart, expectedEnd, tolerance, "") {
  if (arg.size() < 2u) {
    *result_listener << "point count is " << arg.size();
    return false;
  }

  *result_listener << "front=" << arg.front() << ", back=" << arg.back();
  bool ok = true;
  ok &= testing::ExplainMatchResult(Vector2dNear(expectedStart, tolerance), arg.front(),
                                    result_listener);
  ok &= testing::ExplainMatchResult(Vector2dNear(expectedEnd, tolerance), arg.back(),
                                    result_listener);
  return ok;
}

MATCHER_P(PathCommandVerbIs, expectedVerb, "") {
  return testing::ExplainMatchResult(testing::Eq(expectedVerb), arg.verb, result_listener);
}

MATCHER_P2(PathCommandEndpointVerbsAre, expectedFirstVerb, expectedLastVerb, "") {
  if (arg.size() < 2u) {
    *result_listener << "command count is " << arg.size();
    return false;
  }

  *result_listener << "first verb=" << testing::PrintToString(arg.front().verb)
                   << ", last verb=" << testing::PrintToString(arg.back().verb);
  bool ok = true;
  ok &= testing::ExplainMatchResult(PathCommandVerbIs(expectedFirstVerb), arg.front(),
                                    result_listener);
  ok &=
      testing::ExplainMatchResult(PathCommandVerbIs(expectedLastVerb), arg.back(), result_listener);
  return ok;
}

MATCHER_P(LastPointIs, expectedPoint, "") {
  if (arg.empty()) {
    *result_listener << "point list is empty";
    return false;
  }

  return testing::ExplainMatchResult(testing::Eq(expectedPoint), arg.back(), result_listener);
}

const Vector2d& MiddlePoint(std::span<const Vector2d> points) {
  return points[points.size() / 2u];
}

}  // namespace

TEST(RopeSimulationTest, KeepsEndpointsPinnedAfterUpdates) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope = MakeStraightRope(options);

  for (int i = 0; i < 30; ++i) {
    rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0,
                static_cast<double>(i) / 60.0, 123u, false, options);
  }

  ASSERT_GE(rope.points().size(), 2u);
  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0)));
}

TEST(RopeSimulationTest, GravityMovesInteriorParticlesDown) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope = MakeStraightRope(options);
  const double beforeY = MiddlePoint(rope.points()).y;

  for (int i = 0; i < 8; ++i) {
    rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0,
                static_cast<double>(i) / 60.0, 456u, false, options);
  }

  EXPECT_GT(MiddlePoint(rope.points()).y, beforeY);
  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0)));
}

TEST(RopeSimulationTest, ResetCatenaryHangsBetweenEndpoints) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  options.catenaryMinSlackPx = 24.0;
  RopeSimulation rope;

  rope.resetCatenary(Vector2d(0.0, 20.0), Vector2d(120.0, 20.0), options);

  ASSERT_GE(rope.points().size(), 3u);
  EXPECT_THAT(rope.points(), EndpointsNear(Vector2d(0.0, 20.0), Vector2d(120.0, 20.0), 1e-9));
  const Vector2d middle = MiddlePoint(rope.points());
  EXPECT_NEAR(middle.x, 60.0, 1.0);
  EXPECT_GT(middle.y, 20.0);
}

TEST(RopeSimulationTest, ResetCatenaryStartsSettledWithZeroVelocity) {
  RopeSimulationOptions options = TestOptions();
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), options);
  const std::vector<Vector2d> before(rope.points().begin(), rope.points().end());

  EXPECT_FALSE(rope.needsAnimation());
  rope.update(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 123u, false,
              options);

  EXPECT_EQ(std::vector<Vector2d>(rope.points().begin(), rope.points().end()), before);
  EXPECT_FALSE(rope.needsAnimation());
}

TEST(RopeSimulationTest, ScrollImpulseAffectsInteriorOnly) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation withoutScroll = MakeStraightRope(options);
  RopeSimulation withScroll = MakeStraightRope(options);

  withoutScroll.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 0.0, 789u, false,
                       options);
  withScroll.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 12.0, 0.0, 789u, false,
                    options);

  ASSERT_EQ(withoutScroll.points().size(), withScroll.points().size());
  EXPECT_THAT(withScroll.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0)));
  EXPECT_NE(MiddlePoint(withScroll.points()).y, MiddlePoint(withoutScroll.points()).y);
}

TEST(RopeSimulationTest, ScrollImpulseIsCappedForFastScrolls) {
  RopeSimulationOptions options = TestOptions();
  options.constraintIterations = 0;
  options.gravityPxPerSec2 = 0.0;
  options.scrollResponse = 1.0;
  options.maxScrollImpulsePx = 0.5;
  RopeSimulation rope = MakeStraightRope(options);

  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 1000.0, 1.0 / 60.0, 789u, false,
              options);

  ASSERT_GE(rope.points().size(), 3u);
  EXPECT_LE(std::abs(MiddlePoint(rope.points()).y), 0.5 + 1e-9);
  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0)));
}

TEST(RopeSimulationTest, ApplyImpulseCreatesMotionOnNextUpdate) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), options);
  const Vector2d before = rope.points()[rope.points().size() / 2u];

  rope.applyImpulse(Vector2d(4.0, 0.0));
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 0.0, 123u, false, options);

  EXPECT_GT(MiddlePoint(rope.points()).x, before.x);
  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0)));
}

TEST(RopeSimulationTest, BottomImpulseIsLocalizedNearCatenaryBottom) {
  RopeSimulationOptions options = TestOptions();
  options.constraintIterations = 0;
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), options);
  std::size_t bottomIndex = 1;
  for (std::size_t i = 2; i + 1 < rope.points().size(); ++i) {
    if (rope.points()[i].y > rope.points()[bottomIndex].y) {
      bottomIndex = i;
    }
  }
  const std::vector<Vector2d> before(rope.points().begin(), rope.points().end());

  rope.applyBottomImpulse(Vector2d(0.25, 0.0), 0.12);
  rope.update(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 123u, false,
              options);

  const std::size_t shoulderIndex = 1;
  const Vector2d bottomDelta = rope.points()[bottomIndex] - before[bottomIndex];
  const Vector2d shoulderDelta = rope.points()[shoulderIndex] - before[shoulderIndex];
  EXPECT_GT(std::abs(bottomDelta.x), std::abs(shoulderDelta.x) * 4.0);
  EXPECT_GT(std::abs(bottomDelta.x), std::abs(bottomDelta.y) * 20.0);
  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0)));
}

TEST(RopeSimulationTest, CatenaryBottomImpulseDecaysWithoutEnergyGain) {
  RopeSimulationOptions options = TestOptions();
  options.damping = 0.86;
  options.gravityPxPerSec2 = 500.0;
  options.idleSwayPxPerSec2 = 500.0;
  options.settleMotionThresholdPx = 0.0;
  options.settleStillnessSeconds = 100.0;
  options.settleTimeSeconds = 100.0;
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), options);
  const std::vector<Vector2d> rest(rope.points().begin(), rope.points().end());

  rope.applyBottomImpulse(Vector2d(0.25, 0.0), 0.12);

  double earlyPeak = 0.0;
  double latePeak = 0.0;
  for (int frame = 0; frame < 120; ++frame) {
    rope.update(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), 1.0 / 60.0, 0.0,
                static_cast<double>(frame + 1) / 60.0, 123u, false, options);

    double maxDeviation = 0.0;
    for (std::size_t i = 0; i < rope.points().size(); ++i) {
      maxDeviation = std::max(maxDeviation, rope.points()[i].distance(rest[i]));
    }

    if (frame < 20) {
      earlyPeak = std::max(earlyPeak, maxDeviation);
    } else if (frame >= 90) {
      latePeak = std::max(latePeak, maxDeviation);
    }
  }

  EXPECT_GT(earlyPeak, 0.0);
  EXPECT_LT(latePeak, earlyPeak * 0.35);
  EXPECT_TRUE(rope.needsAnimation());
}

TEST(RopeSimulationTest, CatenaryBottomImpulseIsStableAcrossFrameCadences) {
  RopeSimulationOptions options = TestOptions();
  options.damping = 0.86;
  options.gravityPxPerSec2 = 500.0;
  options.idleSwayPxPerSec2 = 500.0;
  options.settleMotionThresholdPx = 0.0;
  options.settleStillnessSeconds = 100.0;
  options.settleTimeSeconds = 100.0;
  RopeSimulation sixtyHz;
  RopeSimulation fastInputCadence;
  RopeSimulation coarseCadence;
  sixtyHz.resetCatenary(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), options);
  fastInputCadence.resetCatenary(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), options);
  coarseCadence.resetCatenary(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), options);
  sixtyHz.applyBottomImpulse(Vector2d(0.25, 0.0), 0.12);
  fastInputCadence.applyBottomImpulse(Vector2d(0.25, 0.0), 0.12);
  coarseCadence.applyBottomImpulse(Vector2d(0.25, 0.0), 0.12);

  for (int frame = 0; frame < 60; ++frame) {
    sixtyHz.update(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), 1.0 / 60.0, 0.0,
                   static_cast<double>(frame + 1) / 60.0, 123u, false, options);
  }
  for (int frame = 0; frame < 120; ++frame) {
    fastInputCadence.update(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), 1.0 / 120.0, 0.0,
                            static_cast<double>(frame + 1) / 120.0, 123u, false, options);
  }
  for (int frame = 0; frame < 15; ++frame) {
    coarseCadence.update(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), 1.0 / 15.0, 0.0,
                         static_cast<double>(frame + 1) / 15.0, 123u, false, options);
  }

  ASSERT_EQ(sixtyHz.points().size(), fastInputCadence.points().size());
  ASSERT_EQ(sixtyHz.points().size(), coarseCadence.points().size());
  for (std::size_t i = 0; i < sixtyHz.points().size(); ++i) {
    EXPECT_THAT(sixtyHz.points()[i], Vector2dNear(fastInputCadence.points()[i], 0.02));
    EXPECT_THAT(sixtyHz.points()[i], Vector2dNear(coarseCadence.points()[i], 0.02));
  }
}

TEST(RopeSimulationTest, LargeFrameDeltaAdvancesThroughFixedRealtimeSubsteps) {
  RopeSimulationOptions options = TestOptions();
  options.constraintIterations = 0;
  options.gravityPxPerSec2 = 0.0;
  options.damping = 1.0;
  options.maxDeltaTime = 1.0 / 60.0;
  options.settleTimeSeconds = 1.0;

  RopeSimulation singleFrame = MakeStraightRope(options);
  RopeSimulation fourFrames = MakeStraightRope(options);
  singleFrame.applyImpulse(Vector2d(4.0, 0.0));
  fourFrames.applyImpulse(Vector2d(4.0, 0.0));

  singleFrame.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 4.0 / 60.0, 0.0, 4.0 / 60.0, 123u,
                     false, options);
  for (int i = 0; i < 4; ++i) {
    fourFrames.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0,
                      static_cast<double>(i + 1) / 60.0, 123u, false, options);
  }

  ASSERT_EQ(singleFrame.points().size(), fourFrames.points().size());
  const std::size_t middle = singleFrame.points().size() / 2u;
  EXPECT_THAT(singleFrame.points()[middle], Vector2dNear(fourFrames.points()[middle], 1e-9));
}

TEST(RopeSimulationTest, ShortFrameDeltasPreserveRealtimeVelocityScale) {
  RopeSimulationOptions options = TestOptions();
  options.constraintIterations = 0;
  options.gravityPxPerSec2 = 0.0;
  options.damping = 1.0;
  options.maxDeltaTime = 1.0 / 60.0;
  options.settleTimeSeconds = 1.0;

  RopeSimulation singleFrame = MakeStraightRope(options);
  RopeSimulation twoFrames = MakeStraightRope(options);
  singleFrame.applyImpulse(Vector2d(4.0, 0.0));
  twoFrames.applyImpulse(Vector2d(4.0, 0.0));

  singleFrame.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 123u,
                     false, options);
  twoFrames.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 120.0, 0.0, 1.0 / 120.0, 123u,
                   false, options);
  twoFrames.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 120.0, 0.0, 1.0 / 60.0, 123u,
                   false, options);

  ASSERT_EQ(singleFrame.points().size(), twoFrames.points().size());
  const std::size_t middle = singleFrame.points().size() / 2u;
  EXPECT_THAT(singleFrame.points()[middle], Vector2dNear(twoFrames.points()[middle], 1e-9));
}

TEST(RopeSimulationTest, SleepsAfterStillnessAndWakesOnImpulse) {
  RopeSimulationOptions options = TestOptions();
  options.constraintIterations = 0;
  options.gravityPxPerSec2 = 0.0;
  options.damping = 0.0;
  options.maxDeltaTime = 1.0 / 60.0;
  options.settleMotionThresholdPx = 0.001;
  options.settleStillnessSeconds = 0.02;
  options.settleTimeSeconds = 100.0;
  RopeSimulation rope = MakeStraightRope(options);
  rope.applyImpulse(Vector2d(3.0, 0.0));

  ASSERT_TRUE(rope.needsAnimation());
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 123u, false,
              options);
  EXPECT_TRUE(rope.needsAnimation());
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 2.0 / 60.0, 123u, false,
              options);
  EXPECT_FALSE(rope.needsAnimation());

  const std::vector<Vector2d> asleep(rope.points().begin(), rope.points().end());
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 3.0 / 60.0, 123u, false,
              options);
  EXPECT_EQ(std::vector<Vector2d>(rope.points().begin(), rope.points().end()), asleep);

  rope.applyImpulse(Vector2d(1.0, 0.0));
  EXPECT_TRUE(rope.needsAnimation());
}

TEST(RopeSimulationTest, OverdueDampingSleepsOnlyAfterStillnessAndResetsOnWake) {
  RopeSimulationOptions options = TestOptions();
  options.constraintIterations = 0;
  options.gravityPxPerSec2 = 0.0;
  options.damping = 1.0;
  options.overdueDamping = 0.0;
  options.overdueDampingRampSeconds = 0.0;
  options.maxDeltaTime = 1.0 / 60.0;
  options.settleMotionThresholdPx = 0.001;
  options.settleStillnessSeconds = 0.02;
  options.settleTimeSeconds = 0.0;
  RopeSimulation rope = MakeStraightRope(options);
  rope.applyImpulse(Vector2d(0.75, 0.0));

  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 123u, false,
              options);
  EXPECT_TRUE(rope.needsAnimation());
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 2.0 / 60.0, 123u, false,
              options);
  EXPECT_TRUE(rope.needsAnimation());
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 3.0 / 60.0, 123u, false,
              options);
  ASSERT_FALSE(rope.needsAnimation());

  const Vector2d beforeWake = MiddlePoint(rope.points());
  rope.applyImpulse(Vector2d(0.75, 0.0));
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 4.0 / 60.0, 123u, false,
              options);

  EXPECT_TRUE(rope.needsAnimation());
  EXPECT_NE(MiddlePoint(rope.points()).x, beforeWake.x);
}

TEST(RopeSimulationTest, EndpointMotionCarriesBodyAndAddsVelocity) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  options.endpointFollow = 0.8;
  options.endpointImpulse = 0.2;
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), options);
  const Vector2d before = rope.points()[rope.points().size() / 2u];

  rope.update(Vector2d(0.0, 12.0), Vector2d(100.0, 12.0), 1.0 / 60.0, 0.0, 0.0, 123u, false,
              options);

  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 12.0), Vector2d(100.0, 12.0)));
  EXPECT_GT(MiddlePoint(rope.points()).y, before.y + 8.0);
}

TEST(RopeSimulationTest, EndpointMotionRetargetsAndDampsBackToCatenary) {
  RopeSimulationOptions options = TestOptions();
  options.segmentCount = 16;
  options.constraintIterations = 12;
  options.gravityPxPerSec2 = 0.0;
  options.damping = 0.84;
  options.overdueDamping = 0.2;
  options.overdueDampingRampSeconds = 0.0;
  options.settleTimeSeconds = 0.7;
  options.settleMotionThresholdPx = 0.015;
  options.settleStillnessSeconds = 0.12;
  options.settleRestDistanceThresholdPx = 0.3;
  options.catenaryRestoringForcePerSec2 = 900.0;
  options.endpointFollow = 0.82;
  options.endpointImpulse = 0.0;
  options.endpointMotionVelocityRetention = 0.20;
  options.endpointCatenaryBlend = 0.20;

  RopeSimulation target;
  target.resetCatenary(Vector2d(0.0, 0.0), Vector2d(112.0, 0.0), options);
  const std::vector<Vector2d> targetPoints(target.points().begin(), target.points().end());

  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(120.0, 0.0), options);
  rope.applyBottomImpulse(Vector2d(0.2, 0.0), 0.12);
  rope.update(Vector2d(0.0, 0.0), Vector2d(112.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 123u, false,
              options);
  const double initialDeviation = MaxDeviationFrom(rope.points(), targetPoints);

  for (int frame = 1; frame < 240 && rope.needsAnimation(); ++frame) {
    rope.update(Vector2d(0.0, 0.0), Vector2d(112.0, 0.0), 1.0 / 60.0, 0.0,
                static_cast<double>(frame + 1) / 60.0, 123u, false, options);
  }

  const double settledDeviation = MaxDeviationFrom(rope.points(), targetPoints);
  EXPECT_GT(initialDeviation, 0.05);
  EXPECT_LT(settledDeviation, 0.01);
  EXPECT_FALSE(rope.needsAnimation());
}

TEST(RopeSimulationTest, IdleSwayIsDeterministicAndSubtle) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  options.idleSwayPxPerSec2 = 18.0;

  RopeSimulation first = MakeStraightRope(options);
  RopeSimulation second = MakeStraightRope(options);
  first.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 1.25, 42u, false,
               options);
  second.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 1.25, 42u, false,
                options);

  ASSERT_EQ(first.points().size(), second.points().size());
  const Vector2d firstMiddle = MiddlePoint(first.points());
  EXPECT_THAT(firstMiddle, testing::Eq(MiddlePoint(second.points())));
  EXPECT_THAT(firstMiddle.x, testing::AllOf(testing::Gt(49.9), testing::Lt(50.1)));
}

TEST(RopeSimulationTest, HoverFreezePreservesBodyWhenAnchorsDoNotMove) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope = MakeStraightRope(options);
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 0.0, 12u, false, options);
  const std::vector<Vector2d> before(rope.points().begin(), rope.points().end());

  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 30.0, 1.0, 12u, true, options);

  EXPECT_EQ(std::vector<Vector2d>(rope.points().begin(), rope.points().end()), before);
}

TEST(RopeSimulationTest, ConvertsToBezierPathEndingAtTarget) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope = MakeStraightRope(options);
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 0.0, 12u, false, options);

  const Path path = rope.toPath(options);
  ASSERT_FALSE(path.empty());
  EXPECT_THAT(path.commands(), PathCommandEndpointVerbsAre(Path::Verb::MoveTo, Path::Verb::QuadTo));
  EXPECT_THAT(path.points(), LastPointIs(Vector2d(100.0, 0.0)));
}

// --- Degenerate reset / empty rope -----------------------------------------

TEST(RopeSimulationTest, ResetWithFewerThanTwoPointsClearsAndSleeps) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope = MakeStraightRope(options);
  ASSERT_FALSE(rope.empty());

  const std::vector<Vector2d> single = {Vector2d(5.0, 5.0)};
  rope.reset(single, options);

  EXPECT_TRUE(rope.empty());
  EXPECT_FALSE(rope.needsAnimation());
  EXPECT_TRUE(rope.toPath(options).empty());
}

TEST(RopeSimulationTest, ResetWithEmptyRouteClearsRope) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope;
  rope.reset(std::span<const Vector2d>(), options);

  EXPECT_TRUE(rope.empty());
  EXPECT_FALSE(rope.needsAnimation());
}

TEST(RopeSimulationTest, ResetWithCoincidentEndpointsCollapsesToPoint) {
  // Zero-length route exercises the totalLength <= kMinDistance branch in
  // reset(), filling every particle with the shared endpoint.
  const RopeSimulationOptions options = TestOptions();
  const std::vector<Vector2d> route = {Vector2d(7.0, 9.0), Vector2d(7.0, 9.0)};
  RopeSimulation rope;
  rope.reset(route, options);

  ASSERT_GE(rope.points().size(), 2u);
  EXPECT_THAT(rope.points(), testing::Each(Vector2d(7.0, 9.0)));
}

// --- Self-healing update on an empty/uninitialized rope --------------------

TEST(RopeSimulationTest, UpdateOnEmptyRopeSelfInitializesCatenary) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope;
  ASSERT_TRUE(rope.empty());

  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 7u, false,
              options);

  ASSERT_GE(rope.points().size(), 2u);
  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0)));
}

TEST(RopeSimulationTest, ZeroDeltaUpdateSolvesConstraintsWithoutAdvancing) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation rope = MakeStraightRope(options);
  rope.applyImpulse(Vector2d(3.0, 0.0));

  // A zero-time frame should pin endpoints and solve constraints without
  // integrating any motion.
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 0.0, 0.0, 0.0, 1u, false, options);

  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0)));
}

// --- Impulse guards --------------------------------------------------------

TEST(RopeSimulationTest, ApplyImpulseIgnoredOnEmptyRope) {
  RopeSimulation rope;
  rope.applyImpulse(Vector2d(5.0, 5.0));
  EXPECT_TRUE(rope.empty());
  EXPECT_FALSE(rope.needsAnimation());
}

TEST(RopeSimulationTest, ApplyImpulseIgnoredForNegligibleImpulse) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), options);
  ASSERT_FALSE(rope.needsAnimation());

  rope.applyImpulse(Vector2d(0.0, 0.0));

  // A zero impulse must not wake the rope.
  EXPECT_FALSE(rope.needsAnimation());
}

TEST(RopeSimulationTest, ApplyBottomImpulseIgnoredOnEmptyRope) {
  RopeSimulation rope;
  rope.applyBottomImpulse(Vector2d(5.0, 5.0), 0.2);
  EXPECT_TRUE(rope.empty());
  EXPECT_FALSE(rope.needsAnimation());
}

TEST(RopeSimulationTest, ApplyBottomImpulseIgnoredForNegligibleImpulse) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), options);
  ASSERT_FALSE(rope.needsAnimation());

  rope.applyBottomImpulse(Vector2d(0.0, 0.0), 0.2);

  EXPECT_FALSE(rope.needsAnimation());
}

// --- toPath / endTangent degenerate shapes ---------------------------------

TEST(RopeSimulationTest, EmptyRopeEndTangentIsXAxis) {
  RopeSimulation rope;
  EXPECT_EQ(rope.endTangent(), Vector2d::XAxis());
}

TEST(RopeSimulationTest, EndTangentPointsAlongFinalSegment) {
  const RopeSimulationOptions options = TestOptions();
  RopeSimulation rope = MakeStraightRope(options);

  const Vector2d tangent = rope.endTangent();
  EXPECT_GT(tangent.x, 0.0);
  EXPECT_NEAR(tangent.y, 0.0, 1e-9);
}

TEST(RopeSimulationTest, EndTangentFallsBackToXAxisWhenAllPointsCoincide) {
  // Coincident endpoints collapse every particle onto one point, so no segment
  // has non-zero length and endTangent() must fall back to the X axis.
  const RopeSimulationOptions options = TestOptions();
  const std::vector<Vector2d> route = {Vector2d(3.0, 3.0), Vector2d(3.0, 3.0)};
  RopeSimulation rope;
  rope.reset(route, options);

  EXPECT_EQ(rope.endTangent(), Vector2d::XAxis());
}

TEST(RopeSimulationTest, TwoParticleRopeProducesSingleLineSegmentPath) {
  RopeSimulationOptions options = TestOptions();
  options.segmentCount = 1;  // particleCount == 2
  const std::vector<Vector2d> route = {Vector2d(0.0, 0.0), Vector2d(40.0, 0.0)};
  RopeSimulation rope;
  rope.reset(route, options);

  ASSERT_EQ(rope.points().size(), 2u);
  const Path path = rope.toPath(options);
  ASSERT_GE(path.commands().size(), 2u);
  EXPECT_THAT(path.commands(), PathCommandEndpointVerbsAre(Path::Verb::MoveTo, Path::Verb::LineTo));
  EXPECT_THAT(path.points(), LastPointIs(Vector2d(40.0, 0.0)));
}

// --- Catenary fallback geometry --------------------------------------------

TEST(RopeSimulationTest, VerticalEndpointsUseFallbackCatenaryRoute) {
  // start.x == end.x triggers the BuildFallbackCatenaryRoute branch (the
  // catenary solver requires horizontal separation).
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(50.0, 0.0), Vector2d(50.0, 120.0), options);

  ASSERT_GE(rope.points().size(), 3u);
  EXPECT_THAT(rope.points(), EndpointsNear(Vector2d(50.0, 0.0), Vector2d(50.0, 120.0), 1e-9));
  // The fallback sags the interior downward in y while x tracks the chord.
  const Vector2d middle = MiddlePoint(rope.points());
  EXPECT_NEAR(middle.x, 50.0, 1e-9);
  EXPECT_GT(middle.y, 0.0);
}

TEST(RopeSimulationTest, CoincidentEndpointCatenaryStaysAtSharedPoint) {
  // chordLength <= kMinDistance also routes through the fallback path.
  RopeSimulationOptions options = TestOptions();
  RopeSimulation rope;
  rope.resetCatenary(Vector2d(10.0, 10.0), Vector2d(10.0, 10.0), options);

  ASSERT_GE(rope.points().size(), 2u);
  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(10.0, 10.0), Vector2d(10.0, 10.0)));
}

TEST(RopeSimulationTest, FrozenUpdateRigidlyCarriesBodyWithMovedEndpoints) {
  RopeSimulationOptions options = TestOptions();
  options.gravityPxPerSec2 = 0.0;
  RopeSimulation rope = MakeStraightRope(options);
  rope.applyImpulse(Vector2d(2.0, 0.0));
  rope.update(Vector2d(0.0, 0.0), Vector2d(100.0, 0.0), 1.0 / 60.0, 0.0, 1.0 / 60.0, 9u, false,
              options);
  const std::vector<Vector2d> before(rope.points().begin(), rope.points().end());

  // Frozen with both endpoints shifted +5 in y: integration is skipped, but the
  // body follows the endpoints rigidly (follow == 1.0 when frozen), so every
  // interior particle shifts by the same uniform delta.
  rope.update(Vector2d(0.0, 5.0), Vector2d(100.0, 5.0), 1.0 / 60.0, 0.0, 2.0 / 60.0, 9u, true,
              options);

  EXPECT_THAT(rope.points(), EndpointsAre(Vector2d(0.0, 5.0), Vector2d(100.0, 5.0)));
  const std::size_t middle = rope.points().size() / 2u;
  EXPECT_THAT(rope.points()[middle],
              Vector2dNear(Vector2d(before[middle].x, before[middle].y + 5.0), 1e-9));
}

}  // namespace donner::editor
