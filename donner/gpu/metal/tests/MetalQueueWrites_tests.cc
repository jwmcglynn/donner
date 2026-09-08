/// @file
/// Queue writes preserve earlier submissions while later submissions observe the new contents.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/metal/tests/MetalDeviceGate.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu::metal::tests {
namespace {

struct Capture {
  Buffer readback;
  uint64_t serial = 0;
};

class MetalQueueWritesTest : public testing::Test {
protected:
  void SetUp() override {
    device_ = MetalDevice::Create();
    DONNER_REQUIRE_METAL_DEVICE(device_, "Metal queue write ordering");
  }

  void TearDown() override {
    if (device_) {
      device_->resumeSubmissionsForTest();
    }
  }

  Capture captureTexture(const Texture& texture) {
    Capture capture;
    capture.readback = GetResultOrFail(device_->createBuffer(
        BufferDescriptor{"capture", 256, BufferUsage::CopyDst | BufferUsage::MapRead}));
    auto encoder = GetResultOrFail(device_->createCommandEncoder());
    EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{texture}, capture.readback,
                                             TexelCopyBufferLayout{0, 256, 1}, Extent2d{1, 1}),
                IsOk());
    capture.serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
    return capture;
  }

  Capture captureUniform(const Buffer& uniform) {
    const ShaderModule module = GetResultOrFail(device_->createShaderModule(
        ShaderModuleDescriptor{"captureUniform",
                               RcString(R"msl(#include <metal_stdlib>
using namespace metal;
kernel void cs_main(constant float4& color [[buffer(1)]],
                    texture2d<float, access::write> output [[texture(1)]]) {
  output.write(color, uint2(0));
}
)msl"),
                               ShaderSourceKind::Msl,
                               {},
                               {ComputeEntryPointInfo{"cs_main", WorkgroupSize{1, 1, 1}}}}));
    const BindGroupLayout layout =
        GetResultOrFail(device_->createBindGroupLayout(BindGroupLayoutDescriptor{
            "captureLayout",
            {{0, ShaderStage::Compute, BindingType::UniformBuffer},
             {1, ShaderStage::Compute, BindingType::WriteOnlyStorageTexture2d}}}));
    const PipelineLayout pipelineLayout = GetResultOrFail(
        device_->createPipelineLayout(PipelineLayoutDescriptor{"capturePipelineLayout", {layout}}));
    const ComputePipeline pipeline = GetResultOrFail(device_->createComputePipeline(
        ComputePipelineDescriptor{"capturePipeline", pipelineLayout,
                                  ComputeState{module, "cs_main"}, WorkgroupSize{1, 1, 1}}));
    const Texture output = GetResultOrFail(device_->createTexture(
        TextureDescriptor{"captureOutput", Extent2d{1, 1}, TextureFormat::RGBA8Unorm,
                          TextureUsage::StorageBinding | TextureUsage::CopySrc}));
    const TextureView view =
        GetResultOrFail(device_->createTextureView(output, TextureViewDescriptor{"captureView"}));
    const BindGroup group = GetResultOrFail(device_->createBindGroup(
        BindGroupDescriptor{"captureGroup",
                            layout,
                            {{0, BufferBinding{uniform, 0, 16}}, {1, TextureViewBinding{view}}}}));
    Capture capture;
    capture.readback = GetResultOrFail(device_->createBuffer(
        BufferDescriptor{"capture", 256, BufferUsage::CopyDst | BufferUsage::MapRead}));
    auto encoder = GetResultOrFail(device_->createCommandEncoder());
    ComputePassEncoder* pass = GetResultOrFail(encoder->beginComputePass({"capture"}));
    EXPECT_THAT(pass->setPipeline(pipeline), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, group), IsOk());
    EXPECT_THAT(pass->dispatchWorkgroups(1), IsOk());
    EXPECT_THAT(pass->end(), IsOk());
    EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{output}, capture.readback,
                                             TexelCopyBufferLayout{0, 256, 1}, Extent2d{1, 1}),
                IsOk());
    capture.serial = GetResultOrFail(device_->submit(GetResultOrFail(encoder->finish())));
    return capture;
  }

  void expectPixel(const Capture& capture, std::array<uint8_t, 4> expected, const char* label) {
    const auto bytes = GetResultOrFail(device_->readBackBuffer(capture.readback));
    ASSERT_GE(bytes.size(), 4u);
    const svg::RendererBitmap actual{
        .dimensions = {1, 1}, .pixels = {bytes[0], bytes[1], bytes[2], bytes[3]}, .rowBytes = 4};
    const svg::RendererBitmap reference{
        .dimensions = {1, 1}, .pixels = {expected.begin(), expected.end()}, .rowBytes = 4};
    editor::tests::CompareBitmapToBitmap(actual, reference, label,
                                         editor::tests::PixelmatchIdentityParams());
  }

  std::unique_ptr<MetalDevice> device_;
};

TEST_F(MetalQueueWritesTest, UniformUpdateDoesNotChangeAnEarlierSubmission) {
  const Buffer uniform = GetResultOrFail(device_->createBuffer(
      BufferDescriptor{"color", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  const std::array<float, 4> red = {1.0f, 0.0f, 0.0f, 1.0f};
  const std::array<float, 4> blue = {0.0f, 0.0f, 1.0f, 1.0f};
  ASSERT_THAT(
      device_->writeBuffer(
          uniform, 0, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(red.data()), 16)),
      IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureUniform(uniform);
  ASSERT_THAT(device_->completedSerial(), testing::Lt(first.serial));
  ASSERT_THAT(
      device_->writeBuffer(
          uniform, 0, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(blue.data()), 16)),
      IsOk());
  const Capture second = captureUniform(uniform);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, {255, 0, 0, 255}, "uniform_before_update");
  expectPixel(second, {0, 0, 255, 255}, "uniform_after_update");
}

TEST_F(MetalQueueWritesTest, TextureUpdateDoesNotChangeAnEarlierSubmission) {
  const Texture texture = GetResultOrFail(
      device_->createTexture(TextureDescriptor{"source", Extent2d{1, 1}, TextureFormat::RGBA8Unorm,
                                               TextureUsage::CopySrc | TextureUsage::CopyDst}));
  const std::array<uint8_t, 4> red = {255, 0, 0, 255};
  const std::array<uint8_t, 4> blue = {0, 0, 255, 255};
  const TexelCopyBufferLayout layout{0, 256, 1};
  ASSERT_THAT(device_->writeTexture(texture, red, layout, Extent2d{1, 1}), IsOk());
  ASSERT_THAT(device_->pauseSubmissionsForTest(), IsOk());
  const Capture first = captureTexture(texture);
  ASSERT_THAT(device_->completedSerial(), testing::Lt(first.serial));
  ASSERT_THAT(device_->writeTexture(texture, blue, layout, Extent2d{1, 1}), IsOk());
  const Capture second = captureTexture(texture);
  device_->resumeSubmissionsForTest();
  ASSERT_TRUE(device_->waitForSerial(second.serial, 5.0)) << device_->lastErrorForTest();
  expectPixel(first, red, "texture_before_update");
  expectPixel(second, blue, "texture_after_update");
}

}  // namespace
}  // namespace donner::gpu::metal::tests
