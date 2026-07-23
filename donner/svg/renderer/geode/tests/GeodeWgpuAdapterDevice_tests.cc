/// @file
/// Conformance tests for \c donner::geode::GeodeWgpuAdapterDevice: every resource kind the
/// solid/gradient/mask/image pipeline family needs is created through the adapter on the real
/// headless wgpu device, a minimal render pass plus both copy commands executes to completion,
/// and fail-closed inputs are rejected before reaching wgpu.

#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

using testing::Ge;
using testing::HasSubstr;

namespace donner::geode {
namespace {

/// Minimal WGSL exercising a uniform binding and one vertex buffer, matching the pipeline the
/// conformance scene creates.
constexpr const char* kSolidWgsl = R"(
struct Uniforms {
  color: vec4f,
}
@group(0) @binding(0) var<uniform> uniforms: Uniforms;

@vertex
fn vs_main(@location(0) pos: vec2f) -> @builtin(position) vec4f {
  return vec4f(pos, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
  return uniforms.color;
}
)";

std::vector<uint8_t> MakeBytes(size_t count) {
  std::vector<uint8_t> bytes(count);
  for (size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<uint8_t>(i & 0xFF);
  }
  return bytes;
}

class GeodeWgpuAdapterDeviceTests : public testing::Test {
protected:
  void SetUp() override {
    geodeDevice_ = GeodeDevice::CreateHeadless();
    ASSERT_NE(geodeDevice_, nullptr)
        << "Failed to create the headless wgpu device. Check driver availability.";
    adapter_ = std::make_unique<GeodeWgpuAdapterDevice>(*geodeDevice_);
  }

  std::unique_ptr<GeodeDevice> geodeDevice_;
  std::unique_ptr<GeodeWgpuAdapterDevice> adapter_;
};

TEST_F(GeodeWgpuAdapterDeviceTests, FamilySceneRendersAndCompletes) {
  // ----- Every resource kind the pipeline family uses, created through the adapter -----
  const gpu::Texture target = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"target", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc}));
  const gpu::TextureView targetView = gpu::GetResultOrFail(
      adapter_->createTextureView(target, gpu::TextureViewDescriptor{"targetView"}));
  const gpu::Texture copyDestination = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"copyDestination", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::CopyDst}));

  const gpu::Texture sampled = gpu::GetResultOrFail(adapter_->createTexture(
      gpu::TextureDescriptor{"sampled", gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
                             gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst}));
  const gpu::TextureView sampledView = gpu::GetResultOrFail(
      adapter_->createTextureView(sampled, gpu::TextureViewDescriptor{"sampledView"}));
  const gpu::Sampler sampler = gpu::GetResultOrFail(adapter_->createSampler(
      gpu::SamplerDescriptor{"linearRepeat", gpu::FilterMode::Linear, gpu::FilterMode::Linear,
                             gpu::AddressMode::Repeat, gpu::AddressMode::Repeat}));

  const gpu::Buffer vertexBuffer = gpu::GetResultOrFail(adapter_->createBuffer(
      gpu::BufferDescriptor{"vertices", 48, gpu::BufferUsage::Vertex | gpu::BufferUsage::CopyDst}));
  const gpu::Buffer uniformBuffer =
      gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
          "uniforms", 16, gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst}));
  const gpu::Buffer readbackBuffer =
      gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
          "readback", 1024, gpu::BufferUsage::CopyDst | gpu::BufferUsage::MapRead}));

  const gpu::BindGroupLayout uniformLayout =
      gpu::GetResultOrFail(adapter_->createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "solidBindings",
          {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex | gpu::ShaderStage::Fragment,
                                     gpu::BindingType::UniformBuffer}}}));
  const gpu::BindGroup uniformGroup =
      gpu::GetResultOrFail(adapter_->createBindGroup(gpu::BindGroupDescriptor{
          "solidUniforms",
          uniformLayout,
          {gpu::BindGroupEntry{0, gpu::BufferBinding{uniformBuffer, 0, 16}}}}));

  // Texture + sampler binding kinds, exercised through creation (the minimal draw below binds
  // only the uniform group its pipeline layout declares).
  const gpu::BindGroupLayout textureLayout =
      gpu::GetResultOrFail(adapter_->createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "textureBindings",
          {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Fragment,
                                     gpu::BindingType::SampledTexture2dFloat},
           gpu::BindGroupLayoutEntry{1, gpu::ShaderStage::Fragment,
                                     gpu::BindingType::FilteringSampler}}}));
  const gpu::BindGroup textureGroup = gpu::GetResultOrFail(adapter_->createBindGroup(
      gpu::BindGroupDescriptor{"textureGroup",
                               textureLayout,
                               {gpu::BindGroupEntry{0, gpu::TextureViewBinding{sampledView}},
                                gpu::BindGroupEntry{1, gpu::SamplerBinding{sampler}}}}));
  (void)textureGroup;

  const gpu::PipelineLayout pipelineLayout = gpu::GetResultOrFail(adapter_->createPipelineLayout(
      gpu::PipelineLayoutDescriptor{"solidLayout", {uniformLayout}}));
  const gpu::ShaderModule shader = gpu::GetResultOrFail(adapter_->createShaderModule(
      gpu::ShaderModuleDescriptor{"solidWgsl", kSolidWgsl, gpu::ShaderSourceKind::Wgsl}));

  const gpu::RenderPipeline pipeline =
      gpu::GetResultOrFail(adapter_->createRenderPipeline(gpu::RenderPipelineDescriptor{
          "solid", pipelineLayout,
          gpu::VertexState{shader,
                           "vs_main",
                           {gpu::VertexBufferLayout{
                               8,
                               gpu::VertexStepMode::Vertex,
                               {gpu::VertexAttribute{gpu::VertexFormat::Float32x2, 0, 0}}}}},
          gpu::FragmentState{
              shader,
              "fs_main",
              {gpu::ColorTargetState{
                  gpu::TextureFormat::RGBA8Unorm,
                  gpu::BlendState{
                      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                                          gpu::BlendOperation::Add},
                      gpu::BlendComponent{gpu::BlendFactor::One, gpu::BlendFactor::OneMinusSrcAlpha,
                                          gpu::BlendOperation::Add}}}}}}));

  // The TEMPORARY 8a escape hatches resolve migrated objects for the still-wgpu GeoEncoder.
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuRenderPipelineOf(pipeline)));
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuBindGroupLayoutOf(uniformLayout)));
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuTextureOf(target)));
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuTextureViewOf(targetView)));

  // ----- Queue writes -----
  EXPECT_THAT(adapter_->writeBuffer(vertexBuffer, 0, MakeBytes(48)), gpu::IsOk());
  EXPECT_THAT(adapter_->writeBuffer(uniformBuffer, 0, MakeBytes(16)), gpu::IsOk());
  EXPECT_THAT(adapter_->writeTexture(sampled, MakeBytes(3 * 256 + 16),
                                     gpu::TexelCopyBufferLayout{0, 256, 4}, gpu::Extent2d{4, 4}),
              gpu::IsOk());

  // ----- Minimal render pass + both copy commands -----
  std::unique_ptr<gpu::CommandEncoder> encoder =
      gpu::GetResultOrFail(adapter_->createCommandEncoder());
  gpu::RenderPassEncoder* pass =
      gpu::GetResultOrFail(encoder->beginRenderPass(gpu::RenderPassDescriptor{
          "mainPass",
          {gpu::RenderPassColorAttachment{
              targetView, gpu::LoadOp::Clear, gpu::StoreOp::Store, {0, 0, 0.5, 1}}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(pipeline), gpu::IsOk());
  EXPECT_THAT(pass->setBindGroup(0, uniformGroup), gpu::IsOk());
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer), gpu::IsOk());
  EXPECT_THAT(pass->setScissorRect(0, 0, 4, 4), gpu::IsOk());
  EXPECT_THAT(pass->setViewport(0, 0, 4, 4, 0, 1), gpu::IsOk());
  EXPECT_THAT(pass->draw(6), gpu::IsOk());
  EXPECT_THAT(pass->end(), gpu::IsOk());
  EXPECT_THAT(encoder->copyTextureToTexture(target, copyDestination, gpu::Extent2d{4, 4}),
              gpu::IsOk());
  EXPECT_THAT(
      encoder->copyTextureToBuffer(gpu::TexelCopyTextureInfo{target}, readbackBuffer,
                                   gpu::TexelCopyBufferLayout{0, 256, 4}, gpu::Extent2d{4, 4}),
      gpu::IsOk());

  gpu::CommandBuffer commands = gpu::GetResultOrFail(encoder->finish());
  const uint64_t serial = gpu::GetResultOrFail(adapter_->submit(std::move(commands)));
  EXPECT_THAT(serial, Ge(uint64_t{1}));

  // The submission must complete on the real device and advance completedSerial.
  EXPECT_TRUE(adapter_->waitForSerial(serial, /*timeoutSeconds=*/30.0))
      << "submission " << serial
      << " did not complete; completedSerial=" << adapter_->completedSerial();
  EXPECT_THAT(adapter_->completedSerial(), Ge(serial));
}

TEST_F(GeodeWgpuAdapterDeviceTests, ImportedExternalTextureIsUsableAndNotOwned) {
  // A texture the adapter did not create, standing in for a host-owned render target.
  wgpu::TextureDescriptor externalDescriptor = {};
  externalDescriptor.label = wgpuLabel("externalTarget");
  externalDescriptor.size = {4u, 4u, 1u};
  externalDescriptor.format = wgpu::TextureFormat::RGBA8Unorm;
  externalDescriptor.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  externalDescriptor.mipLevelCount = 1;
  externalDescriptor.sampleCount = 1;
  externalDescriptor.dimension = wgpu::TextureDimension::_2D;
  ScopedWgpuHandle<wgpu::Texture> externalTexture(
      geodeDevice_->device().createTexture(externalDescriptor));
  ASSERT_TRUE(static_cast<bool>(externalTexture));

  gpu::Texture imported = gpu::GetResultOrFail(adapter_->importExternalTexture(
      externalTexture.get(), gpu::Extent2d{4, 4}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::RenderAttachment | gpu::TextureUsage::CopySrc));
  EXPECT_TRUE(static_cast<bool>(adapter_->wgpuTextureOf(imported)));

  // Destroying the handle forgets the registration but must not release the external texture:
  // the host's handle stays usable.
  EXPECT_THAT(adapter_->destroyTexture(std::move(imported)), gpu::IsOk());
  EXPECT_EQ(externalTexture.get().getWidth(), 4u);
}

TEST_F(GeodeWgpuAdapterDeviceTests, SpirvShaderKindFailsClosedAsUnsupported) {
  EXPECT_THAT(adapter_->createShaderModule(gpu::ShaderModuleDescriptor{
                  "spirv", "", gpu::ShaderSourceKind::Spirv, {0x07230203u, 0x00010300u}}),
              gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::Unsupported, HasSubstr("WGSL only")));
}

TEST_F(GeodeWgpuAdapterDeviceTests, MisalignedBindOffsetFailsClosedBeforeWgpu) {
  const gpu::Buffer uniformBuffer =
      gpu::GetResultOrFail(adapter_->createBuffer(gpu::BufferDescriptor{
          "uniforms", 64, gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst}));
  const gpu::BindGroupLayout layout =
      gpu::GetResultOrFail(adapter_->createBindGroupLayout(gpu::BindGroupLayoutDescriptor{
          "uniforms",
          {gpu::BindGroupLayoutEntry{0, gpu::ShaderStage::Vertex,
                                     gpu::BindingType::UniformBuffer}}}));

  // Rejected by the base class's shared validation (InvalidDescriptor naming the offset), so
  // the misaligned binding never reaches wgpu's createBindGroup.
  EXPECT_THAT(
      adapter_->createBindGroup(gpu::BindGroupDescriptor{
          "group", layout, {gpu::BindGroupEntry{0, gpu::BufferBinding{uniformBuffer, 8, 16}}}}),
      gpu::IsGpuErrorWithMessage(gpu::GpuErrorType::InvalidDescriptor,
                                 HasSubstr("offsetBytes 8 is not a multiple of the 256-byte "
                                           "binding offset alignment")));
}

}  // namespace
}  // namespace donner::geode
