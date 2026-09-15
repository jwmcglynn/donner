/// @file
/// Host buffer mapping executed on Metal: a mapping waits for the submission that fills its
/// buffer, reads exactly the range it named, and fails closed once it is released, its buffer is
/// destroyed, or the range does not fit.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/BufferMappingScene.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal {
namespace {

using gpu::tests::ExpectDestroyedBufferInvalidatesMapping;
using gpu::tests::ExpectMappingWaitsForItsSubmission;
using gpu::tests::ExpectMapReadUsageIsRequired;
using gpu::tests::ExpectOneMappingPerBuffer;
using gpu::tests::ExpectRangePastTheEndIsRefused;
using gpu::tests::ExpectReadBeforeCompletionIsRefused;
using gpu::tests::ExpectSceneTexels;
using gpu::tests::ExpectSubrangeMappingReadsItsOwnBytes;
using gpu::tests::ExpectUnmapEndsAccess;
using gpu::tests::kMappingSceneByteSize;
using gpu::tests::MappingScene;
using gpu::tests::SceneWaitParams;

class MetalBufferMappingTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal host buffer mapping");
  }

  void TearDown() override {
    if (device_) {
      EXPECT_THAT(device_->lastErrorForTest(), testing::IsEmpty());
    }
  }

  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalBufferMappingTest, AMappingWaitsForTheSubmissionThatFillsIt) {
  ExpectMappingWaitsForItsSubmission(*device_);
}

TEST_F(MetalBufferMappingTest, ASubrangeMappingReadsItsOwnBytes) {
  ExpectSubrangeMappingReadsItsOwnBytes(*device_);
}

TEST_F(MetalBufferMappingTest, ReadingBeforeCompletionIsRefused) {
  ExpectReadBeforeCompletionIsRefused(*device_);
}

TEST_F(MetalBufferMappingTest, OneBufferCarriesOneMappingAtATime) {
  ExpectOneMappingPerBuffer(*device_);
}

TEST_F(MetalBufferMappingTest, UnmappingEndsAccessThroughTheMapping) {
  ExpectUnmapEndsAccess(*device_);
}

TEST_F(MetalBufferMappingTest, DestroyingTheBufferInvalidatesItsMapping) {
  ExpectDestroyedBufferInvalidatesMapping(*device_);
}

TEST_F(MetalBufferMappingTest, ARangePastTheEndIsRefused) {
  ExpectRangePastTheEndIsRefused(*device_);
}

TEST_F(MetalBufferMappingTest, MapReadUsageIsRequired) {
  ExpectMapReadUsageIsRequired(*device_);
}

TEST_F(MetalBufferMappingTest, AMappingStaysPendingWhileItsSubmissionIsHeld) {
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*device_, scene));

  BufferMapping mapping = GetResultOrFail(
      device_->mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));
  // A budget short enough to expire while the submission is still gated, which is the only way to
  // observe "not ready" without racing the GPU.
  EXPECT_EQ(GetResultOrFail(device_->waitForMapping(mapping, MapWaitParams{0.001, 0.05}, {})),
            MapWaitOutcome::TimedOut);
  EXPECT_THAT(device_->mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidState));

  device_->resumeSubmissionsForTest();

  EXPECT_EQ(GetResultOrFail(device_->waitForMapping(mapping, SceneWaitParams(), {})),
            MapWaitOutcome::Ready);
  ASSERT_NO_FATAL_FAILURE(
      ExpectSceneTexels(GetResultOrFail(device_->mappedBytes(mapping)), "released submission"));
  EXPECT_THAT(device_->unmapBuffer(std::move(mapping)), IsOk());
}

TEST_F(MetalBufferMappingTest, AManagedBufferIsSynchronizedBeforeItsMappingReadsIt) {
  // On a device without unified memory the GPU and host copies of a buffer are distinct, and the
  // submission that wrote it has to publish its changes back before the host reads them. Forcing
  // that memory model exercises the publish step on hardware whose real model would skip it.
  device_ = MetalDevice::Create(MetalDevice::MemoryModel::ForceNonUnified);
  ASSERT_NE(device_, nullptr);
  ASSERT_THAT(device_->usesUnifiedMemoryForTest(), testing::IsFalse());
  const uint64_t publishesBefore = device_->deviceWritePublishCountForTest();

  ExpectMappingWaitsForItsSubmission(*device_);

  EXPECT_THAT(device_->deviceWritePublishCountForTest(), testing::Gt(publishesBefore))
      << "The mapped buffer's submission must publish its device writes back to the host copy, "
         "or the mapping would read a stale mirror";
}

TEST_F(MetalBufferMappingTest, AMappingReadsWhatTheReadbackAccessorReads) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*device_, scene));
  ASSERT_THAT(device_->waitForSerial(scene.serial, 30.0), testing::IsTrue())
      << device_->lastErrorForTest();
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
}  // namespace donner::gpu::metal
