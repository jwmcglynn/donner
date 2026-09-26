/// @file
/// Nightly CPU timing of the same Geode fill commands on recording and available GPU backends.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

#include "donner/gpu/RecordingDevice.h"
#include "donner/svg/renderer/geode/benchmarks/DrawCpuWorkload.h"

#if defined(__APPLE__)
#include "donner/gpu/metal/MetalDevice.h"
#elif defined(__linux__)
#include "donner/gpu/vulkan/VulkanDevice.h"
#endif

namespace donner::geode::benchmarks {
namespace {

constexpr std::array<uint32_t, 3> kDrawCounts = {1, 100, 10000};
constexpr uint32_t kSamples = 3;

void ReportSamples(const char* backend, gpu::Device& device) {
  DrawCpuWorkload workload(device);
  ASSERT_TRUE(workload.initialize()) << backend;
  for (uint32_t draws : kDrawCounts) {
    ASSERT_THAT(workload.run(draws), testing::Optional(testing::_)) << backend << " warmup";
    std::vector<uint64_t> recordingNs;
    std::vector<uint64_t> submissionNs;
    for (uint32_t sampleIndex = 0; sampleIndex < kSamples; ++sampleIndex) {
      const std::optional<DrawCpuSample> sample = workload.run(draws);
      ASSERT_THAT(sample, testing::Optional(testing::_)) << backend << " N=" << draws;
      recordingNs.push_back(sample->recordingNs);
      submissionNs.push_back(sample->submissionNs);
    }
    std::sort(recordingNs.begin(), recordingNs.end());
    std::sort(submissionNs.begin(), submissionNs.end());
    const uint64_t recordMedianNs = recordingNs[kSamples / 2];
    const uint64_t submitMedianNs = submissionNs[kSamples / 2];
    std::fprintf(stderr,
                 "gpu_draw_cpu backend=%s draws=%u samples=%u record_total_ns=%llu "
                 "record_ns_per_draw=%.3f submit_total_ns=%llu submit_ns_per_draw=%.3f\n",
                 backend, draws, kSamples, static_cast<unsigned long long>(recordMedianNs),
                 static_cast<double>(recordMedianNs) / draws,
                 static_cast<unsigned long long>(submitMedianNs),
                 static_cast<double>(submitMedianNs) / draws);
  }
}

TEST(DrawCpuBenchmarkWallclock, RecordingBackend) {
  gpu::RecordingDevice device;
  ReportSamples("recording", device);
}

#if defined(__APPLE__)
TEST(DrawCpuBenchmarkWallclock, NativeMetal) {
  std::unique_ptr<gpu::metal::MetalDevice> device = gpu::metal::MetalDevice::Create();
  ASSERT_NE(device, nullptr);
  ReportSamples("metal", *device);
}
#endif

#if defined(__linux__)
TEST(DrawCpuBenchmarkWallclock, NativeVulkan) {
  std::unique_ptr<gpu::vulkan::VulkanDevice> device = gpu::vulkan::VulkanDevice::Create();
  ASSERT_NE(device, nullptr);
  ReportSamples("vulkan", *device);
}
#endif

}  // namespace
}  // namespace donner::geode::benchmarks
