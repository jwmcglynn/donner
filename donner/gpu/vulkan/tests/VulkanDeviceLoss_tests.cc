/// @file
/// Device loss on Vulkan: a device takes part in the loss condition of the root it opens over, so
/// a loss another device over that root declares ends its waits and mappings, and a submission the
/// driver reports as `VK_ERROR_DEVICE_LOST` declares the root lost as a backend-reported loss
/// rather than leaving a later wait to misattribute it as a timeout.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/tests/BufferMappingScene.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"
#include "donner/gpu/vulkan/tests/NativeQueueGate.h"

namespace donner::gpu::vulkan {
namespace {

using gpu::tests::kMappingSceneByteSize;
using gpu::tests::MappingScene;
using gpu::tests::SceneWaitParams;
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;
using testing::Lt;
using testing::NotNull;

/// Why a case that holds the queue open is skipped on a device without the test-only extension:
/// VK_KHR_timeline_semaphore is optional on a conforming Vulkan 1.1 driver.
constexpr std::string_view kNoQueueGate =
    "Device lacks VK_KHR_timeline_semaphore; the queue gate needs it";

/// A Vulkan device sharing its loss condition with every other device over the same root, which
/// is how a selected backend opens its devices.
class VulkanDeviceLossTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = openDeviceOverTheRoot();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available; the "
                  "device-loss gate must not be skipped on this runner";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  /// Opens another device over the same root, sharing its loss condition.
  std::unique_ptr<VulkanDevice> openDeviceOverTheRoot() const {
    return VulkanDevice::Create(rootLoss_);
  }

  /// Opens a device over the same root whose queue a test-owned gate can hold open.
  std::unique_ptr<VulkanDevice> openGatedDeviceOverTheRoot() const {
    return VulkanDevice::CreateWithTimelineSemaphoreForTest(rootLoss_);
  }

  /// Submits one empty command buffer on \p device and returns what the submission reported.
  /// @param device Device to submit on.
  static Result<uint64_t> SubmitEmptyCommandBuffer(VulkanDevice& device) {
    return device.submit(GetResultOrFail(GetResultOrFail(device.createCommandEncoder())->finish()));
  }

  /// Submits one empty command buffer on \p device and returns its serial.
  /// @param device Device to submit on.
  static uint64_t SubmitEmptyWork(VulkanDevice& device) {
    return GetResultOrFail(SubmitEmptyCommandBuffer(device));
  }

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
  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanDeviceLossTest, ALossASiblingDeclaresEndsAPendingMappingWithinASlice) {
  const std::unique_ptr<VulkanDevice> gated = openGatedDeviceOverTheRoot();
  if (!gated) {
    GTEST_SKIP() << kNoQueueGate;
  }

  // The scene is built before the gate closes: writeTexture submits and waits on its own fence,
  // which a gated queue would never let complete.
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*gated, scene));
  ASSERT_THAT(gated->waitForSerial(scene.serial, 30.0), IsTrue()) << gated->lastErrorForTest();

  tests::NativeQueueGate gate(gated->nativeContextForTest());
  ASSERT_NO_FATAL_FAILURE(gate.start());

  // A second copy of the scene, which the gate holds open, fills the buffer the mapping names.
  const Buffer held = gpu::tests::MakeReadbackBuffer(*gated);
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(gated->createCommandEncoder());
  ASSERT_THAT(encoder->copyTextureToBuffer(
                  TexelCopyTextureInfo{scene.texture}, held,
                  TexelCopyBufferLayout{0, gpu::tests::kMappingSceneBytesPerRow,
                                        gpu::tests::kMappingSceneExtent},
                  Extent2d{gpu::tests::kMappingSceneExtent, gpu::tests::kMappingSceneExtent}),
              IsOk());
  (void)GetResultOrFail(gated->submit(GetResultOrFail(encoder->finish())));
  BufferMapping mapping =
      GetResultOrFail(gated->mapBufferAsync(held, MapMode::Read, 0, kMappingSceneByteSize));
  // An error of the gated device's own would end the mapping the same way, so only the sibling's
  // declaration may be what ends it here.
  ASSERT_THAT(gated->lastErrorForTest(), IsEmpty());

  // Another device over the same root gives up on its own bounded wait.
  device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a sibling's queue drain gave up");

  // A budget far longer than one slice, so a wait that ran it out cannot pass as prompt.
  MapWaitReport report;
  const int64_t waitedMs = MillisecondsTaken([&] {
    report = GetResultOrFail(gated->waitForMapping(mapping, MapWaitParams{0.001, 5.0}, {}));
  });
  EXPECT_THAT(report.outcome, Eq(MapWaitOutcome::DeviceLost))
      << "a mapping over a lost root can never complete, so waiting out its budget would report a "
         "permanent failure as a slow one";
  EXPECT_THAT(waitedMs, Lt(1000)) << "the loss must end the wait at its first readiness check";
  EXPECT_THAT(gated->mappedBytes(mapping), IsGpuError(GpuErrorType::InvalidState))
      << "a mapping that never completed has no bytes to read";

  ASSERT_EQ(gate.release(), VK_SUCCESS);
  EXPECT_THAT(gated->unmapBuffer(std::move(mapping)), IsOk());
  EXPECT_THAT(gated->lastErrorForTest(), IsEmpty())
      << "the sibling's declaration must be the only thing that went wrong on this device";
}

TEST_F(VulkanDeviceLossTest, ADeviceLostSubmissionDeclaresTheRootLostAsABackendReport) {
  const std::unique_ptr<VulkanDevice> sibling = openDeviceOverTheRoot();
  ASSERT_THAT(sibling, NotNull());

  device_->failNextSubmissionForTest(/*deviceLost=*/true);
  EXPECT_THAT(SubmitEmptyCommandBuffer(*device_), IsGpuError(GpuErrorType::InvalidState))
      << "the submission the driver refused with VK_ERROR_DEVICE_LOST must fail";

  EXPECT_THAT(device_->isLost(), IsTrue())
      << "a device the driver reported lost leaves the root in an unknown state";
  EXPECT_THAT(sibling->isLost(), IsTrue()) << "the loss belongs to the root, not to one device";
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::None))
      << "the driver reported this loss; no wait gave up";
  EXPECT_THAT(device_->waitForSerial(device_->lastSubmittedSerial(), 1.0), IsFalse())
      << "the wait a Geode context's queue drain makes must fail on a lost device";

  // A queue drain that gives up afterwards, as a Geode context's does, is a consequence of the
  // loss and must not claim it.
  device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a later queue drain");
  EXPECT_THAT(rootLoss_->timedOutSite.load(), Eq(DeviceLostWaitSite::None));
}

TEST_F(VulkanDeviceLossTest, ADeclaredRootLossRefusesRepeatedSubmissionsBeforeFlight) {
  // Prepare valid work before the sibling's declaration, so encoder admission does not mask the
  // submission gate. A logical loss leaves the native Vulkan device usable by the driver.
  std::array<CommandBuffer, 3> prepared{
      GetResultOrFail(GetResultOrFail(device_->createCommandEncoder())->finish()),
      GetResultOrFail(GetResultOrFail(device_->createCommandEncoder())->finish()),
      GetResultOrFail(GetResultOrFail(device_->createCommandEncoder())->finish())};
  ASSERT_THAT(DeclareDeviceLost(*rootLoss_), IsTrue());
  for (CommandBuffer& buffer : prepared) {
    EXPECT_THAT(device_->submit(std::move(buffer)), IsGpuError(GpuErrorType::DeviceLost));
    EXPECT_THAT(device_->lastSubmittedSerial(), Eq(0u))
        << "work over the lost root must never be accepted into flight";
  }
  EXPECT_THAT(device_->bufferWriteStatsForTest().inFlightBytes, Eq(0u));
}

TEST_F(VulkanDeviceLossTest, ADriverReportedLossRefusesReadsThroughAReadyMappingAsALoss) {
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*device_, scene));
  BufferMapping mapping = GetResultOrFail(
      device_->mapBufferAsync(scene.readback, MapMode::Read, 0, kMappingSceneByteSize));
  ASSERT_THAT(GetResultOrFail(device_->waitForMapping(mapping, SceneWaitParams(), {})).outcome,
              Eq(MapWaitOutcome::Ready))
      << device_->lastErrorForTest();
  ASSERT_THAT(device_->mappedBytes(mapping), HasResult());

  device_->failNextSubmissionForTest(/*deviceLost=*/true);
  EXPECT_THAT(SubmitEmptyCommandBuffer(*device_), IsGpuError(GpuErrorType::InvalidState));

  EXPECT_THAT(device_->mappedBytes(mapping), IsGpuError(GpuErrorType::DeviceLost))
      << "a read after the driver reported the device lost is refused as a device loss, as "
         "mappedBytes documents, not as a generic invalid state";
  EXPECT_THAT(device_->unmapBuffer(std::move(mapping)), IsOk());
}

TEST_F(VulkanDeviceLossTest, ABusyBufferReadOverALostRootReportsTheLoss) {
  const std::unique_ptr<VulkanDevice> gated = openGatedDeviceOverTheRoot();
  if (!gated) {
    GTEST_SKIP() << kNoQueueGate;
  }

  // The scene is built before the gate closes: writeTexture submits and waits on its own fence,
  // which a gated queue would never let complete.
  MappingScene scene;
  ASSERT_NO_FATAL_FAILURE(gpu::tests::BuildMappingScene(*gated, scene));
  ASSERT_THAT(gated->waitForSerial(scene.serial, 30.0), IsTrue()) << gated->lastErrorForTest();

  tests::NativeQueueGate gate(gated->nativeContextForTest());
  ASSERT_NO_FATAL_FAILURE(gate.start());

  // A copy the gate holds open keeps the buffer busy, so reading it back has to wait for that copy.
  const Buffer busy = gpu::tests::MakeReadbackBuffer(*gated);
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(gated->createCommandEncoder());
  ASSERT_THAT(encoder->copyTextureToBuffer(
                  TexelCopyTextureInfo{scene.texture}, busy,
                  TexelCopyBufferLayout{0, gpu::tests::kMappingSceneBytesPerRow,
                                        gpu::tests::kMappingSceneExtent},
                  Extent2d{gpu::tests::kMappingSceneExtent, gpu::tests::kMappingSceneExtent}),
              IsOk());
  (void)GetResultOrFail(gated->submit(GetResultOrFail(encoder->finish())));
  ASSERT_THAT(gated->lastErrorForTest(), IsEmpty());

  device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a sibling's queue drain gave up");

  EXPECT_THAT(gated->readBackBuffer(busy),
              IsGpuErrorWithMessage(GpuErrorType::DeviceLost, HasSubstr("lost")))
      << "a read that cannot wait for its buffer's work because the root is lost must report the "
         "loss, not a timeout";

  ASSERT_EQ(gate.release(), VK_SUCCESS);
  EXPECT_THAT(gated->lastErrorForTest(), IsEmpty());
}

TEST_F(VulkanDeviceLossTest, AnUploadHeldOnTheQueueEndsWhenASiblingDeclaresTheRootLost) {
  const std::unique_ptr<VulkanDevice> gated = openGatedDeviceOverTheRoot();
  if (!gated) {
    GTEST_SKIP() << kNoQueueGate;
  }
  const Extent2d extent{gpu::tests::kMappingSceneExtent, gpu::tests::kMappingSceneExtent};
  const Texture texture = GetResultOrFail(gated->createTexture(
      TextureDescriptor{"heldUpload", extent, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));

  // The gate holds the queue, so the upload writeTexture submits and then waits for cannot run.
  tests::NativeQueueGate gate(gated->nativeContextForTest());
  ASSERT_NO_FATAL_FAILURE(gate.start());
  ASSERT_THAT(gated->lastErrorForTest(), IsEmpty());

  // The sibling gives up once the upload's fence wait is known to be blocked.
  int steps = 0;
  gated->setFenceWaitStepHookForTest([&] {
    if (++steps == 1) {
      device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                        "a sibling's queue drain gave up");
    }
  });
  Status written = OkStatus();
  const int64_t waitedMs = MillisecondsTaken([&] {
    written = gated->writeTexture(texture, gpu::tests::MappingSceneUpload(),
                                  TexelCopyBufferLayout{0, gpu::tests::kMappingSceneBytesPerRow,
                                                        gpu::tests::kMappingSceneExtent},
                                  extent);
  });
  gated->setFenceWaitStepHookForTest({});
  EXPECT_THAT(written, IsGpuErrorWithMessage(GpuErrorType::DeviceLost, HasSubstr("lost")))
      << "an upload that cannot finish because the root is lost must report the loss, not a "
         "timeout";
  EXPECT_THAT(steps, Eq(1))
      << "the upload's wait must end at the check after the step during which the loss was "
         "declared";
  EXPECT_THAT(waitedMs, Lt(1000)) << "the upload must not spend its whole wait on a lost root";

  ASSERT_EQ(gate.release(), VK_SUCCESS);
  EXPECT_THAT(gated->lastErrorForTest(), IsEmpty())
      << "the sibling's declaration must be the only thing that went wrong on this device";
}

TEST_F(VulkanDeviceLossTest, UploadsOverARootAlreadyDeclaredLostAreRefusedBeforeTheyStage) {
  const std::unique_ptr<VulkanDevice> gated = openGatedDeviceOverTheRoot();
  if (!gated) {
    GTEST_SKIP() << kNoQueueGate;
  }
  const Extent2d extent{gpu::tests::kMappingSceneExtent, gpu::tests::kMappingSceneExtent};
  const Texture texture = GetResultOrFail(gated->createTexture(TextureDescriptor{
      "lostRootUpload", extent, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));

  // The gate stands in for a hung root: an upload that reached its queue would never signal.
  tests::NativeQueueGate gate(gated->nativeContextForTest());
  ASSERT_NO_FATAL_FAILURE(gate.start());
  ASSERT_THAT(gated->lastErrorForTest(), IsEmpty());
  device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a sibling's queue drain gave up");

  int steps = 0;
  gated->setFenceWaitStepHookForTest([&] { ++steps; });
  // A caller that uploads every frame keeps calling after the loss.
  constexpr int kUploads = 8;
  for (int upload = 0; upload < kUploads; ++upload) {
    SCOPED_TRACE(testing::Message() << "upload " << upload);
    EXPECT_THAT(gated->writeTexture(texture, gpu::tests::MappingSceneUpload(),
                                    TexelCopyBufferLayout{0, gpu::tests::kMappingSceneBytesPerRow,
                                                          gpu::tests::kMappingSceneExtent},
                                    extent),
                IsGpuErrorWithMessage(GpuErrorType::DeviceLost, HasSubstr("lost")));
  }
  gated->setFenceWaitStepHookForTest({});
  EXPECT_THAT(gated->pendingTextureUploadCountForTest(), Eq(0u))
      << "an upload refused on a lost root must not leave objects waiting on a fence that will "
         "never signal";
  EXPECT_THAT(steps, Eq(0)) << "an upload on a lost root must not wait at all";

  ASSERT_EQ(gate.release(), VK_SUCCESS);
  EXPECT_THAT(gated->lastErrorForTest(), IsEmpty())
      << "the sibling's declaration must be the only thing that went wrong on this device";
}

TEST_F(VulkanDeviceLossTest, AWaitEndsAtOnceWhenASiblingHasDeclaredTheRootLost) {
  const std::unique_ptr<VulkanDevice> gated = openGatedDeviceOverTheRoot();
  if (!gated) {
    GTEST_SKIP() << kNoQueueGate;
  }
  tests::NativeQueueGate gate(gated->nativeContextForTest());
  ASSERT_NO_FATAL_FAILURE(gate.start());
  const uint64_t serial = SubmitEmptyWork(*gated);
  ASSERT_THAT(gated->lastErrorForTest(), IsEmpty());

  device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                    "a sibling's queue drain gave up");

  bool completed = true;
  const int64_t waitedMs =
      MillisecondsTaken([&] { completed = gated->waitForSerial(serial, 5.0); });
  EXPECT_THAT(completed, IsFalse());
  EXPECT_THAT(waitedMs, Lt(1000)) << "a wait on a lost root must not spend its budget";

  ASSERT_EQ(gate.release(), VK_SUCCESS);
  EXPECT_THAT(gated->lastErrorForTest(), IsEmpty());
}

TEST_F(VulkanDeviceLossTest, AWaitEndsWhenASiblingDeclaresTheRootLostWhileItWaits) {
  const std::unique_ptr<VulkanDevice> gated = openGatedDeviceOverTheRoot();
  if (!gated) {
    GTEST_SKIP() << kNoQueueGate;
  }
  tests::NativeQueueGate gate(gated->nativeContextForTest());
  ASSERT_NO_FATAL_FAILURE(gate.start());
  const uint64_t serial = SubmitEmptyWork(*gated);
  ASSERT_THAT(gated->lastErrorForTest(), IsEmpty());

  // The sibling gives up after the first step of this device's fence wait has timed out, so the
  // wait is known to be blocked when the loss is declared.
  int steps = 0;
  gated->setFenceWaitStepHookForTest([&] {
    if (++steps == 1) {
      device_->markLostAfterWaitTimeout(DeviceLostWaitSite::QueueIdle, std::chrono::milliseconds{5},
                                        "a sibling's queue drain gave up");
    }
  });
  bool completed = true;
  const int64_t waitedMs =
      MillisecondsTaken([&] { completed = gated->waitForSerial(serial, 5.0); });
  gated->setFenceWaitStepHookForTest({});
  EXPECT_THAT(completed, IsFalse());
  EXPECT_THAT(steps, Eq(1))
      << "the wait must end at the check after the step during which the loss was declared";
  EXPECT_THAT(waitedMs, Lt(1000))
      << "a wait must notice a loss declared after it started, not only one declared before";

  ASSERT_EQ(gate.release(), VK_SUCCESS);
  EXPECT_THAT(gated->lastErrorForTest(), IsEmpty());
}

}  // namespace
}  // namespace donner::gpu::vulkan
