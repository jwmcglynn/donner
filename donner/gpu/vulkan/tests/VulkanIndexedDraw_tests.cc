/// @file
/// Indexed draws through the Vulkan backend: the shared indexed scene renders exactly, and an
/// index range the encoder rejects never reaches the device.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/SpirvEmitter.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/VertexInputSlice.h"
#include "donner/gpu/vulkan/VulkanDevice.h"

namespace donner::gpu::vulkan::tests {
namespace {

using testing::HasSubstr;
using testing::IsEmpty;
using testing::IsTrue;

class VulkanIndexedDrawTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = VulkanDevice::Create();
    if (!device_) {
      // CI sets DONNER_REQUIRE_VULKAN=1 (see BUILD.bazel) so a missing driver is a red test
      // instead of a silent skip; local runs without a Vulkan runtime still skip.
      const char* requireVulkan = std::getenv("DONNER_REQUIRE_VULKAN");
      if (requireVulkan != nullptr && std::string_view(requireVulkan) == "1") {
        FAIL() << "DONNER_REQUIRE_VULKAN=1 is set but no Vulkan 1.1 device is available; the "
                  "indexed-draw gate must not be skipped on this runner";
      }
      GTEST_SKIP() << "No Vulkan 1.1 device available";
    }
  }

  /// Emits BuildVertexInputModule as SPIR-V.
  ShaderModuleDescriptor vertexInputShader() {
    const auto module = gpu::tests::BuildVertexInputModule();
    EXPECT_FALSE(module.hasError()) << module.error();
    const auto emitted = shader::EmitSpirv(module.result());
    EXPECT_FALSE(emitted.hasError()) << emitted.error();
    return ShaderModuleDescriptor{"attributes", {}, ShaderSourceKind::Spirv, emitted.result()};
  }

  std::unique_ptr<VulkanDevice> device_;
};

TEST_F(VulkanIndexedDrawTest, IndexedQuadsWithOffsetsInstancingAndScissorMatchTheExpectedImage) {
  gpu::tests::CheckIndexedDrawScene(*device_, vertexInputShader(), [this](const Buffer& buffer) {
    return device_->readBackBuffer(buffer);
  });
  EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
}

TEST_F(VulkanIndexedDrawTest, SixteenBitIndicesAlwaysCarryTheirFullRange) {
  EXPECT_THAT(device_->supportsFullIndexRange(IndexFormat::Uint16), IsTrue());
}

TEST_F(VulkanIndexedDrawTest, AnIndexRangePastTheBoundBufferNeverReachesTheDevice) {
  const ShaderModule shaderModule =
      GetResultOrFail(device_->createShaderModule(vertexInputShader()));
  const PipelineLayout layout =
      GetResultOrFail(device_->createPipelineLayout(PipelineLayoutDescriptor{"attributes", {}}));
  const RenderPipeline pipeline = GetResultOrFail(device_->createRenderPipeline(
      gpu::tests::VertexInputPipelineDescriptor(shaderModule, layout)));
  const Buffer vertexBuffer = GetResultOrFail(device_->createBuffer(BufferDescriptor{
      "positions", 4 * sizeof(gpu::tests::VertexInputPosition), BufferUsage::Vertex}));
  const Buffer instanceBuffer = GetResultOrFail(device_->createBuffer(BufferDescriptor{
      "instances", 2 * sizeof(gpu::tests::VertexInputInstance), BufferUsage::Vertex}));
  // Eight 16-bit indices; the draw below asks for nine.
  const Buffer indexBuffer =
      GetResultOrFail(device_->createBuffer(BufferDescriptor{"indices", 16, BufferUsage::Index}));
  const Texture target = GetResultOrFail(device_->createTexture(TextureDescriptor{
      "target", {4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view =
      GetResultOrFail(device_->createTextureView(target, TextureViewDescriptor{"target"}));

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_->createCommandEncoder());
  RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(
      RenderPassDescriptor{"indexed", {{view, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}}}}));
  ASSERT_THAT(pass->setPipeline(pipeline), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(1, instanceBuffer), IsOk());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(9),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("index range [0, 9)")));
  EXPECT_THAT(pass->end(), IsGpuError(GpuErrorType::OutOfBounds));
  EXPECT_THAT(encoder->finish(), IsGpuError(GpuErrorType::OutOfBounds));
  // Nothing was submitted, so the validation layers observed no command buffer at all.
  EXPECT_THAT(device_->lastSubmittedSerial(), testing::Eq(uint64_t{0}));
  EXPECT_THAT(device_->lastErrorForTest(), IsEmpty());
}

}  // namespace
}  // namespace donner::gpu::vulkan::tests
