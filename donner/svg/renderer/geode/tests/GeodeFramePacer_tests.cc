#include "donner/svg/renderer/geode/GeodeFramePacer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace donner::geode {
namespace {

using testing::ElementsAre;
using testing::Eq;
using testing::Le;

/// A GPU that never finishes work on its own: a submission completes only when a wait asks for
/// it, which then completes everything up to that serial. It records every serial waited for.
struct HeldGpu {
  uint64_t submitted = 0;
  uint64_t completed = 0;
  std::vector<uint64_t> waitedFor;
  gpu::SerialWaitEnd waitEnd = gpu::SerialWaitEnd::Completed;

  uint64_t submit(int submissions) {
    submitted += static_cast<uint64_t>(submissions);
    return submitted;
  }

  GeodeFramePacer::Backend backend(bool canWait = true) {
    GeodeFramePacer::Backend result;
    result.completedSerial = [this] { return completed; };
    if (canWait) {
      result.waitForSerial = [this](uint64_t serial) {
        waitedFor.push_back(serial);
        if (waitEnd == gpu::SerialWaitEnd::Completed) {
          completed = std::max(completed, serial);
        }
        return gpu::SerialWaitResult{waitEnd, std::chrono::milliseconds{3}};
      };
    }
    return result;
  }
};

TEST(GeodeFramePacer, DefaultsToTwoFramesInFlight) {
  EXPECT_THAT(GeodeFramePacer().maxFramesInFlight(), Eq(2u));
}

TEST(GeodeFramePacer, FramesInFlightNeverExceedTheLimit) {
  for (uint32_t limit = 1; limit <= 3; ++limit) {
    SCOPED_TRACE(testing::Message() << "limit " << limit);
    HeldGpu gpu;
    GeodeFramePacer pacer;
    pacer.setMaxFramesInFlight(limit);
    constexpr int kFrames = 12;
    uint32_t waits = 0;
    for (int frame = 0; frame < kFrames; ++frame) {
      waits += pacer.admitFrame(gpu.backend()).waits;
      // Each frame submits a few times, like a frame with filter chunks.
      pacer.frameSubmitted(gpu.submit(3));
      EXPECT_THAT(pacer.framesInFlight(gpu.backend()), Le(limit));
      EXPECT_THAT(gpu.submitted - gpu.completed, Le(3u * limit));
    }
    EXPECT_THAT(waits, Eq(static_cast<uint32_t>(kFrames) - limit))
        << "once the limit is reached, each frame waits for exactly one earlier frame";
  }
}

TEST(GeodeFramePacer, WaitsForTheOldestFrameOnly) {
  HeldGpu gpu;
  GeodeFramePacer pacer;
  pacer.setMaxFramesInFlight(2);
  for (int frame = 0; frame < 4; ++frame) {
    (void)pacer.admitFrame(gpu.backend());
    pacer.frameSubmitted(gpu.submit(2));
  }
  // Frames end at serials 2, 4, 6, 8. Admitting the third frame waits for the first; the fourth,
  // for the second.
  EXPECT_THAT(gpu.waitedFor, ElementsAre(2u, 4u));
}

TEST(GeodeFramePacer, CompletedFramesAdmitWithoutWaiting) {
  HeldGpu gpu;
  GeodeFramePacer pacer;
  pacer.setMaxFramesInFlight(1);
  for (int frame = 0; frame < 5; ++frame) {
    const GeodeFramePacer::Admission admission = pacer.admitFrame(gpu.backend());
    EXPECT_THAT(admission.waits, Eq(0u));
    pacer.frameSubmitted(gpu.submit(1));
    gpu.completed = gpu.submitted;  // The GPU keeps up.
  }
  EXPECT_THAT(gpu.waitedFor, testing::IsEmpty());
}

TEST(GeodeFramePacer, AFrameThatSubmitsNothingNewAddsNoEntry) {
  HeldGpu gpu;
  GeodeFramePacer pacer;
  pacer.frameSubmitted(gpu.submit(1));
  pacer.frameSubmitted(gpu.submitted);
  pacer.frameSubmitted(0);
  EXPECT_THAT(pacer.framesInFlight(gpu.backend()), Eq(1u));
}

TEST(GeodeFramePacer, ABackendThatCannotBlockAdmitsEveryFrame) {
  HeldGpu gpu;
  GeodeFramePacer pacer;
  for (int frame = 0; frame < 5; ++frame) {
    const GeodeFramePacer::Admission admission = pacer.admitFrame(gpu.backend(/*canWait=*/false));
    EXPECT_THAT(admission.waits, Eq(0u));
    pacer.frameSubmitted(gpu.submit(1));
  }
  // The embedder reads the backlog to pace frames itself.
  EXPECT_THAT(pacer.framesInFlight(gpu.backend(false)), Eq(5u));
  gpu.completed = 4;
  EXPECT_THAT(pacer.framesInFlight(gpu.backend(false)), Eq(1u));
}

TEST(GeodeFramePacer, ZeroDisablesPacing) {
  HeldGpu gpu;
  GeodeFramePacer pacer;
  pacer.setMaxFramesInFlight(0);
  for (int frame = 0; frame < 5; ++frame) {
    (void)pacer.admitFrame(gpu.backend());
    pacer.frameSubmitted(gpu.submit(1));
  }
  EXPECT_THAT(gpu.waitedFor, testing::IsEmpty());
  EXPECT_THAT(pacer.framesInFlight(gpu.backend()), Eq(5u));
}

TEST(GeodeFramePacer, LimitClampsToTheMaximum) {
  GeodeFramePacer pacer;
  pacer.setMaxFramesInFlight(1000);
  EXPECT_THAT(pacer.maxFramesInFlight(), Eq(GeodeFramePacer::kMaxFramesInFlightLimit));
}

TEST(GeodeFramePacer, AStalledWaitReportsAndForgetsThePendingFrames) {
  HeldGpu gpu;
  GeodeFramePacer pacer;
  pacer.setMaxFramesInFlight(1);
  (void)pacer.admitFrame(gpu.backend());
  pacer.frameSubmitted(gpu.submit(1));
  gpu.waitEnd = gpu::SerialWaitEnd::Stalled;
  const GeodeFramePacer::Admission admission = pacer.admitFrame(gpu.backend());
  EXPECT_TRUE(admission.stalled);
  EXPECT_FALSE(admission.failed);
  EXPECT_THAT(admission.waits, Eq(0u));
  EXPECT_THAT(admission.waited, Eq(std::chrono::milliseconds{3}));
  EXPECT_THAT(pacer.framesInFlight(gpu.backend()), Eq(0u));
}

TEST(GeodeFramePacer, AFailedWaitReportsFailure) {
  HeldGpu gpu;
  GeodeFramePacer pacer;
  pacer.setMaxFramesInFlight(1);
  (void)pacer.admitFrame(gpu.backend());
  pacer.frameSubmitted(gpu.submit(1));
  gpu.waitEnd = gpu::SerialWaitEnd::Failed;
  const GeodeFramePacer::Admission admission = pacer.admitFrame(gpu.backend());
  EXPECT_TRUE(admission.failed);
  EXPECT_FALSE(admission.stalled);
}

}  // namespace
}  // namespace donner::geode
