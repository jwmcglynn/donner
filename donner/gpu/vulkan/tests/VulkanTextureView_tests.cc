/// @file
/// Texture views on Vulkan: a native image view exists only for a texture that can be sampled,
/// stored to or rendered to, because Vulkan refuses a view of an image with none of those usages.
/// A view of any other texture is still a valid runtime handle, and binding it stays refused.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string_view>

#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan {
namespace {

using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsFalse;
using testing::IsTrue;

class VulkanTextureViewTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::Create();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available; the "
                  "texture view gate must not be skipped on this runner";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  void TearDown() override {
    if (device_) {
      // Under the Khronos validation layer an invalid native call latches here.
      EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
    }
  }

  /// Creates a 4x4 RGBA8 texture with \p usage. @param usage Usage to create it with.
  Texture makeTexture(TextureUsage usage) {
    return GetResultOrFail(device_->createTexture(
        TextureDescriptor{"view", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, usage}));
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanTextureViewTest, AViewOfACopyOnlyTextureHasNoNativeView) {
  const Texture texture = makeTexture(TextureUsage::CopySrc | TextureUsage::CopyDst);
  const TextureView view =
      GetResultOrFail(device_->createTextureView(texture, TextureViewDescriptor{"copyOnly"}));

  EXPECT_THAT(GetResultOrFail(device_->hasNativeViewForTest(view)), IsFalse())
      << "Vulkan accepts an image view only of an image with a sampled, storage or attachment "
         "usage (VUID-VkImageViewCreateInfo-image-04441)";
}

TEST_F(VulkanTextureViewTest, AViewOfATextureThatCanBeViewedHasANativeView) {
  for (const TextureUsage usage :
       {TextureUsage::Sampled, TextureUsage::StorageBinding, TextureUsage::RenderAttachment,
        TextureUsage::Sampled | TextureUsage::CopySrc}) {
    SCOPED_TRACE(testing::Message() << "usage " << usage);
    const Texture texture = makeTexture(usage);
    const TextureView view =
        GetResultOrFail(device_->createTextureView(texture, TextureViewDescriptor{"viewable"}));
    EXPECT_THAT(GetResultOrFail(device_->hasNativeViewForTest(view)), IsTrue());
  }
}

TEST_F(VulkanTextureViewTest, AViewOfACopyOnlyTextureLeavesTheDeviceUsable) {
  const Texture copyOnly = makeTexture(TextureUsage::CopySrc);
  const TextureView copyOnlyView =
      GetResultOrFail(device_->createTextureView(copyOnly, TextureViewDescriptor{"copyOnly"}));
  ASSERT_THAT(device_->hasNativeViewForTest(copyOnlyView), HasResult())
      << "the view is a live handle of this device";

  // Under the Khronos validation layer a native view of the copy-only image is an error that
  // latches the device, so the work after it would fail.
  const Texture sampled = makeTexture(TextureUsage::Sampled);
  EXPECT_THAT(device_->createTextureView(sampled, TextureViewDescriptor{"after"}), HasResult());
  EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
}

TEST_F(VulkanTextureViewTest, BindingAViewOfACopyOnlyTextureIsRefused) {
  const BindGroupLayout layout =
      GetResultOrFail(device_->createBindGroupLayout(BindGroupLayoutDescriptor{
          "sampled",
          {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat}}}));
  const Texture texture = makeTexture(TextureUsage::CopySrc | TextureUsage::CopyDst);
  const TextureView view =
      GetResultOrFail(device_->createTextureView(texture, TextureViewDescriptor{"copyOnly"}));

  EXPECT_THAT(device_->createBindGroup(BindGroupDescriptor{
                  "group", layout, {BindGroupEntry{0, TextureViewBinding{view}}}}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("Sampled usage")))
      << "a view with no native image view must never reach a descriptor";
}

}  // namespace
}  // namespace donner::gpu::vulkan
