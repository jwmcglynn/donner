#include "donner/editor/FrameCallbackAdmission.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {
namespace {

TEST(FrameCallbackAdmissionTest, DiagnosticCountersPreserveGuardAndSeparateFrameOutcomes) {
  FrameAttemptStats stats;
  for (bool event : {false, true}) {
    EXPECT_TRUE(stats.admit(event, false, false));
    stats.finish(event, true);
    EXPECT_TRUE(stats.admit(event, false, false));
    stats.finish(event, false);
    EXPECT_FALSE(stats.admit(event, true, false));
    EXPECT_FALSE(stats.admit(event, false, true));
    EXPECT_FALSE(stats.admit(event, true, true));
  }
  EXPECT_THAT(stats.attempts, testing::ElementsAre(5, 5));
  EXPECT_THAT(stats.rendered, testing::ElementsAre(1, 1));
  EXPECT_THAT(stats.idle, testing::ElementsAre(1, 1));
  EXPECT_EQ(stats.blockedByFrame, 4u);
  EXPECT_EQ(stats.blockedByGpu, 4u);
}

TEST(FrameCallbackAdmissionTest, EventFrameCannotPostponeAnOwedAnimationBoundary) {
  const double boundaryMs = 1000.0 / 60.0;
  const auto next = NextScheduledFrameDeadline(boundaryMs, 2 * boundaryMs, true);
  ASSERT_TRUE(next.has_value());
  EXPECT_DOUBLE_EQ(*next, boundaryMs);
  EXPECT_LE(*next, 20.0) << "The scheduled callback at 20 ms must still be admitted";
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

TEST(EventFrameWakeTest, RequestsBeforeTheFrameStartsShareOneFrame) {
  EventFrameWake wake;
  const auto generation = wake.start();
  EXPECT_TRUE(wake.request(generation)) << "The first request must queue a frame";
  for (int request = 0; request < 1024; ++request) {
    EXPECT_FALSE(wake.request(generation)) << "A queued frame already serves later requests";
  }
  const auto served = wake.begin();
  EXPECT_FALSE(wake.finish(generation, served)) << "Every request landed before the frame began";
  EXPECT_TRUE(wake.request(generation)) << "Admission is released once the frame finishes";
}

TEST(EventFrameWakeTest, RequestDuringRunningFrameQueuesExactlyOneMore) {
  EventFrameWake wake;
  const auto generation = wake.start();
  ASSERT_TRUE(wake.request(generation));
  const auto served = wake.begin();
  // The running frame still holds admission, so this request cannot queue its own frame.
  EXPECT_FALSE(wake.request(generation));
  EXPECT_FALSE(wake.request(generation));
  ASSERT_TRUE(wake.finish(generation, served))
      << "A request the running frame did not serve must not be dropped";
  EXPECT_FALSE(wake.request(generation)) << "The follow-up frame is already queued";
  const auto followUp = wake.begin();
  EXPECT_FALSE(wake.finish(generation, followUp));
}

TEST(EventFrameWakeTest, RequestAfterFrameFinishesQueuesItsOwnFrame) {
  EventFrameWake wake;
  const auto generation = wake.start();
  ASSERT_TRUE(wake.request(generation));
  ASSERT_FALSE(wake.finish(generation, wake.begin()));
  EXPECT_TRUE(wake.request(generation));
}

TEST(EventFrameWakeTest, CancelledPostAllowsTheNextRequestToQueue) {
  EventFrameWake wake;
  const auto generation = wake.start();
  ASSERT_TRUE(wake.request(generation));
  wake.cancel(generation);
  EXPECT_TRUE(wake.request(generation));
}

TEST(EventFrameWakeTest, StoppedGenerationNeverQueuesAnotherFrame) {
  EventFrameWake wake;
  const auto generation = wake.start();
  ASSERT_TRUE(wake.request(generation));
  const auto served = wake.begin();
  EXPECT_FALSE(wake.request(generation));
  wake.stop();
  EXPECT_FALSE(wake.isCurrent(generation));
  EXPECT_FALSE(wake.finish(generation, served)) << "Shutdown wins over an unserved request";
  EXPECT_FALSE(wake.request(generation));
  EXPECT_EQ(wake.generation(), 0u);
}

TEST(EventFrameWakeTest, StaleFrameCannotReleaseOrRequeueNewGeneration) {
  EventFrameWake wake;
  const auto oldGeneration = wake.start();
  ASSERT_TRUE(wake.request(oldGeneration));
  const auto staleServed = wake.begin();
  wake.stop();
  const auto newGeneration = wake.start();
  ASSERT_NE(newGeneration, oldGeneration);
  ASSERT_TRUE(wake.request(newGeneration));
  EXPECT_FALSE(wake.finish(oldGeneration, staleServed));
  EXPECT_FALSE(wake.request(newGeneration)) << "The stale frame must not release the new one";
  EXPECT_FALSE(wake.finish(newGeneration, wake.begin()));
  EXPECT_TRUE(wake.request(newGeneration));
}

}  // namespace
}  // namespace donner::editor
