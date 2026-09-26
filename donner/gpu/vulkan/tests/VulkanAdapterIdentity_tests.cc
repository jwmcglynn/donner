/// @file
/// The baseline gate names the physical Vulkan device selected by its shared root.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan::tests {
namespace {

TEST(VulkanAdapterIdentityTest, NameBelongsToSelectedSharedRoot) {
  const std::shared_ptr<VulkanSharedRoot> root = VulkanDevice::CreateSharedRoot();
  ASSERT_NE(root, nullptr) << "The Vulkan baseline lane needs a physical device";

  const std::string selectedName = root->adapterName();
  EXPECT_THAT(selectedName, testing::Not(testing::IsEmpty()));

  const std::unique_ptr<VulkanDevice> first = VulkanDevice::CreateOverSharedRoot(root);
  const std::unique_ptr<VulkanDevice> second = VulkanDevice::CreateOverSharedRoot(root);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_THAT(root->adapterName(), testing::Eq(selectedName));
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
