/// @file
/// The Vulkan writeTexture destination-origin slice: uploads a sub-rectangle at a nonzero origin
/// through donner::gpu::vulkan::VulkanDevice and compares the whole destination byte-for-byte
/// against the shared expected image, so both the written texels and the untouched ones around
/// them are checked.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/SubRectangleUploadScene.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan::tests {
namespace {

using gpu::tests::kSubRectUploadBytesPerRow;
using gpu::tests::kSubRectUploadExtent;
using gpu::tests::kSubRectUploadHeight;
using gpu::tests::kSubRectUploadWidth;
using gpu::tests::kSubRectUploadX;
using gpu::tests::kSubRectUploadY;
using gpu::tests::SubRectUploadBytes;
using gpu::tests::SubRectUploadDestinationFillBytes;
using gpu::tests::SubRectUploadExpectedTexel;

/// Extent of the destination texture.
constexpr Extent2d kDestinationExtent{kSubRectUploadExtent, kSubRectUploadExtent};

/// Layout of the whole-texture fill and of the readback.
constexpr TexelCopyBufferLayout kFullLayout{0, kSubRectUploadBytesPerRow, kSubRectUploadExtent};

/// Layout of the sub-rectangle payload.
constexpr TexelCopyBufferLayout kUploadLayout{0, kSubRectUploadBytesPerRow, kSubRectUploadHeight};

class VulkanSubRectangleUploadTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::Create();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available; the "
                  "vertical-slice gate must not be skipped on this runner";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanSubRectangleUploadTest, WritesOnlyTheRectangleAtTheDestinationOrigin) {
  const Texture destination = GetResultOrFail(device_->createTexture(
      TextureDescriptor{"destination", kDestinationExtent, TextureFormat::RGBA8Unorm,
                        TextureUsage::CopyDst | TextureUsage::CopySrc}));
  const Buffer readback = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"readback", uint64_t{kSubRectUploadBytesPerRow} * kSubRectUploadExtent,
                       BufferUsage::CopyDst | BufferUsage::MapRead}));

  ASSERT_THAT(device_->writeTexture(destination, SubRectUploadDestinationFillBytes(), kFullLayout,
                                    kDestinationExtent),
              IsOk());
  ASSERT_THAT(device_->writeTexture(destination, SubRectUploadBytes(), kUploadLayout,
                                    Extent2d{kSubRectUploadWidth, kSubRectUploadHeight},
                                    Origin2d{kSubRectUploadX, kSubRectUploadY}),
              IsOk());

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
  ASSERT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{destination}, readback, kFullLayout,
                                           kDestinationExtent),
              IsOk());
  const uint64_t serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
  ASSERT_TRUE(device_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "Command buffer did not complete cleanly";

  const Result<std::vector<uint8_t>> pixels = device_->readBackBuffer(readback);
  ASSERT_THAT(pixels, HasResult());
  for (uint32_t y = 0; y < kSubRectUploadExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectUploadExtent; ++x) {
      const size_t offset = size_t{y} * kSubRectUploadBytesPerRow + size_t{x} * 4u;
      const std::array<uint8_t, 4> actual = {
          pixels.result()[offset + 0], pixels.result()[offset + 1], pixels.result()[offset + 2],
          pixels.result()[offset + 3]};
      EXPECT_THAT(actual, testing::ElementsAreArray(SubRectUploadExpectedTexel(x, y)))
          << "texel (" << x << ", " << y << ")";
    }
  }
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
