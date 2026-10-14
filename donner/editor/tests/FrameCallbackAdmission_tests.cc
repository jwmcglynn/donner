#include "donner/editor/FrameCallbackAdmission.h"

#include <gtest/gtest.h>

namespace donner::editor {
namespace {

TEST(FrameCallbackAdmissionTest, EventFrameCannotPostponeAnOwedAnimationBoundary) {
  const double boundaryMs = 1000.0 / 60.0;
  const auto next = NextScheduledFrameDeadline(boundaryMs, 2 * boundaryMs, true);
  ASSERT_TRUE(next.has_value());
  EXPECT_DOUBLE_EQ(*next, boundaryMs);
  EXPECT_LE(*next, 20.0) << "The scheduled callback at20ms must still be admitted";
  EXPECT_EQ(NextScheduledFrameDeadline(next, 2 * boundaryMs, false), 2 * boundaryMs);
}

TEST(FrameCallbackAdmissionTest, EventFrameKeepsEarlierDeadlineUntilScheduledFrameServesIt) {
  EXPECT_EQ(NextScheduledFrameDeadline(10.0, std::nullopt, true), 10.0);
  EXPECT_EQ(NextScheduledFrameDeadline(10.0, 5.0, true), 5.0);
  EXPECT_EQ(NextScheduledFrameDeadline(std::nullopt, 5.0, true), 5.0);
  EXPECT_EQ(NextScheduledFrameDeadline(10.0, std::nullopt, false), std::nullopt);
  EXPECT_EQ(NextScheduledFrameDeadline(std::nullopt, std::nullopt, true), std::nullopt);
}

TEST(FrameCallbackAdmissionTest, OnePendingFrameIncludesDeferredExecution) {
  FrameCallbackAdmission admission;
  const auto generation = admission.start();
  ASSERT_TRUE(admission.acquire(generation));
  for (int tick = 0; tick < 1024; ++tick) {
    EXPECT_FALSE(admission.acquire(generation));
  }
  ASSERT_TRUE(admission.isCurrent(generation));
  // Delivering the notification alone must not release the actual deferred frame.
  EXPECT_FALSE(admission.acquire(generation));
  admission.complete(generation);
  EXPECT_TRUE(admission.acquire(generation));
}

TEST(FrameCallbackAdmissionTest, FailedPostAllowsNextTick) {
  FrameCallbackAdmission admission;
  const auto generation = admission.start();
  ASSERT_TRUE(admission.acquire(generation));
  admission.complete(generation);
  EXPECT_TRUE(admission.acquire(generation));
}

TEST(FrameCallbackAdmissionTest, StopInvalidatesQueuedAndRunningCallbacks) {
  FrameCallbackAdmission admission;
  const auto generation = admission.start();
  ASSERT_TRUE(admission.acquire(generation));
  admission.stop();
  EXPECT_FALSE(admission.isCurrent(generation));
  EXPECT_FALSE(admission.acquire(generation));
  EXPECT_FALSE(admission.isCurrent(0));
  admission.complete(generation);
  EXPECT_EQ(admission.generation(), 0u);
}

TEST(FrameCallbackAdmissionTest, StaleCompletionCannotReleaseNewGeneration) {
  FrameCallbackAdmission admission;
  const auto oldGeneration = admission.start();
  ASSERT_TRUE(admission.acquire(oldGeneration));
  admission.stop();
  const auto newGeneration = admission.start();
  ASSERT_NE(newGeneration, oldGeneration);
  ASSERT_TRUE(admission.acquire(newGeneration));
  admission.complete(oldGeneration);
  EXPECT_FALSE(admission.isCurrent(oldGeneration));
  EXPECT_FALSE(admission.acquire(newGeneration));
  admission.complete(newGeneration);
  EXPECT_TRUE(admission.acquire(newGeneration));
}

}  // namespace
}  // namespace donner::editor
