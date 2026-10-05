/// @file
/// Progress of the one queue every runtime device over a Vulkan root submits to, and the waits
/// that judge a stall by it: a device's wait sees a sibling's work ahead of it on that queue
/// finish, one command buffer at a time, and a stall anywhere ahead of it ends the wait within
/// the bound of the queue's last progress.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/DeviceLost.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan {
namespace {

using testing::_;
using testing::Eq;
using testing::Ge;
using testing::Gt;
using testing::Lt;
using testing::Optional;

/// The stall bound the waits take, and how often the held work finishes a command buffer. A
/// release three times slower than intended still keeps the queue progressing within the bound.
constexpr std::chrono::milliseconds kStallBound{500};
constexpr std::chrono::milliseconds kReleaseInterval{100};
/// Command buffers in the sibling's held submission.
constexpr uint64_t kHeldCommandBuffers = 12;

/// Submits \p count empty command buffers on \p device as one submission. @return Its serial.
/// @param device Device to submit on. @param count Command buffers in the submission.
uint64_t SubmitEmptyCommandBuffers(VulkanDevice& device, uint64_t count) {
  std::vector<CommandBuffer> commandBuffers;
  for (uint64_t i = 0; i < count; ++i) {
    commandBuffers.push_back(
        GetResultOrFail(GetResultOrFail(device.createCommandEncoder())->finish()));
  }
  return GetResultOrFail(device.submit(commandBuffers));
}

/// Two runtime devices over one root whose queue a test can hold one command buffer at a time.
class VulkanQueueProgressTest : public testing::Test {
protected:
  void SetUp() override {
    root_ = VulkanDevice::CreateSharedRootWithTimelineSemaphoreForTest(rootLoss_);
    if (root_ != nullptr) {
      sibling_ = VulkanDevice::CreateOverSharedRoot(root_);
      device_ = VulkanDevice::CreateOverSharedRoot(root_);
    }
    if (!sibling_ || !device_) {
      if (std::unique_ptr<VulkanDevice> plain = VulkanDevice::Create(); plain != nullptr) {
        GTEST_SKIP() << "Device lacks VK_KHR_timeline_semaphore; holding the queue needs it";
      }
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  void TearDown() override {
    if (sibling_) {
      sibling_->resumeSubmissionsForTest();
    }
  }

  /// Lets the sibling's held command buffers run one at a time, \ref kReleaseInterval apart, from
  /// a thread of their own: \p releases of them. @param releases How many to let run.
  PacedSteps releaseSibling(int releases) {
    return PacedSteps(releases, kReleaseInterval, [this](int released) {
      sibling_->releasePausedCommandBuffersForTest(static_cast<uint64_t>(released));
    });
  }

  std::shared_ptr<DeviceLostState> rootLoss_ = std::make_shared<DeviceLostState>();
  std::shared_ptr<VulkanSharedRoot> root_;
  std::unique_ptr<VulkanDevice> sibling_;  //!< Submits first, so its work runs ahead.
  std::unique_ptr<VulkanDevice> device_;   //!< Waits for work queued behind the sibling's.
};

/// A device's submission completes only after a sibling's work queued ahead of it on the shared
/// queue. While that work keeps finishing command buffers, the device's wait sees the queue
/// progress and waits it out, although its own work completes nothing for far longer than the
/// bound.
TEST_F(VulkanQueueProgressTest, AWaitSeesASiblingsWorkAheadOfItFinish) {
  ASSERT_THAT(sibling_->pauseSubmissionsForTest(), IsOk());
  (void)SubmitEmptyCommandBuffers(*sibling_, kHeldCommandBuffers);
  const uint64_t serial = SubmitEmptyCommandBuffers(*device_, 1);

  PacedSteps release = releaseSibling(static_cast<int>(kHeldCommandBuffers));
  const SerialWaitResult waited = device_->waitForSerialUnlessStalled(serial, kStallBound);
  release.join();
  ASSERT_THAT(release, KeptPaceWithin(kStallBound));

  EXPECT_THAT(waited.end, Eq(SerialWaitEnd::Completed))
      << "the sibling's work ahead kept finishing, yet the wait gave up: " << waited;
  EXPECT_THAT(waited.waited.count(), Gt(kStallBound.count()))
      << "the work ahead finished within the bound, so this case measured nothing";
  EXPECT_FALSE(device_->isLost());
}

/// Work ahead on the shared queue that stops finishing command buffers holds the device's work
/// too: its wait gives up within the bound of the queue's last progress, and not before.
TEST_F(VulkanQueueProgressTest, AWaitBehindASiblingThatStopsEndsWithinTheBound) {
  constexpr uint64_t kReleased = 3;
  ASSERT_THAT(sibling_->pauseSubmissionsForTest(), IsOk());
  (void)SubmitEmptyCommandBuffers(*sibling_, kHeldCommandBuffers);
  const uint64_t serial = SubmitEmptyCommandBuffers(*device_, 1);

  PacedSteps release = releaseSibling(static_cast<int>(kReleased));
  const SerialWaitResult waited = device_->waitForSerialUnlessStalled(serial, kStallBound);
  const auto endedAt = std::chrono::steady_clock::now();
  release.join();
  ASSERT_THAT(release, KeptPaceWithin(kStallBound));

  // The wait cannot have seen the last progress before its release began, so the lower bound is
  // measured from there, and the upper bound from when the release returned.
  EXPECT_THAT(waited.end, Eq(SerialWaitEnd::Stalled)) << waited;
  EXPECT_THAT(MillisecondsBetween(release.lastStepStart(), endedAt), Ge(kStallBound.count()))
      << "the wait gave up while the work ahead of it was still finishing command buffers";
  EXPECT_THAT(MillisecondsBetween(release.lastStepEnd(), endedAt), Lt(kStallBound.count() + 1000))
      << "the wait outlived the bound after the queue stopped progressing";
  EXPECT_FALSE(device_->isLost()) << "the serial wait declared a loss itself";
}

/// Held work is not progress, and each command buffer that finishes is, whichever device over the
/// root submitted it.
TEST_F(VulkanQueueProgressTest, LastProgressFollowsCommandBuffersFinishing) {
  ASSERT_THAT(sibling_->pauseSubmissionsForTest(), IsOk());
  const uint64_t held = SubmitEmptyCommandBuffers(*sibling_, 2);
  const std::optional<std::chrono::steady_clock::time_point> submittedAt = device_->lastProgress();
  ASSERT_THAT(submittedAt, Optional(_)) << "a native device reports when its queue progressed";

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::optional<std::chrono::steady_clock::time_point> whileHeld = device_->lastProgress();
  ASSERT_THAT(whileHeld, Optional(_));
  EXPECT_THAT(MillisecondsBetween(*submittedAt, *whileHeld), Eq(0))
      << "the queue reported progress while all of its work was held";

  sibling_->releasePausedCommandBuffersForTest(1);
  // The first command buffer finishes; the second stays held, so the submission stays incomplete.
  ASSERT_THAT(sibling_->waitForSerialUnlessStalled(held, std::chrono::milliseconds(200)).end,
              Eq(SerialWaitEnd::Stalled));
  const std::optional<std::chrono::steady_clock::time_point> afterOne = device_->lastProgress();
  ASSERT_THAT(afterOne, Optional(_));
  EXPECT_THAT(MillisecondsBetween(*submittedAt, *afterOne), Gt(0))
      << "a sibling's command buffer finished, yet the queue reported no progress";
}

/// Work that sets no progress event, such as a texture upload, still starts the clock on a queue
/// with nothing tracked outstanding. Otherwise a wait for it is judged by progress from before it
/// was submitted, and on a root that sat idle for longer than the wait's bound it is stalled at
/// once.
TEST_F(VulkanQueueProgressTest, AnUntrackedSubmissionToAnIdleQueueStartsTheClock) {
  constexpr uint32_t kExtent = 4;
  const Texture texture = GetResultOrFail(device_->createTexture(TextureDescriptor{
      "upload", Extent2d{kExtent, kExtent}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  const uint64_t tracked = SubmitEmptyCommandBuffers(*device_, 1);
  ASSERT_THAT(device_->waitForSerialUnlessStalled(tracked, kStallBound).end,
              Eq(SerialWaitEnd::Completed));

  // The queue is idle, and its last progress is the tracked work finishing.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::chrono::steady_clock::time_point beforeUpload = std::chrono::steady_clock::now();
  // Rows of a texel copy are 256-byte aligned, however narrow the texture.
  const std::vector<uint8_t> texels(size_t{kTexelRowPitchAlignment} * kExtent, 0x40);
  ASSERT_THAT(device_->writeTexture(texture, texels,
                                    TexelCopyBufferLayout{0, kTexelRowPitchAlignment, kExtent},
                                    Extent2d{kExtent, kExtent}),
              IsOk());

  const std::optional<std::chrono::steady_clock::time_point> progressAt = device_->lastProgress();
  ASSERT_THAT(progressAt, Optional(_));
  EXPECT_THAT(*progressAt, Ge(beforeUpload))
      << "after an upload to an idle queue, its last progress is still "
      << MillisecondsBetween(*progressAt, beforeUpload) << " ms before the upload was submitted";
}

}  // namespace
}  // namespace donner::gpu::vulkan
