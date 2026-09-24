/// @file
/// A second runtime device over one Vulkan root must be able to register the producer's image.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan {
namespace {

using testing::IsEmpty;
using testing::IsFalse;
using testing::NotNull;

TEST(VulkanTextureRegistration, ASecondRuntimeDeviceNamesTheProducerImage) {
  const std::shared_ptr<VulkanSharedRoot> root = VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(root, NotNull()) << "the native Vulkan registration gate needs a Vulkan device";
  std::unique_ptr<VulkanDevice> producer = VulkanDevice::CreateOverSharedRoot(root);
  std::unique_ptr<VulkanDevice> consumer = VulkanDevice::CreateOverSharedRoot(root);
  ASSERT_THAT(producer, NotNull());
  ASSERT_THAT(consumer, NotNull());

  const Texture source = GetResultOrFail(producer->createTexture(
      TextureDescriptor{"producer",
                        {4, 4},
                        TextureFormat::RGBA8Unorm,
                        TextureUsage::Sampled | TextureUsage::CopySrc | TextureUsage::CopyDst}));
  const Result<TextureExport> exported = producer->exportTexture(source);
  ASSERT_THAT(exported, HasResult()) << "a sibling over the same VkDevice needs an image export";

  const Result<Texture> registered = consumer->registerTexture(exported.result());
  ASSERT_THAT(registered, HasResult())
      << "the consumer should alias the producer image through an independent runtime handle";
  EXPECT_THAT(consumer->ownsTextureBacking(registered.result()), IsFalse())
      << "the registration must not become a second owner of the native image";
}

TEST(VulkanTextureRegistration, ATextureFromAnotherNativeRootIsRefused) {
  const std::shared_ptr<VulkanSharedRoot> producerRoot = VulkanDevice::CreateSharedRoot();
  const std::shared_ptr<VulkanSharedRoot> foreignRoot = VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(producerRoot, NotNull());
  ASSERT_THAT(foreignRoot, NotNull());
  std::unique_ptr<VulkanDevice> producer = VulkanDevice::CreateOverSharedRoot(producerRoot);
  std::unique_ptr<VulkanDevice> foreign = VulkanDevice::CreateOverSharedRoot(foreignRoot);
  ASSERT_THAT(producer, NotNull());
  ASSERT_THAT(foreign, NotNull());
  const Texture source = GetResultOrFail(producer->createTexture(
      TextureDescriptor{"foreign", {4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  const Result<TextureExport> exported = producer->exportTexture(source);
  ASSERT_THAT(exported, HasResult());

  EXPECT_THAT(foreign->registerTexture(exported.result()),
              IsGpuError(GpuErrorType::DeviceMismatch));
}

/// The consumer's registration and in-flight records retain the native allocation even after
/// the producer runtime device and the export token are gone.
TEST(VulkanTextureRegistration, ARegistrationOutlivesItsProducerAndReadsExactPixels) {
  const std::shared_ptr<VulkanSharedRoot> root = VulkanDevice::CreateSharedRoot();
  ASSERT_THAT(root, NotNull());
  std::unique_ptr<VulkanDevice> producer = VulkanDevice::CreateOverSharedRoot(root);
  std::unique_ptr<VulkanDevice> consumer = VulkanDevice::CreateOverSharedRoot(root);
  ASSERT_THAT(producer, NotNull());
  ASSERT_THAT(consumer, NotNull());

  constexpr Extent2d kSize{4, 4};
  constexpr uint64_t kReadbackBytes = uint64_t{kTexelRowPitchAlignment} * kSize.height;
  const Texture source = GetResultOrFail(producer->createTexture(
      TextureDescriptor{"retained", kSize, TextureFormat::RGBA8Unorm,
                        TextureUsage::Sampled | TextureUsage::CopyDst | TextureUsage::CopySrc}));
  std::array<uint8_t, kSize.width * 4> expectedRow{};
  for (uint32_t x = 0; x < kSize.width; ++x) {
    expectedRow[x * 4 + 1] = 255;
    expectedRow[x * 4 + 3] = 255;
  }
  std::vector<uint8_t> uploaded(kReadbackBytes);
  for (uint32_t y = 0; y < kSize.height; ++y) {
    std::copy(expectedRow.begin(), expectedRow.end(),
              uploaded.begin() + static_cast<std::ptrdiff_t>(y * kTexelRowPitchAlignment));
  }
  constexpr TexelCopyBufferLayout kLayout{0, kTexelRowPitchAlignment, kSize.height};
  ASSERT_THAT(producer->writeTexture(source, uploaded, kLayout, kSize), IsOk());

  const Texture registered = [&] {
    const TextureExport exported = GetResultOrFail(producer->exportTexture(source));
    return GetResultOrFail(consumer->registerTexture(exported));
  }();
  producer.reset();

  const Buffer readback = GetResultOrFail(consumer->createBuffer(
      BufferDescriptor{"readback", kReadbackBytes, BufferUsage::CopyDst | BufferUsage::MapRead}));
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(consumer->createCommandEncoder());
  ASSERT_THAT(
      encoder->copyTextureToBuffer(TexelCopyTextureInfo{registered}, readback, kLayout, kSize),
      IsOk());
  const uint64_t serial = GetResultOrFail(consumer->submit(GetResultOrFail(encoder->finish())));
  ASSERT_THAT(consumer->waitForSerial(serial, 5.0), testing::IsTrue());
  const Result<std::vector<uint8_t>> pixels = consumer->readBackBuffer(readback);
  ASSERT_THAT(pixels, HasResult());
  ASSERT_EQ(pixels.result().size(), uploaded.size());
  for (uint32_t y = 0; y < kSize.height; ++y) {
    SCOPED_TRACE(testing::Message() << "row " << y);
    const std::span<const uint8_t> row(pixels.result().data() + y * kTexelRowPitchAlignment,
                                       expectedRow.size());
    EXPECT_THAT(row, testing::ElementsAreArray(expectedRow));
  }
  EXPECT_THAT(consumer->lastErrorForTest(), IsEmpty());
}

}  // namespace
}  // namespace donner::gpu::vulkan
