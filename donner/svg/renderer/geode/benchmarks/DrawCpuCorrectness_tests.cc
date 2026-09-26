/// @file
/// Default-suite guard for the draw-count contract used by the CPU benchmark.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/gpu/RecordingDevice.h"
#include "donner/svg/renderer/geode/benchmarks/DrawCpuWorkload.h"

namespace donner::geode::benchmarks {
namespace {

TEST(DrawCpuBenchmarkCorrectness, CountsEveryRecordedGeodeFillDraw) {
  gpu::RecordingDevice device;
  DrawCpuWorkload workload(device);
  ASSERT_TRUE(workload.initialize());
  for (uint32_t draws : {1u, 100u}) {
    const std::optional<DrawCpuSample> sample = workload.run(draws);
    ASSERT_THAT(sample, testing::Optional(testing::_));
    EXPECT_THAT(sample->draws, testing::Eq(draws));
    EXPECT_THAT(sample->recordingNs, testing::Gt(0u));
    EXPECT_THAT(sample->submissionNs, testing::Gt(0u));
  }
}

}  // namespace
}  // namespace donner::geode::benchmarks
