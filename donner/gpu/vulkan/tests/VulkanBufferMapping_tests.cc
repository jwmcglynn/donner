/// @file
/// Host buffer mapping executed on Vulkan: a mapping waits for the submission that fills its
/// buffer, reads exactly the range it named, and fails closed once it is released, its buffer is
/// destroyed, or the range does not fit.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/tests/BufferMappingScene.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/tests/NativeQueueGate.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan {
namespace {

using gpu::tests::ExpectDestroyedBufferInvalidatesMapping;
using gpu::tests::ExpectMappingWaitsForItsSubmission;
using gpu::tests::ExpectMapReadUsageIsRequired;
using gpu::tests::ExpectOneMappingPerBuffer;
using gpu::tests::ExpectRangePastTheEndIsRefused;
using gpu::tests::ExpectReadBeforeCompletionIsRefused;
using gpu::tests::ExpectSubrangeMappingReadsItsOwnBytes;
using gpu::tests::ExpectUnmapEndsAccess;
using gpu::tests::kMappingSceneByteSize;
using gpu::tests::MappingScene;
using gpu::tests::SceneWaitParams;

class VulkanBufferMappingTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::Create();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available; the "
                  "host mapping gate must not be skipped on this runner";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  void TearDown() override {
    if (device_) {
      EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
    }
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanBufferMappingTest, AMappingWaitsForTheSubmissionThatFillsIt) {
  ExpectMappingWaitsForItsSubmission(*device_);
}

TEST_F(VulkanBufferMappingTest, ASubrangeMappingReadsItsOwnBytes) {
  ExpectSubrangeMappingReadsItsOwnBytes(*device_);
}

TEST_F(VulkanBufferMappingTest, ReadingBeforeCompletionIsRefused) {
  ExpectReadBeforeCompletionIsRefused(*device_);
}

TEST_F(VulkanBufferMappingTest, OneBufferCarriesOneMappingAtATime) {
  ExpectOneMappingPerBuffer(*device_);
}

TEST_F(VulkanBufferMappingTest, UnmappingEndsAccessThroughTheMapping) {
  ExpectUnmapEndsAccess(*device_);
}

TEST_F(VulkanBufferMappingTest, DestroyingTheBufferInvalidatesItsMapping) {
  ExpectDestroyedBufferInvalidatesMapping(*device_);
}

TEST_F(VulkanBufferMappingTest, ARangePastTheEndIsRefused) {
  ExpectRangePastTheEndIsRefused(*device_);
}

TEST_F(VulkanBufferMappingTest, MapReadUsageIsRequired) {
  ExpectMapReadUsageIsRequired(*device_);
}

TEST_F(VulkanBufferMappingTest, AWriteStillWaitingForTheQueueBlocksMapping) {
  // A write to a busy buffer is copied into the pending queue and applied at the beginning of the
  // next submission. That submission can be unrelated work, so it would land after a mapping
  // taken in between had already reported itself ready, changing bytes the host was reading. The
  // mapping is refused until the queue is drained.
  std::unique_ptr<VulkanDevice> gated = VulkanDevice::CreateWithTimelineSemaphoreForTest();
  if (!gated) {
    GTEST_SKIP() << "Device lacks VK_KHR_timeline_semaphore; the queue gate needs it";
  }
  const Buffer buffer = GetResultOrFail(gated->createBuffer(
      BufferDescriptor{"queuedWrite", kMappingSceneByteSize,
                       BufferUsage::CopyDst | BufferUsage::CopySrc | BufferUsage::MapRead}));

  tests::NativeQueueGate gate(gated->nativeContextForTest());
  ASSERT_NO_FATAL_FAILURE(gate.start());

  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*gated, scene));
  const std::vector<uint8_t> payload(16, 0x7C);
  ASSERT_THAT(gated->writeBuffer(buffer, 0, payload), IsOk());
  ASSERT_GT(gated->bufferWriteStatsForTest().pendingBytes, 0u) << "the write must have queued";

  EXPECT_THAT(gated->mapBufferAsync(buffer, MapMode::Read, 0, kMappingSceneByteSize),
              IsGpuError(GpuErrorType::InvalidState))
      << "A mapping must not be taken while a queued write for that buffer is still unapplied";

  ASSERT_EQ(gate.release(), VK_SUCCESS);
  // An ordinary submission applies the queued writes first.
  const uint64_t flushSerial = GetResultOrFail(
      gated->submit(GetResultOrFail(GetResultOrFail(gated->createCommandEncoder())->finish())));
  ASSERT_THAT(gated->waitForSerial(flushSerial, 30.0), testing::IsTrue())
      << gated->lastErrorForTest();
  ASSERT_EQ(gated->bufferWriteStatsForTest().pendingBytes, 0u) << "the queue must have drained";

  BufferMapping mapping =
      GetResultOrFail(gated->mapBufferAsync(buffer, MapMode::Read, 0, kMappingSceneByteSize));
  ASSERT_EQ(GetResultOrFail(gated->waitForMapping(mapping, SceneWaitParams(), {})),
            MapWaitOutcome::Ready);
  const std::span<const uint8_t> bytes = GetResultOrFail(gated->mappedBytes(mapping));
  EXPECT_EQ(bytes[0], 0x7C) << "the drained write must be what the mapping reads";
  EXPECT_THAT(gated->unmapBuffer(std::move(mapping)), IsOk());
}

TEST_F(VulkanBufferMappingTest, AMappingReadsWhatTheReadbackAccessorReads) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*device_, scene));
  const std::vector<uint8_t> expected = GetResultOrFail(device_->readBackBuffer(scene.readback));

  BufferMapping mapping = GetResultOrFail(
      device_->mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));
  ASSERT_EQ(GetResultOrFail(device_->waitForMapping(mapping, SceneWaitParams(), {})),
            MapWaitOutcome::Ready);

  const std::span<const uint8_t> mapped = GetResultOrFail(device_->mappedBytes(mapping));
  EXPECT_THAT(std::vector<uint8_t>(mapped.begin(), mapped.end()),
              testing::ElementsAreArray(expected))
      << "Mapping and the readback accessor must report the same bytes";
  EXPECT_THAT(device_->unmapBuffer(std::move(mapping)), IsOk());
}

}  // namespace
}  // namespace donner::gpu::vulkan
