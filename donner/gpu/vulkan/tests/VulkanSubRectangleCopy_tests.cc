/// @file
/// The Vulkan texture-to-texture copy slice: copies a sub-rectangle between two textures through
/// donner::gpu::vulkan::VulkanDevice and compares the destination texels byte-for-byte against
/// the shared expected image.

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
#include "donner/gpu/tests/SubRectangleCopyScene.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan::tests {
namespace {

using gpu::tests::kSubRectCopyBytesPerRow;
using gpu::tests::kSubRectCopyDestinationX;
using gpu::tests::kSubRectCopyDestinationY;
using gpu::tests::kSubRectCopyExtent;
using gpu::tests::kSubRectCopyHeight;
using gpu::tests::kSubRectCopySourceX;
using gpu::tests::kSubRectCopySourceY;
using gpu::tests::kSubRectCopyWidth;
using gpu::tests::SubRectCopyDestinationUpload;
using gpu::tests::SubRectCopyExpectedTexel;
using gpu::tests::SubRectCopySourceUpload;

class VulkanSubRectangleCopyTest : public testing::Test {
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

  /// Unwraps an RHI result, failing the test on error.
  template <typename T>
  T unwrap(Result<T>&& result, const char* what) {
    if (result.hasError()) {
      ADD_FAILURE() << what << " failed: " << result.error();
    }
    return std::move(result).result();
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanSubRectangleCopyTest, CopiesTheRectangleBetweenTheTwoOrigins) {
  const Extent2d extent{kSubRectCopyExtent, kSubRectCopyExtent};
  const TexelCopyBufferLayout layout{0, kSubRectCopyBytesPerRow, kSubRectCopyExtent};

  Texture source = unwrap(
      device_->createTexture(TextureDescriptor{"source", extent, TextureFormat::RGBA8Unorm,
                                               TextureUsage::CopySrc | TextureUsage::CopyDst}),
      "createTexture source");
  Texture destination = unwrap(
      device_->createTexture(TextureDescriptor{"destination", extent, TextureFormat::RGBA8Unorm,
                                               TextureUsage::CopyDst | TextureUsage::CopySrc}),
      "createTexture destination");
  Buffer readback = unwrap(device_->createBuffer(BufferDescriptor{
                               "readback", uint64_t{kSubRectCopyBytesPerRow} * kSubRectCopyExtent,
                               BufferUsage::CopyDst | BufferUsage::MapRead}),
                           "createBuffer readback");

  ASSERT_FALSE(device_->writeTexture(source, SubRectCopySourceUpload(), layout, extent).hasError());
  ASSERT_FALSE(device_->writeTexture(destination, SubRectCopyDestinationUpload(), layout, extent)
                   .hasError());

  std::unique_ptr<CommandEncoder> encoder =
      unwrap(device_->createCommandEncoder(), "createCommandEncoder");
  ASSERT_FALSE(encoder
                   ->copyTextureToTexture(
                       source, destination, Extent2d{kSubRectCopyWidth, kSubRectCopyHeight},
                       Origin2d{kSubRectCopySourceX, kSubRectCopySourceY},
                       Origin2d{kSubRectCopyDestinationX, kSubRectCopyDestinationY})
                   .hasError());
  ASSERT_FALSE(
      encoder->copyTextureToBuffer(TexelCopyTextureInfo{destination}, readback, layout, extent)
          .hasError());

  Result<CommandBuffer> commands = encoder->finish();
  ASSERT_FALSE(commands.hasError()) << commands.error();
  Result<uint64_t> serial = device_->submit(std::move(commands).result());
  ASSERT_FALSE(serial.hasError()) << serial.error();

  ASSERT_TRUE(device_->waitForSerial(serial.result(), /*timeoutSeconds=*/30.0))
      << "Command buffer did not complete cleanly";

  Result<std::vector<uint8_t>> pixels = device_->readBackBuffer(readback);
  ASSERT_FALSE(pixels.hasError()) << pixels.error();

  for (uint32_t y = 0; y < kSubRectCopyExtent; ++y) {
    for (uint32_t x = 0; x < kSubRectCopyExtent; ++x) {
      const size_t offset = size_t{y} * kSubRectCopyBytesPerRow + size_t{x} * 4u;
      const std::array<uint8_t, 4> actual = {
          pixels.result()[offset + 0], pixels.result()[offset + 1], pixels.result()[offset + 2],
          pixels.result()[offset + 3]};
      EXPECT_THAT(actual, testing::ElementsAreArray(SubRectCopyExpectedTexel(x, y)))
          << "texel (" << x << ", " << y << ")";
    }
  }
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
