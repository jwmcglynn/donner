/// @file
/// Device loss on Metal: a loss declared anywhere over the backend root ends this device's
/// mappings, and a command buffer that fails on the GPU declares the root lost as a
/// backend-reported loss rather than leaving a later wait to misattribute it as a timeout.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
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

  /// Finishes one empty command buffer on the device under test.
  CommandBuffer emptyCommandBuffer() {
    return GetResultOrFail(GetResultOrFail(device_->createCommandEncoder())->finish());
  }

  /// Submits one empty command buffer and returns its serial.
  uint64_t submitEmptyWork() { return GetResultOrFail(device_->submit(emptyCommandBuffer())); }

  /// Milliseconds \p action took.
  /// @param action Work to time.
  static int64_t MillisecondsTaken(const std::function<void()>& action) {
    const auto start = std::chrono::steady_clock::now();
    action();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
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

TEST_F(MetalDeviceLossTest, ADeclaredRootLossRefusesRepeatedSubmissionsBeforeCommit) {
  // Prepare valid work before the sibling's declaration, so encoder admission does not mask the
  // submission gate. A logical loss leaves the native Metal device usable by the driver.
  std::array<CommandBuffer, 3> prepared{emptyCommandBuffer(), emptyCommandBuffer(),
                                        emptyCommandBuffer()};
  ASSERT_THAT(DeclareDeviceLost(*rootLoss_), IsTrue());
  for (CommandBuffer& buffer : prepared) {
    EXPECT_THAT(device_->submit(std::move(buffer)), IsGpuError(GpuErrorType::DeviceLost));
    EXPECT_THAT(device_->lastSubmittedSerial(), Eq(0u))
        << "work over the lost root must never be accepted into flight";
  }
  EXPECT_THAT(device_->writeStatsForTest().inFlightStagingBytes, Eq(0u));
}

TEST_F(MetalDeviceLossTest, AFailedCommandBufferDeclaresTheRootLostAsABackendReport) {
  const std::unique_ptr<MetalDevice> sibling = openDeviceOverTheRoot();
  ASSERT_THAT(sibling, NotNull());

  device_->failNextSubmissionForTest();
  const uint64_t serial = submitEmptyWork();
  ASSERT_THAT(device_->waitForCompletionHandlersForTest(1, 30.0), IsTrue());

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

TEST_F(MetalDeviceLossTest, AFailedEarlierCommandBufferFailsItsWholeSubmission) {
  const std::unique_ptr<MetalDevice> sibling = openDeviceOverTheRoot();
  ASSERT_THAT(sibling, NotNull());

  // A frame submits several command buffers at once; the first one fails and the last does not.
  device_->failNextSubmissionForTest(/*commandBufferIndex=*/0);
  std::array<CommandBuffer, 2> frame{emptyCommandBuffer(), emptyCommandBuffer()};
  const uint64_t serial = GetResultOrFail(device_->submit(frame));
  ASSERT_THAT(device_->waitForCompletionHandlersForTest(1, 30.0), IsTrue());

  EXPECT_THAT(device_->waitForSerial(serial, 30.0), IsFalse())
      << "a submission whose earlier buffer failed must not read as finished";
  EXPECT_THAT(device_->lastErrorForTest(), HasSubstr("injected command buffer failure"));
  EXPECT_THAT(device_->isLost(), IsTrue());
  EXPECT_THAT(sibling->isLost(), IsTrue());
}

TEST_F(MetalDeviceLossTest, ALaterSuccessDoesNotCompleteAnEarlierSubmissionStillFinishing) {
  // The earlier submission fails, but its completion is not published yet when the later,
  // successful one completes.
  device_->failNextSubmissionForTest();
  device_->holdNextCompletionForTest();
  const uint64_t earlier = submitEmptyWork();
  const uint64_t later = submitEmptyWork();
  ASSERT_THAT(device_->waitForCompletionHandlersForTest(2, 30.0), IsTrue());

  EXPECT_THAT(device_->completedSerial(), Lt(earlier));
  EXPECT_THAT(device_->waitForSerial(earlier, 0.2), IsFalse())
      << "work whose outcome is not known yet must not read as finished";

  device_->releaseHeldCompletionForTest();

  EXPECT_THAT(device_->isLost(), IsTrue());
  EXPECT_THAT(device_->waitForSerial(earlier, 30.0), IsFalse());
  EXPECT_THAT(device_->waitForSerial(later, 30.0), IsFalse())
      << "a submission after failed work runs on a lost root";
}

TEST_F(MetalDeviceLossTest, AWaitEndsAtOnceWhenASiblingDeclaresTheRootLost) {
  const std::unique_ptr<MetalDevice> sibling = openDeviceOverTheRoot();
  ASSERT_THAT(sibling, NotNull());
  device_->holdNextCompletionForTest();
  const uint64_t serial = submitEmptyWork();
  ASSERT_THAT(device_->waitForCompletionHandlersForTest(1, 30.0), IsTrue());

  sibling->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a sibling's queue drain gave up");

  bool completed = true;
  const int64_t waitedMs =
      MillisecondsTaken([&] { completed = device_->waitForSerial(serial, 5.0); });
  EXPECT_THAT(completed, IsFalse());
  EXPECT_THAT(waitedMs, Lt(1000)) << "a wait on a lost root must not spend its budget";

  device_->releaseHeldCompletionForTest();
}

TEST_F(MetalDeviceLossTest, TeardownAfterASiblingDeclaredTheRootLostReturnsPromptly) {
  const std::unique_ptr<MetalDevice> sibling = openDeviceOverTheRoot();
  ASSERT_THAT(sibling, NotNull());
  // Work that never finishes, as on a GPU that stopped answering.
  device_->holdNextCompletionForTest();
  (void)submitEmptyWork();
  ASSERT_THAT(device_->waitForCompletionHandlersForTest(1, 30.0), IsTrue());

  sibling->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a sibling's queue drain gave up");

  const int64_t teardownMs = MillisecondsTaken([&] { device_.reset(); });
  EXPECT_THAT(teardownMs, Lt(1000))
      << "teardown skips GPU waits once the root is lost, rather than waiting out a hung GPU";
}

}  // namespace
}  // namespace donner::gpu::metal
