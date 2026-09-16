/// @file
/// UI draw data through the Vulkan backend: the shared scene renders exactly, proving the
/// compiled UI shader's packed-color unpacking, both alpha entry points, the device-pixel scissor
/// and the per-list indexed ranges execute as recorded.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string_view>

#include "donner/editor/tests/UiDrawScene.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan::tests {
namespace {

class VulkanUiRendererTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::Create();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available; the "
                  "UI rendering gate must not be skipped on this runner";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanUiRendererTest, SceneRendersExactly) {
  const auto readback = [this](const Buffer& buffer) { return device_->readBackBuffer(buffer); };
  editor::tests::CheckUiScene(*device_, readback, "vulkan_ui_draw_scene");
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
