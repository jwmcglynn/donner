/// @file
/// Device loss on Metal: a loss declared anywhere over the backend root ends this device's
/// mappings, and a command buffer that fails on the GPU declares the root lost as a
/// backend-reported loss rather than leaving a later wait to misattribute it as a timeout.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/BufferMappingScene.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal {
namespace {

using gpu::tests::kMappingSceneByteSize;
using gpu::tests::MappingScene;
using gpu::tests::SceneWaitParams;
using testing::Eq;
using testing::HasSubstr;
using testing::IsFalse;
using testing::IsTrue;
using testing::Lt;
using testing::NotNull;

/// A Metal device sharing its loss condition with every other device over the same root, which is
/// how a selected backend opens its devices.
class MetalDeviceLossTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = openDeviceOverTheRoot();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal device loss");
  }

  /// Opens another device over the same root, sharing its loss condition.
  std::unique_ptr<MetalDevice> openDeviceOverTheRoot() const {
    return MetalDevice::Create(MetalDevice::MemoryModel::Detected, kMaxBufferByteSize,
                               std::chrono::seconds(5), rootLoss_);
  }

  /// Submits one empty command buffer and returns its serial.
  uint64_t submitEmptyWork() {
    return GetResultOrFail(device_->submit(
        GetResultOrFail(GetResultOrFail(device_->createCommandEncoder())->finish())));
  }

  std::shared_ptr<DeviceLostState> rootLoss_ = std::make_shared<DeviceLostState>();
  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalDeviceLossTest, ALossDeclaredOverTheRootEndsAPendingMappingWithinASlice) {
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*device_, scene));
  BufferMapping mapping = GetResultOrFail(
      device_->mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));

  // Another device over the same root gives up on its own bounded wait while this mapping is
  // still waiting for its held submission.
  ASSERT_THAT(DeclareDeviceLost(*rootLoss_), IsTrue());

  // A budget far longer than one slice, so a wait that ran it out cannot pass as prompt.
  const auto waitStart = std::chrono::steady_clock::now();
  const MapWaitReport report =
      GetResultOrFail(device_->waitForMapping(mapping, MapWaitParams{0.001, 10.0}, {}));
  const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - waitStart)
                            .count();
  EXPECT_THAT(report.outcome, Eq(MapWaitOutcome::DeviceLost))
      << "a mapping over a lost root can never complete, so waiting out its budget would report a "
         "permanent failure as a slow one";
  EXPECT_THAT(waitedMs, Lt(1000)) << "the loss must end the wait at its first readiness check";
  EXPECT_THAT(device_->mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidState))
      << "a mapping that never completed has no bytes to read";

  device_->resumeSubmissionsForTest();
  EXPECT_THAT(device_->unmapBuffer(std::move(mapping)), IsOk());
}

TEST_F(MetalDeviceLossTest, ALossDeclaredOverTheRootOutranksACompletedSubmission) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*device_, scene));
  ASSERT_THAT(device_->waitForSerial(scene.serial, 30.0), IsTrue()) << device_->lastErrorForTest();
  BufferMapping mapping = GetResultOrFail(
      device_->mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));

  ASSERT_THAT(DeclareDeviceLost(*rootLoss_), IsTrue());

  EXPECT_THAT(GetResultOrFail(device_->waitForMapping(mapping, SceneWaitParams(), {})).outcome,
              Eq(MapWaitOutcome::DeviceLost))
      << "a completed serial does not make bytes trustworthy once the root is lost";
  EXPECT_THAT(device_->mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidState))
      << "a mapping that never completed has no bytes to read";
  EXPECT_THAT(device_->unmapBuffer(std::move(mapping)), IsOk());
}

TEST_F(MetalDeviceLossTest, ALossDeclaredOverTheRootEndsReadsThroughACompletedMapping) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*device_, scene));
  BufferMapping mapping = GetResultOrFail(
      device_->mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));
  ASSERT_THAT(GetResultOrFail(device_->waitForMapping(mapping, SceneWaitParams(), {})).outcome,
              Eq(MapWaitOutcome::Ready))
      << device_->lastErrorForTest();
  ASSERT_THAT(device_->mappedBytes(mapping), HasResult());

  ASSERT_THAT(DeclareDeviceLost(*rootLoss_), IsTrue());

  EXPECT_THAT(device_->mappedBytes(mapping), IsGpuError(GpuErrorType::DeviceLost))
      << "reads through a mapping end once the root is lost, whichever device declared it";
  EXPECT_THAT(device_->unmapBuffer(std::move(mapping)), IsOk());
}

TEST_F(MetalDeviceLossTest, AFailedCommandBufferDeclaresTheRootLostAsABackendReport) {
  const std::unique_ptr<MetalDevice> sibling = openDeviceOverTheRoot();
  ASSERT_THAT(sibling, NotNull());

  device_->failNextCompletionForTest();
  const uint64_t serial = submitEmptyWork();

  EXPECT_THAT(device_->waitForSerial(serial, 30.0), IsFalse());
  EXPECT_THAT(device_->lastErrorForTest(), HasSubstr("injected command buffer failure"));
  EXPECT_THAT(device_->isLost(), IsTrue())
      << "work that failed on the GPU leaves the root in an unknown state";
  EXPECT_THAT(sibling->isLost(), IsTrue()) << "the loss belongs to the root, not to one device";
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::None))
      << "the backend reported this loss; no wait gave up";

  // A wait that gives up afterwards is a consequence of the loss and must not claim it.
  device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a later queue drain");
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::None));
}

}  // namespace
}  // namespace donner::gpu::metal
