/// @file
/// Device-level regressions for the resource-state wiring: the barriers and render pass
/// dependencies the backend actually emits, and what it leaves in the tracker when work fails.
///
/// The model's own suite covers the table. These cover the code that feeds it, which is where
/// both of the defects this file exists for lived. They need a real device, so unlike the model
/// suite they run only where a Vulkan loader exists.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/vulkan/VulkanDevice.h"
#include "donner/gpu/vulkan/VulkanResourceState.h"

namespace donner::gpu::vulkan::tests {
namespace {

constexpr uint32_t kExtent = 4;
/// Buffer-image copy rows are 256-byte aligned, as every other upload in this suite is; the
/// upload buffer is padded to that pitch rather than packed.
constexpr uint32_t kBytesPerRow = 256;

class VulkanResourceStateDeviceTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::Create();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  /// A texture of the standard test extent with \p usage.
  Texture makeTexture(const char* label, TextureUsage usage) {
    return unwrap(device_->createTexture(TextureDescriptor{label, Extent2d{kExtent, kExtent},
                                                           TextureFormat::RGBA8Unorm, usage}),
                  "createTexture");
  }

  /// Bytes for one full upload of the standard extent.
  static std::vector<uint8_t> uploadBytes() {
    return std::vector<uint8_t>(size_t{kBytesPerRow} * kExtent, 0x40);
  }

  static TexelCopyBufferLayout uploadLayout() {
    return TexelCopyBufferLayout{0, kBytesPerRow, kExtent};
  }

  /// Barriers recorded against \p slot since the device was created.
  std::vector<VulkanDevice::RecordedImageBarrierForTest> barriersFor(uint32_t slot) const {
    std::vector<VulkanDevice::RecordedImageBarrierForTest> matching;
    for (const VulkanDevice::RecordedImageBarrierForTest& barrier :
         device_->recordedImageBarriersForTest()) {
      if (barrier.textureSlot == slot) {
        matching.push_back(barrier);
      }
    }
    return matching;
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanResourceStateDeviceTest, AFailedUploadLeavesNoStateForALaterSubmitToPromote) {
  // Not Sampled, so a completed upload would leave this texture in the transfer-destination
  // layout and a failed one must leave it untouched - a difference the tracker can show.
  Texture written = makeTexture("written", TextureUsage::CopyDst | TextureUsage::CopySrc);
  Texture other = makeTexture("other", TextureUsage::CopyDst | TextureUsage::CopySrc);

  ASSERT_EQ(unwrap(device_->trackedTextureLayoutForTest(written), "layout before"),
            VulkanDevice::TrackedTextureLayout::Undefined);

  device_->failNextTextureUploadForTest(VulkanDevice::UploadFailureModeForTest::BeforeSubmit);
  EXPECT_TRUE(
      device_->writeTexture(written, uploadBytes(), uploadLayout(), Extent2d{kExtent, kExtent})
          .hasError())
      << "the seam must make this upload fail";

  // An unrelated, successful submission. Before the fix its commit promoted whatever the failed
  // upload had staged, because both share one tracker.
  Status otherUpload =
      device_->writeTexture(other, uploadBytes(), uploadLayout(), Extent2d{kExtent, kExtent});
  ASSERT_FALSE(otherUpload.hasError()) << otherUpload.error();

  EXPECT_EQ(unwrap(device_->trackedTextureLayoutForTest(written), "layout after"),
            VulkanDevice::TrackedTextureLayout::Undefined)
      << "the failed upload never ran, so nothing it staged may become the texture's real state";
}

TEST_F(VulkanResourceStateDeviceTest, AnUploadTheQueueTookKeepsItsStateWhenTheWaitTimesOut) {
  // The mirror of the test above: here the submission reaches the queue and only the wait for it
  // fails, so the upload still runs and the image really does end up in its post-upload layout.
  // Discarding then would leave the tracker describing a state the image is not in.
  Texture written = makeTexture("written", TextureUsage::CopyDst | TextureUsage::CopySrc);
  Texture destination = makeTexture("destination", TextureUsage::CopyDst | TextureUsage::CopySrc);

  device_->failNextTextureUploadForTest(VulkanDevice::UploadFailureModeForTest::AfterSubmit);
  EXPECT_TRUE(
      device_->writeTexture(written, uploadBytes(), uploadLayout(), Extent2d{kExtent, kExtent})
          .hasError())
      << "the seam must report the wait over an accepted submission as having timed out";

  const size_t before = barriersFor(written.slotIndex()).size();

  // A later submission reading that image. The queue is in order, so this meets the layout the
  // upload left behind, and its barrier has to be derived from that.
  std::unique_ptr<CommandEncoder> encoder =
      unwrap(device_->createCommandEncoder(), "createCommandEncoder");
  ASSERT_FALSE(encoder
                   ->copyTextureToTexture(written, destination, Extent2d{kExtent, kExtent},
                                          Origin2d{0, 0}, Origin2d{0, 0})
                   .hasError());
  Result<CommandBuffer> commands = encoder->finish();
  ASSERT_FALSE(commands.hasError()) << commands.error();
  Result<uint64_t> serial = device_->submit(std::move(commands).result());
  ASSERT_FALSE(serial.hasError()) << serial.error();
  ASSERT_TRUE(device_->waitForSerial(serial.result(), /*timeoutSeconds=*/30.0));

  const std::vector<VulkanDevice::RecordedImageBarrierForTest> barriers =
      barriersFor(written.slotIndex());
  ASSERT_GT(barriers.size(), before);
  const VulkanDevice::RecordedImageBarrierForTest& next = barriers.back();
  EXPECT_EQ(next.oldLayout, int32_t{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL})
      << "the queued upload left the image in its post-upload layout, not an undefined one";
  EXPECT_TRUE((next.srcAccess & VK_ACCESS_TRANSFER_WRITE_BIT) != 0)
      << "the upload's write still has to be made available to the copy that reads it";
  EXPECT_EQ(next.srcStage, uint32_t{VK_PIPELINE_STAGE_TRANSFER_BIT});
}

TEST_F(VulkanResourceStateDeviceTest, ThePassEntryEdgeCoversAnAttachmentThatIsNotTheLast) {
  // One attachment carries a prior write, the other is untouched, and the written one is not
  // last: deriving the edge from a single attachment picks up the untouched one and orders
  // nothing. A transfer stands in for the producing write here; which stage and access a given
  // producer contributes is what the model suite covers.
  Texture producedFirst =
      makeTexture("producedFirst", TextureUsage::RenderAttachment | TextureUsage::CopyDst);
  Texture freshSecond = makeTexture("freshSecond", TextureUsage::RenderAttachment);

  Status producedFirstUpload = device_->writeTexture(producedFirst, uploadBytes(), uploadLayout(),
                                                     Extent2d{kExtent, kExtent});
  ASSERT_FALSE(producedFirstUpload.hasError()) << producedFirstUpload.error();

  TextureView writtenView =
      unwrap(device_->createTextureView(producedFirst, TextureViewDescriptor{"writtenView"}),
             "createTextureView written");
  TextureView freshView =
      unwrap(device_->createTextureView(freshSecond, TextureViewDescriptor{"freshView"}),
             "createTextureView fresh");

  std::unique_ptr<CommandEncoder> encoder =
      unwrap(device_->createCommandEncoder(), "createCommandEncoder");
  RenderPassDescriptor pass;
  pass.colorAttachments.push_back(
      RenderPassColorAttachment{writtenView, LoadOp::Load, StoreOp::Store, {0, 0, 0, 0}});
  pass.colorAttachments.push_back(
      RenderPassColorAttachment{freshView, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}});
  Result<RenderPassEncoder*> renderPass = encoder->beginRenderPass(pass);
  ASSERT_FALSE(renderPass.hasError()) << renderPass.error();
  ASSERT_FALSE(renderPass.result()->end().hasError());

  Result<CommandBuffer> commands = encoder->finish();
  ASSERT_FALSE(commands.hasError()) << commands.error();
  Result<uint64_t> serial = device_->submit(std::move(commands).result());
  ASSERT_FALSE(serial.hasError()) << serial.error();
  ASSERT_TRUE(device_->waitForSerial(serial.result(), /*timeoutSeconds=*/30.0));

  const VulkanDevice::RecordedSubpassDependencyForTest entry =
      unwrap(device_->lastRenderPassEntryDependencyForTest(), "entry dependency");
  EXPECT_TRUE((entry.srcStage & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0)
      << "the load of the written attachment must be ordered after the write that produced it";
  EXPECT_TRUE((entry.srcAccess & VK_ACCESS_TRANSFER_WRITE_BIT) != 0)
      << "that write must be made available to the load";
  EXPECT_EQ(entry.dstStage, uint32_t{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT});
}

TEST_F(VulkanResourceStateDeviceTest, WritingOneTextureTwiceInALayoutThatDoesNotChangeStillWaits) {
  // Both writes leave the destination in the transfer-destination layout, so a layout-only
  // comparison would emit nothing between them. The same shape as two dispatches writing one
  // storage texture, which the model suite covers at the compute stage.
  Texture source = makeTexture("source", TextureUsage::CopySrc | TextureUsage::CopyDst);
  Texture destination = makeTexture("destination", TextureUsage::CopyDst | TextureUsage::CopySrc);
  Status sourceUpload =
      device_->writeTexture(source, uploadBytes(), uploadLayout(), Extent2d{kExtent, kExtent});
  ASSERT_FALSE(sourceUpload.hasError()) << sourceUpload.error();

  const size_t before = barriersFor(destination.slotIndex()).size();

  std::unique_ptr<CommandEncoder> encoder =
      unwrap(device_->createCommandEncoder(), "createCommandEncoder");
  for (int copy = 0; copy < 2; ++copy) {
    ASSERT_FALSE(encoder
                     ->copyTextureToTexture(source, destination, Extent2d{kExtent, kExtent},
                                            Origin2d{0, 0}, Origin2d{0, 0})
                     .hasError());
  }
  Result<CommandBuffer> commands = encoder->finish();
  ASSERT_FALSE(commands.hasError()) << commands.error();
  Result<uint64_t> serial = device_->submit(std::move(commands).result());
  ASSERT_FALSE(serial.hasError()) << serial.error();
  ASSERT_TRUE(device_->waitForSerial(serial.result(), /*timeoutSeconds=*/30.0));

  const std::vector<VulkanDevice::RecordedImageBarrierForTest> destinationBarriers =
      barriersFor(destination.slotIndex());
  ASSERT_GE(destinationBarriers.size(), before + 2)
      << "the second write into the same layout still needs its own barrier";

  const VulkanDevice::RecordedImageBarrierForTest& second = destinationBarriers.back();
  EXPECT_EQ(second.oldLayout, second.newLayout) << "the layout did not change";
  EXPECT_TRUE((second.srcAccess & VK_ACCESS_TRANSFER_WRITE_BIT) != 0)
      << "the earlier write must be made available to the later one";
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
