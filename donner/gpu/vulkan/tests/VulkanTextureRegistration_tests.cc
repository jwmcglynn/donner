/// @file
/// A second runtime device over one Vulkan root must be able to register the producer's image.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan {
namespace {

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

}  // namespace
}  // namespace donner::gpu::vulkan
