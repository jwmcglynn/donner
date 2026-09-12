/// @file
/// Encoder state machine tests: pass lifecycle, draw prerequisites, error poisoning, and copy
/// validation.

#include "donner/gpu/CommandEncoder.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "donner/gpu/GpuLimits.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::AllOf;
using testing::Eq;
using testing::HasSubstr;
using testing::Ne;

namespace donner::gpu {
namespace {

/// Backend fake that reports the 32-bit index range as unsupported, standing in for a Vulkan
/// device whose physical device lacks fullDrawIndexUint32. Every other hook accepts.
class Uint16OnlyDevice final : public Device {
public:
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }
  bool supportsFullIndexRange(IndexFormat format) const override {
    return format == IndexFormat::Uint16;
  }

protected:
  Status onCreateBuffer(uint32_t, const BufferDescriptor&) override { return OkStatus(); }
  Status onCreateTexture(uint32_t, const TextureDescriptor&) override { return OkStatus(); }
  Status onCreateTextureView(uint32_t, uint32_t, const TextureViewDescriptor&) override {
    return OkStatus();
  }
  Status onCreateSampler(uint32_t, const SamplerDescriptor&) override { return OkStatus(); }
  Status onCreateBindGroupLayout(uint32_t, const BindGroupLayoutDescriptor&) override {
    return OkStatus();
  }
  Status onCreateBindGroup(uint32_t, const BindGroupDescriptor&) override { return OkStatus(); }
  Status onCreatePipelineLayout(uint32_t, const PipelineLayoutDescriptor&) override {
    return OkStatus();
  }
  Status onCreateShaderModule(uint32_t, const ShaderModuleDescriptor&) override {
    return OkStatus();
  }
  Status onCreateRenderPipeline(uint32_t, const RenderPipelineDescriptor&) override {
    return OkStatus();
  }
  Status onCreateComputePipeline(uint32_t, const ComputePipelineDescriptor&) override {
    return OkStatus();
  }
  void onDestroyResource(std::string_view, uint32_t) override {}
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&) override {
    return OkStatus();
  }
  Status onSubmit(uint64_t, uint32_t, std::span<const Command>) override { return OkStatus(); }
};

/// Creates the solid-fill scene used by the encoder tests: a render target with view, vertex and
/// uniform buffers, a readback buffer, and a pipeline with one uniform bind group.
class CommandEncoderTests : public testing::Test {
protected:
  void SetUp() override {
    target_ = GetResultOrFail(device_.createTexture(
        TextureDescriptor{"target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                          TextureUsage::RenderAttachment | TextureUsage::CopySrc}));
    targetView_ =
        GetResultOrFail(device_.createTextureView(target_, TextureViewDescriptor{"targetView"}));
    vertexBuffer_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"vertices", 48, BufferUsage::Vertex | BufferUsage::CopyDst}));
    uniformBuffer_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"uniforms", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
    // 24 bytes: twelve 16-bit or six 32-bit indices.
    indexBuffer_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"indices", 24, BufferUsage::Index | BufferUsage::CopyDst}));
    readbackBuffer_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));

    bindGroupLayout_ = GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
        "uniforms",
        {BindGroupLayoutEntry{0, ShaderStage::Vertex | ShaderStage::Fragment,
                              BindingType::UniformBuffer}}}));
    pipelineLayout_ = GetResultOrFail(
        device_.createPipelineLayout(PipelineLayoutDescriptor{"solidLayout", {bindGroupLayout_}}));
    bindGroup_ = GetResultOrFail(device_.createBindGroup(
        BindGroupDescriptor{"solidUniforms",
                            bindGroupLayout_,
                            {BindGroupEntry{0, BufferBinding{uniformBuffer_, 0, 16}}}}));
    shader_ = GetResultOrFail(device_.createShaderModule(ShaderModuleDescriptor{
        "solidFill", "@vertex fn vsMain() {}\n@fragment fn fsMain() {}", ShaderSourceKind::Wgsl}));
    pipeline_ = GetResultOrFail(device_.createRenderPipeline(solidPipelineDescriptor()));

    encoder_ = GetResultOrFail(device_.createCommandEncoder());
  }

  /// Begins the standard pass with the pipeline, bind group and vertex buffer set, so a test can
  /// go straight to binding an index buffer and drawing.
  RenderPassEncoder* beginDrawablePass() {
    RenderPassEncoder* pass = beginPass();
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    return pass;
  }

  RenderPipelineDescriptor solidPipelineDescriptor() const {
    return RenderPipelineDescriptor{
        "solid", pipelineLayout_,
        VertexState{
            shader_,
            "vsMain",
            {VertexBufferLayout{
                8, VertexStepMode::Vertex, {VertexAttribute{VertexFormat::Float32x2, 0, 0}}}}},
        FragmentState{shader_,
                      "fsMain",
                      {ColorTargetState{
                          TextureFormat::RGBA8Unorm,
                          BlendState{BlendComponent{BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                                                    BlendOperation::Add},
                                     BlendComponent{BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                                                    BlendOperation::Add}}}}}};
  }

  RenderPassDescriptor passDescriptor() const {
    return RenderPassDescriptor{
        "mainPass",
        {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store, {0, 0, 0.5, 1}}}};
  }

  /// Begins the standard pass, failing the test if it cannot begin.
  RenderPassEncoder* beginPass() {
    return GetResultOrFail(encoder_->beginRenderPass(passDescriptor()));
  }

  RecordingDevice device_;
  Texture target_;
  TextureView targetView_;
  Buffer vertexBuffer_;
  Buffer uniformBuffer_;
  Buffer indexBuffer_;
  Buffer readbackBuffer_;
  BindGroupLayout bindGroupLayout_;
  PipelineLayout pipelineLayout_;
  BindGroup bindGroup_;
  ShaderModule shader_;
  RenderPipeline pipeline_;
  std::unique_ptr<CommandEncoder> encoder_;
};

TEST_F(CommandEncoderTests, FullPassEncodesAndSubmits) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_NE(pass, nullptr);

  EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
  EXPECT_THAT(pass->setScissorRect(0, 0, 4, 4), IsOk());
  EXPECT_THAT(pass->setViewport(0, 0, 4, 4, 0, 1), IsOk());
  EXPECT_THAT(pass->draw(6), IsOk());
  EXPECT_THAT(pass->end(), IsOk());
  EXPECT_THAT(encoder_->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readbackBuffer_,
                                            TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsOk());

  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());
}

TEST_F(CommandEncoderTests, DrawBeforeSetPipelineFails) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->draw(3),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("no pipeline is set")));
}

TEST_F(CommandEncoderTests, BeginRenderPassTwiceFails) {
  beginPass();
  EXPECT_THAT(encoder_->beginRenderPass(passDescriptor()),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("already active")));
}

TEST_F(CommandEncoderTests, PassOpsAfterEndFail) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->end(), IsOk());
  EXPECT_THAT(
      pass->setPipeline(pipeline_),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("no render pass is active")));
}

TEST_F(CommandEncoderTests, SecondPassAfterEndIsAllowed) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->end(), IsOk());

  // The state error above poisons nothing: ending then beginning a new pass is valid.
  EXPECT_THAT(encoder_->beginRenderPass(passDescriptor()), HasResult());
}

TEST_F(CommandEncoderTests, FinishWithOpenPassFails) {
  beginPass();
  EXPECT_THAT(encoder_->finish(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("still active")));
}

TEST_F(CommandEncoderTests, ErrorPoisonsEncoderAndFinishReturnsFirstError) {
  RenderPassEncoder* pass = beginPass();

  // First error: a null buffer handle.
  const Status firstError = pass->setVertexBuffer(0, Buffer());
  ASSERT_THAT(firstError, IsGpuError(GpuErrorType::InvalidHandle));

  // Subsequent operations and finish all return the first error, not new ones.
  EXPECT_THAT(pass->draw(3),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, Eq(firstError.error().message)));
  EXPECT_THAT(encoder_->finish(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, Eq(firstError.error().message)));
}

TEST_F(CommandEncoderTests, FinishTwiceFails) {
  ASSERT_THAT(encoder_->finish(), HasResult());
  EXPECT_THAT(encoder_->finish(),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("already finished")));
}

TEST_F(CommandEncoderTests, OpsAfterFinishFail) {
  ASSERT_THAT(encoder_->finish(), HasResult());
  EXPECT_THAT(encoder_->beginRenderPass(passDescriptor()), IsGpuError(GpuErrorType::InvalidState));
}

TEST_F(CommandEncoderTests, DrawWithoutVertexBufferFails) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->setPipeline(pipeline_), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());

  EXPECT_THAT(pass->draw(3), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                   HasSubstr("vertex buffer at slot 0")));
}

TEST_F(CommandEncoderTests, DrawWithoutBindGroupFails) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->setPipeline(pipeline_), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());

  EXPECT_THAT(pass->draw(3), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                   HasSubstr("bind group at index 0")));
}

TEST_F(CommandEncoderTests, DrawWithIncompatibleBindGroupFails) {
  // A bind group created against a different (structurally identical) layout is incompatible.
  const BindGroupLayout otherLayout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "otherUniforms",
          {BindGroupLayoutEntry{0, ShaderStage::Vertex | ShaderStage::Fragment,
                                BindingType::UniformBuffer}}}));
  const BindGroup otherGroup = GetResultOrFail(device_.createBindGroup(BindGroupDescriptor{
      "otherGroup", otherLayout, {BindGroupEntry{0, BufferBinding{uniformBuffer_, 0, 16}}}}));

  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->setPipeline(pipeline_), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, otherGroup), IsOk());

  EXPECT_THAT(pass->draw(3),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("different layout")));
}

TEST_F(CommandEncoderTests, DrawVertexRangeBeyondBufferFails) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->setPipeline(pipeline_), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());

  // The 48-byte buffer holds 6 vertices with stride 8; drawing 7 overflows.
  EXPECT_THAT(pass->draw(7), IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(CommandEncoderTests, SetVertexBufferRejectsNonVertexUsage) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setVertexBuffer(0, uniformBuffer_),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("Vertex usage")));
}

TEST_F(CommandEncoderTests, SetVertexBufferRejectsOutOfLimitSlot) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setVertexBuffer(kMaxVertexBuffers, vertexBuffer_),
              IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(CommandEncoderTests, SetBindGroupRejectsOutOfLimitIndex) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setBindGroup(kMaxBindGroups, bindGroup_),
              IsGpuError(GpuErrorType::LimitExceeded));
}

TEST_F(CommandEncoderTests, ScissorRectBeyondAttachmentFails) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setScissorRect(2, 0, 4, 4), IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(CommandEncoderTests, ViewportWithInvalidDepthRangeFails) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setViewport(0, 0, 4, 4, /*minDepth=*/0.75f, /*maxDepth=*/0.25f),
              IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(CommandEncoderTests, CopyTextureToBufferDuringPassFails) {
  beginPass();
  EXPECT_THAT(
      encoder_->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readbackBuffer_,
                                    TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("not allowed inside a pass")));
}

TEST_F(CommandEncoderTests, CopyTextureToBufferRejectsMisalignedBytesPerRow) {
  EXPECT_THAT(
      encoder_->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readbackBuffer_,
                                    TexelCopyBufferLayout{0, 100, 4}, Extent2d{4, 4}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("not a multiple of 256")));
}

TEST_F(CommandEncoderTests, CopyTextureToBufferRejectsBufferTooSmall) {
  const Buffer tiny = GetResultOrFail(device_.createBuffer(
      BufferDescriptor{"tiny", 64, BufferUsage::CopyDst | BufferUsage::MapRead}));
  EXPECT_THAT(encoder_->copyTextureToBuffer(TexelCopyTextureInfo{target_}, tiny,
                                            TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(CommandEncoderTests, CopyTextureToBufferRejectsMissingCopySrc) {
  const Texture noCopySrc = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "noCopySrc", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  EXPECT_THAT(encoder_->copyTextureToBuffer(TexelCopyTextureInfo{noCopySrc}, readbackBuffer_,
                                            TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("CopySrc")));
}

TEST_F(CommandEncoderTests, BeginRenderPassRejectsStaleView) {
  ASSERT_THAT(device_.destroyTextureView(std::move(targetView_)), IsOk());
  EXPECT_THAT(encoder_->beginRenderPass(passDescriptor()), IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(CommandEncoderTests, BeginRenderPassRejectsNonRenderableView) {
  const Texture sampledOnly = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "sampledOnly", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  const TextureView sampledView =
      GetResultOrFail(device_.createTextureView(sampledOnly, TextureViewDescriptor{"view"}));

  EXPECT_THAT(
      encoder_->beginRenderPass(RenderPassDescriptor{
          "badPass", {RenderPassColorAttachment{sampledView, LoadOp::Clear, StoreOp::Store}}}),
      IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("RenderAttachment")));
}

TEST_F(CommandEncoderTests, SetBindGroupRejectsDestroyedBuffer) {
  ASSERT_THAT(device_.destroyBuffer(std::move(uniformBuffer_)), IsOk());

  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setBindGroup(0, bindGroup_),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("stale")));
}

TEST_F(CommandEncoderTests, SetBindGroupRejectsBufferSlotReuse) {
  const uint32_t uniformSlot = uniformBuffer_.slotIndex();
  ASSERT_THAT(device_.destroyBuffer(std::move(uniformBuffer_)), IsOk());

  // The replacement reuses the freed buffer slot; the group's stale reference must not alias it.
  const Buffer replacement = GetResultOrFail(device_.createBuffer(
      BufferDescriptor{"replacement", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
  ASSERT_THAT(replacement.slotIndex(), Eq(uniformSlot));

  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setBindGroup(0, bindGroup_),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("stale")));
}

TEST_F(CommandEncoderTests, IndexedPassEncodesBothFormatsAndSubmits) {
  RenderPassEncoder* pass = beginDrawablePass();
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(12), IsOk());
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint32), IsOk());
  EXPECT_THAT(pass->drawIndexed(6, 2, 0, 0, 0), IsOk());
  EXPECT_THAT(pass->end(), IsOk());

  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());
  EXPECT_THAT(device_.serialize(),
              AllOf(HasSubstr("  setIndexBuffer buffer=buffer#2 format=Uint16 offsetBytes=0\n"
                              "  drawIndexed indexCount=12 instanceCount=1 firstIndex=0 "
                              "baseVertex=0 firstInstance=0\n"),
                    HasSubstr("  setIndexBuffer buffer=buffer#2 format=Uint32 offsetBytes=0\n"
                              "  drawIndexed indexCount=6 instanceCount=2 firstIndex=0 "
                              "baseVertex=0 firstInstance=0\n")));
}

TEST_F(CommandEncoderTests, SetIndexBufferRejectsNonIndexUsage) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setIndexBuffer(vertexBuffer_, IndexFormat::Uint16),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("Index usage")));
}

TEST_F(CommandEncoderTests, SetIndexBufferRejectsUnknownFormat) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, static_cast<IndexFormat>(7)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("unknown")));
}

TEST_F(CommandEncoderTests, SixteenBitIndexBindingRejectsAnOddOffset) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, 1),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("not a multiple of the 2-byte index width")));
}

TEST_F(CommandEncoderTests, ThirtyTwoBitIndexBindingRejectsATwoByteOffset) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint32, 2),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("not a multiple of the 4-byte index width")));
}

TEST_F(CommandEncoderTests, IndexBindingPastEndPoisonsButExactEndCanBind) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, 24), IsOk());
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, 26),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("offsetBytes 26")));
  EXPECT_THAT(encoder_->finish(), IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(CommandEncoderTests, SetIndexBufferRejectsDestroyedBuffer) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(device_.destroyBuffer(std::move(indexBuffer_)), IsOk());
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16),
              IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(CommandEncoderTests, SetIndexBufferRejectsAHandleWhoseSlotWasReused) {
  const uint32_t slot = indexBuffer_.slotIndex();
  const uint32_t generation = indexBuffer_.generation();
  const uint64_t deviceId = indexBuffer_.deviceId();
  ASSERT_THAT(device_.destroyBuffer(std::move(indexBuffer_)), IsOk());
  const Buffer replacement = GetResultOrFail(
      device_.createBuffer(BufferDescriptor{"replacement", 24, BufferUsage::Index}));
  ASSERT_THAT(replacement.slotIndex(), Eq(slot));

  // A handle carrying the retired generation must not alias the replacement in the same slot.
  const Buffer stale = Buffer::CreateForBackend(slot, generation, deviceId);
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setIndexBuffer(stale, IndexFormat::Uint16),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("stale")));
}

TEST_F(CommandEncoderTests, SetIndexBufferRejectsAnotherDevicesBuffer) {
  RecordingDevice otherDevice;
  const Buffer foreign = GetResultOrFail(
      otherDevice.createBuffer(BufferDescriptor{"foreign", 24, BufferUsage::Index}));
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setIndexBuffer(foreign, IndexFormat::Uint16),
              IsGpuError(GpuErrorType::DeviceMismatch));
}

TEST_F(CommandEncoderTests, SetIndexBufferOutsideARenderPassFails) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->end(), IsOk());
  EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("no render pass")));
}

TEST_F(CommandEncoderTests, ThirtyTwoBitIndicesAreRefusedWhereTheDeviceCapsTheirRange) {
  Uint16OnlyDevice device;
  const Texture target = GetResultOrFail(device.createTexture(TextureDescriptor{
      "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view =
      GetResultOrFail(device.createTextureView(target, TextureViewDescriptor{"targetView"}));
  const Buffer indices =
      GetResultOrFail(device.createBuffer(BufferDescriptor{"indices", 24, BufferUsage::Index}));
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
  RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
      "pass", {RenderPassColorAttachment{view, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}}}}));

  // The narrower format still binds on the same device; only the capped one is refused, and it
  // is refused at record time so no command buffer carrying it can exist.
  EXPECT_THAT(pass->setIndexBuffer(indices, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(
      pass->setIndexBuffer(indices, IndexFormat::Uint32),
      IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("full Uint32 index range")));
  EXPECT_THAT(encoder->finish(), IsGpuError(GpuErrorType::Unsupported));
}

TEST_F(CommandEncoderTests, DrawIndexedBeforeSetPipelineFails) {
  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->drawIndexed(3),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("no pipeline is set")));
}

TEST_F(CommandEncoderTests, DrawIndexedWithoutIndexBufferFails) {
  RenderPassEncoder* pass = beginDrawablePass();
  EXPECT_THAT(pass->drawIndexed(3), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                          HasSubstr("no index buffer is bound")));
}

TEST_F(CommandEncoderTests, DrawIndexedRejectsATriangleStripPipeline) {
  RenderPipelineDescriptor stripDescriptor = solidPipelineDescriptor();
  stripDescriptor.topology = PrimitiveTopology::TriangleStrip;
  const RenderPipeline strip = GetResultOrFail(device_.createRenderPipeline(stripDescriptor));

  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->setPipeline(strip), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(4),
              IsGpuErrorWithMessage(GpuErrorType::Unsupported, HasSubstr("TriangleStrip")));
}

TEST_F(CommandEncoderTests, DrawIndexedOnePastTheBoundIndexRangeFails) {
  RenderPassEncoder* pass = beginDrawablePass();
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(12), IsOk());
  EXPECT_THAT(pass->drawIndexed(13),
              IsGpuErrorWithMessage(
                  GpuErrorType::OutOfBounds,
                  AllOf(HasSubstr("Uint16 index range [0, 13)"), HasSubstr("24 bytes available"))));
}

TEST_F(CommandEncoderTests, DrawIndexedFirstIndexNearUint32MaxCannotWrap) {
  RenderPassEncoder* pass = beginDrawablePass();
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint32), IsOk());
  // (UINT32_MAX + 1) * 4 wraps to zero in 32-bit arithmetic and would pass a naive check.
  EXPECT_THAT(pass->drawIndexed(1, 1, std::numeric_limits<uint32_t>::max()),
              IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(CommandEncoderTests, DrawIndexedBindOffsetAndFirstIndexAreBoundedTogether) {
  RenderPassEncoder* pass = beginDrawablePass();
  // Four 16-bit indices remain after the offset; skipping two leaves room for two.
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16, 16), IsOk());
  EXPECT_THAT(pass->drawIndexed(2, 1, 2), IsOk());
  EXPECT_THAT(pass->drawIndexed(2, 1, 3),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("8 bytes available")));
}

TEST_F(CommandEncoderTests, RebindingWithAnotherFormatChangesTheEnforcedRange) {
  RenderPassEncoder* pass = beginDrawablePass();
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(12), IsOk());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint32), IsOk());
  EXPECT_THAT(pass->drawIndexed(6), IsOk());
  EXPECT_THAT(pass->drawIndexed(7),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("Uint32 index range")));
}

TEST_F(CommandEncoderTests, DrawIndexedZeroCountsRecordLikeDrawAndSubmit) {
  RenderPassEncoder* pass = beginDrawablePass();
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(0), IsOk());
  EXPECT_THAT(pass->drawIndexed(6, 0), IsOk());
  EXPECT_THAT(pass->end(), IsOk());
  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());

  // Both draws reach the stream verbatim; backends decide to issue nothing for them.
  EXPECT_THAT(device_.serialize(),
              AllOf(HasSubstr("  drawIndexed indexCount=0 instanceCount=1 firstIndex=0 "
                              "baseVertex=0 firstInstance=0\n"),
                    HasSubstr("  drawIndexed indexCount=6 instanceCount=0 firstIndex=0 "
                              "baseVertex=0 firstInstance=0\n")));
}

TEST_F(CommandEncoderTests, DrawIndexedZeroCountStillBoundsTheFirstIndex) {
  RenderPassEncoder* pass = beginDrawablePass();
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(0, 1, 12), IsOk());
  EXPECT_THAT(pass->drawIndexed(0, 1, 13), IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(CommandEncoderTests, DrawIndexedRecordsANegativeBaseVertex) {
  RenderPassEncoder* pass = beginDrawablePass();
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(6, 1, 3, -3, 2), IsOk());
  EXPECT_THAT(pass->end(), IsOk());
  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());
  EXPECT_THAT(device_.serialize(),
              HasSubstr("  drawIndexed indexCount=6 instanceCount=1 firstIndex=3 baseVertex=-3 "
                        "firstInstance=2\n"));
}

TEST_F(CommandEncoderTests, DrawIndexedWithoutVertexBufferFails) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->setPipeline(pipeline_), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(3), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                          HasSubstr("vertex buffer at slot 0")));
}

TEST_F(CommandEncoderTests, DrawIndexedWithoutBindGroupFails) {
  RenderPassEncoder* pass = beginPass();
  ASSERT_THAT(pass->setPipeline(pipeline_), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(3), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                          HasSubstr("bind group at index 0")));
}

/// Pipeline with a vertex-stepped slot 0 and an instance-stepped slot 1, both stride 8, so the
/// 48-byte vertex buffer holds six elements of either kind.
class IndexedInstancingTests : public CommandEncoderTests {
protected:
  void SetUp() override {
    CommandEncoderTests::SetUp();
    RenderPipelineDescriptor descriptor = solidPipelineDescriptor();
    descriptor.vertex.buffers.push_back(VertexBufferLayout{
        8, VertexStepMode::Instance, {VertexAttribute{VertexFormat::Float32x2, 0, 1}}});
    instancedPipeline_ = GetResultOrFail(device_.createRenderPipeline(descriptor));
  }

  RenderPassEncoder* beginInstancedPass() {
    RenderPassEncoder* pass = beginPass();
    EXPECT_THAT(pass->setPipeline(instancedPipeline_), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(1, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
    return pass;
  }

  RenderPipeline instancedPipeline_;
};

TEST_F(IndexedInstancingTests, InstanceRangeBeyondTheInstanceBufferFails) {
  RenderPassEncoder* pass = beginInstancedPass();
  EXPECT_THAT(pass->drawIndexed(3, 4, 0, 0, 2), IsOk());
  EXPECT_THAT(
      pass->drawIndexed(3, 5, 0, 0, 2),
      IsGpuErrorWithMessage(GpuErrorType::OutOfBounds,
                            AllOf(HasSubstr("instance range [2, 7)"), HasSubstr("slot 1"))));
}

TEST_F(IndexedInstancingTests, VertexSteppedRangeIsTheCallersResponsibility) {
  // The vertex reached is baseVertex plus an index value the host never reads, so no CPU-side
  // bound exists: a baseVertex far past the six-element buffer records without error.
  RenderPassEncoder* pass = beginInstancedPass();
  EXPECT_THAT(pass->drawIndexed(3, 1, 0, 1000), IsOk());
}

TEST_F(CommandEncoderTests, IndexBindingDoesNotSurviveEnd) {
  RenderPassEncoder* pass = beginDrawablePass();
  ASSERT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
  EXPECT_THAT(pass->drawIndexed(3), IsOk());
  ASSERT_THAT(pass->end(), IsOk());

  RenderPassEncoder* second = beginDrawablePass();
  EXPECT_THAT(second, Ne(nullptr));
  EXPECT_THAT(second->drawIndexed(3), IsGpuErrorWithMessage(GpuErrorType::InvalidState,
                                                            HasSubstr("no index buffer is bound")));
}

class SetBindGroupTextureTests : public CommandEncoderTests {
protected:
  void SetUp() override {
    CommandEncoderTests::SetUp();

    sampledTexture_ = GetResultOrFail(device_.createTexture(TextureDescriptor{
        "sampled", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
    sampledView_ = GetResultOrFail(
        device_.createTextureView(sampledTexture_, TextureViewDescriptor{"sampledView"}));
    sampler_ = GetResultOrFail(device_.createSampler(SamplerDescriptor{"linear"}));
    textureLayout_ = GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
        "textureBindings",
        {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat},
         BindGroupLayoutEntry{1, ShaderStage::Fragment, BindingType::FilteringSampler}}}));
    textureGroup_ = GetResultOrFail(device_.createBindGroup(
        BindGroupDescriptor{"textureGroup",
                            textureLayout_,
                            {BindGroupEntry{0, TextureViewBinding{sampledView_}},
                             BindGroupEntry{1, SamplerBinding{sampler_}}}}));
  }

  Texture sampledTexture_;
  TextureView sampledView_;
  Sampler sampler_;
  BindGroupLayout textureLayout_;
  BindGroup textureGroup_;
};

TEST_F(SetBindGroupTextureTests, RejectsDestroyedTextureView) {
  ASSERT_THAT(device_.destroyTextureView(std::move(sampledView_)), IsOk());

  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setBindGroup(1, textureGroup_),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("stale")));
}

TEST_F(SetBindGroupTextureTests, RejectsDestroyedUnderlyingTexture) {
  ASSERT_THAT(device_.destroyTexture(std::move(sampledTexture_)), IsOk());

  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(
      pass->setBindGroup(1, textureGroup_),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("texture was destroyed")));
}

TEST_F(SetBindGroupTextureTests, RejectsDestroyedSampler) {
  ASSERT_THAT(device_.destroySampler(std::move(sampler_)), IsOk());

  RenderPassEncoder* pass = beginPass();
  EXPECT_THAT(pass->setBindGroup(1, textureGroup_),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("stale")));
}

TEST_F(CommandEncoderTests, SetPipelineRejectsMismatchedTargetFormat) {
  // A pipeline whose color target is RGBA8 cannot be used in a BGRA8 pass.
  const Texture bgraTarget = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "bgraTarget", Extent2d{4, 4}, TextureFormat::BGRA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView bgraView =
      GetResultOrFail(device_.createTextureView(bgraTarget, TextureViewDescriptor{"bgraView"}));

  RenderPassEncoder* pass = GetResultOrFail(encoder_->beginRenderPass(RenderPassDescriptor{
      "bgraPass", {RenderPassColorAttachment{bgraView, LoadOp::Clear, StoreOp::Store}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(pipeline_),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("format")));
}

TEST_F(CommandEncoderTests, SetPipelineRejectsAttachmentCountMismatch) {
  // The pass has two attachments but the pipeline declares one color target.
  const Texture second = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "second", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView secondView =
      GetResultOrFail(device_.createTextureView(second, TextureViewDescriptor{"secondView"}));

  RenderPassEncoder* pass = GetResultOrFail(encoder_->beginRenderPass(RenderPassDescriptor{
      "twoAttachmentPass",
      {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store},
       RenderPassColorAttachment{secondView, LoadOp::Clear, StoreOp::Store}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(pipeline_),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("color target")));
}

TEST_F(CommandEncoderTests, DuplicateAttachmentViewFails) {
  EXPECT_THAT(encoder_->beginRenderPass(RenderPassDescriptor{
                  "duplicatePass",
                  {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store},
                   RenderPassColorAttachment{targetView_, LoadOp::Load, StoreOp::Store}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("multiple color attachments")));
}

TEST_F(CommandEncoderTests, DistinctViewsOfOneAttachmentTextureFail) {
  const TextureView secondView =
      GetResultOrFail(device_.createTextureView(target_, TextureViewDescriptor{"secondView"}));
  EXPECT_THAT(encoder_->beginRenderPass(RenderPassDescriptor{
                  "aliasedPass",
                  {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store},
                   RenderPassColorAttachment{secondView, LoadOp::Load, StoreOp::Store}}}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("multiple color attachments")));
}

TEST_F(CommandEncoderTests, SamplingAnotherViewOfAnActiveAttachmentFails) {
  // The alias is rejected when the draw uses the group, not when the group is bound: binding is
  // not using, and the encoder must not be poisoned by a group the eventual pipeline never reads.
  const Texture texture = GetResultOrFail(device_.createTexture(
      TextureDescriptor{"feedback", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                        TextureUsage::RenderAttachment | TextureUsage::Sampled}));
  const TextureView attachmentView =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"attachment"}));
  const TextureView sampledView =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"sampled"}));
  const BindGroupLayout layout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "feedbackLayout",
          {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat}}}));
  const BindGroup group = GetResultOrFail(device_.createBindGroup(BindGroupDescriptor{
      "feedbackGroup", layout, {BindGroupEntry{0, TextureViewBinding{sampledView}}}}));
  const PipelineLayout feedbackPipelineLayout = GetResultOrFail(
      device_.createPipelineLayout(PipelineLayoutDescriptor{"feedbackPipelineLayout", {layout}}));
  RenderPipelineDescriptor pipelineDescriptor = solidPipelineDescriptor();
  pipelineDescriptor.layout = feedbackPipelineLayout;
  const RenderPipeline feedbackPipeline =
      GetResultOrFail(device_.createRenderPipeline(pipelineDescriptor));

  RenderPassEncoder* pass = GetResultOrFail(encoder_->beginRenderPass(RenderPassDescriptor{
      "feedbackPass", {RenderPassColorAttachment{attachmentView, LoadOp::Clear, StoreOp::Store}}}));
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setPipeline(feedbackPipeline), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, group), IsOk());
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
  EXPECT_THAT(pass->draw(3), IsGpuErrorWithMessage(GpuErrorType::UsageMismatch,
                                                   HasSubstr("active color attachment")));
}

TEST_F(CommandEncoderTests, AGroupOutsideThePipelineLayoutMayReferenceAnAttachment) {
  // A group bound at an index the active pipeline layout does not declare is never read, so it
  // cannot alias anything. Rejecting it would fail an ordinary pre-bind or replace sequence.
  const Texture texture = GetResultOrFail(device_.createTexture(
      TextureDescriptor{"feedback", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                        TextureUsage::RenderAttachment | TextureUsage::Sampled}));
  const TextureView attachmentView =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"attachment"}));
  const TextureView sampledView =
      GetResultOrFail(device_.createTextureView(texture, TextureViewDescriptor{"sampled"}));
  const BindGroupLayout unusedLayout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "unusedLayout",
          {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat}}}));
  const BindGroup unusedGroup = GetResultOrFail(device_.createBindGroup(BindGroupDescriptor{
      "unusedGroup", unusedLayout, {BindGroupEntry{0, TextureViewBinding{sampledView}}}}));

  RenderPassEncoder* pass = GetResultOrFail(encoder_->beginRenderPass(RenderPassDescriptor{
      "feedbackPass", {RenderPassColorAttachment{attachmentView, LoadOp::Clear, StoreOp::Store}}}));
  ASSERT_NE(pass, nullptr);
  // pipeline_ declares one bind group layout, so index 1 is outside what the draw reads.
  EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
  EXPECT_THAT(pass->setBindGroup(1, unusedGroup), IsOk());
  EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
  EXPECT_THAT(pass->draw(3), IsOk());
  EXPECT_THAT(pass->end(), IsOk());
}

TEST_F(CommandEncoderTests, CopyTextureToBufferRejectsMisalignedOffset) {
  EXPECT_THAT(encoder_->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readbackBuffer_,
                                            TexelCopyBufferLayout{2, 256, 4}, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("not aligned to the 4-byte texel size")));
}

TEST_F(CommandEncoderTests, SubmitNullCommandBufferFails) {
  EXPECT_THAT(device_.submit(CommandBuffer()), IsGpuError(GpuErrorType::InvalidHandle));
}

/// Texture-to-texture copy tests, on top of the solid-fill fixture: `target_` is the 4x4
/// RGBA8Unorm CopySrc source; each test creates the destination it needs.
class CopyTextureToTextureTests : public CommandEncoderTests {
protected:
  Texture createDestination(Extent2d size = Extent2d{4, 4},
                            TextureFormat format = TextureFormat::RGBA8Unorm,
                            TextureUsage usage = TextureUsage::CopyDst) {
    return GetResultOrFail(
        device_.createTexture(TextureDescriptor{"copyDst", size, format, usage}));
  }
};

TEST_F(CopyTextureToTextureTests, WholeRectCopyEncodesAndSubmits) {
  const Texture destination = createDestination();
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{4, 4}), IsOk());

  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());
}

TEST_F(CopyTextureToTextureTests, SmallerThanBothExtentsIsAccepted) {
  const Texture destination = createDestination(Extent2d{2, 4});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{2, 2}), IsOk());
}

TEST_F(CopyTextureToTextureTests, DuringPassFails) {
  const Texture destination = createDestination();
  beginPass();
  EXPECT_THAT(
      encoder_->copyTextureToTexture(target_, destination, Extent2d{4, 4}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("not allowed inside a pass")));
}

TEST_F(CopyTextureToTextureTests, RejectsFormatMismatch) {
  const Texture bgraDestination =
      createDestination(Extent2d{4, 4}, TextureFormat::BGRA8Unorm, TextureUsage::CopyDst);
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, bgraDestination, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("different formats (RGBA8Unorm vs BGRA8Unorm)")));
}

TEST_F(CopyTextureToTextureTests, RejectsMissingCopySrc) {
  const Texture noCopySrc = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "noCopySrc", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const Texture destination = createDestination();
  EXPECT_THAT(encoder_->copyTextureToTexture(noCopySrc, destination, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("CopySrc")));
}

TEST_F(CopyTextureToTextureTests, RejectsMissingCopyDst) {
  const Texture noCopyDst =
      createDestination(Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled);
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, noCopyDst, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("CopyDst")));
}

TEST_F(CopyTextureToTextureTests, RejectsZeroCopySize) {
  const Texture destination = createDestination();
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{4, 0}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("zero dimension")));
}

TEST_F(CopyTextureToTextureTests, RejectsCopyBeyondSourceExtent) {
  const Texture destination = createDestination(Extent2d{8, 8});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{8, 8}),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds,
                                    HasSubstr("source rectangle 8x8 at (0, 0) does not fit")));
}

TEST_F(CopyTextureToTextureTests, RejectsCopyBeyondDestinationExtent) {
  const Texture destination = createDestination(Extent2d{2, 2});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds,
                                    HasSubstr("destination rectangle 4x4 at (0, 0) does not fit")));
}

TEST_F(CopyTextureToTextureTests, SubRectangleCopyIsAcceptedAndSubmits) {
  const Texture destination = createDestination(Extent2d{8, 8});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{2, 2}, Origin2d{2, 1},
                                             Origin2d{5, 6}),
              IsOk());

  auto finished = encoder_->finish();
  ASSERT_THAT(finished, HasResult());
  EXPECT_THAT(device_.submit(std::move(finished).result()), HasResult());
}

TEST_F(CopyTextureToTextureTests, ACopyFlushWithTheSourceEdgeIsAccepted) {
  // The far edge check is inclusive: a rectangle ending exactly on the texture edge is in bounds,
  // and rejecting it would make the last row and column of every texture uncopyable.
  const Texture destination = createDestination(Extent2d{8, 8});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{2, 2}, Origin2d{2, 2},
                                             Origin2d{6, 6}),
              IsOk());
}

TEST_F(CopyTextureToTextureTests, RejectsSourceOriginThatPushesTheRectPastTheEdge) {
  const Texture destination = createDestination(Extent2d{8, 8});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{4, 4}, Origin2d{1, 0}),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds,
                                    HasSubstr("source rectangle 4x4 at (1, 0) does not fit "
                                              "source \"target\" size 4x4")));
}

TEST_F(CopyTextureToTextureTests, RejectsDestinationOriginThatPushesTheRectPastTheEdge) {
  const Texture destination = createDestination(Extent2d{4, 4});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{4, 4}, Origin2d{},
                                             Origin2d{0, 1}),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds,
                                    HasSubstr("destination rectangle 4x4 at (0, 1) does not fit "
                                              "destination \"copyDst\" size 4x4")));
}

TEST_F(CopyTextureToTextureTests, RejectsAnOriginThatWouldOverflowThirtyTwoBitArithmetic) {
  // origin + extent is computed in 64 bits: in 32-bit arithmetic this pair wraps to a small
  // in-bounds far edge, and the copy would be accepted and then read out of the texture.
  const Texture destination = createDestination(Extent2d{8, 8});
  EXPECT_THAT(encoder_->copyTextureToTexture(target_, destination, Extent2d{4, 4},
                                             Origin2d{std::numeric_limits<uint32_t>::max() - 1, 0}),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("does not fit source")));
}

TEST_F(CopyTextureToTextureTests, RejectsNullSourceHandle) {
  const Texture destination = createDestination();
  EXPECT_THAT(encoder_->copyTextureToTexture(Texture(), destination, Extent2d{4, 4}),
              IsGpuError(GpuErrorType::InvalidHandle));
}

TEST_F(CopyTextureToTextureTests, RejectsSelfCopy) {
  // WebGPU forbids copyTextureToTexture where source and destination are the same texture.
  EXPECT_THAT(
      encoder_->copyTextureToTexture(target_, target_, Extent2d{4, 4}),
      IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                            HasSubstr("source and destination are the same texture \"target\"")));
}

TEST_F(CommandEncoderTests, InvalidAttachmentListsCannotProduceACommandBuffer) {
  for (size_t count : {size_t{0}, size_t{kMaxColorAttachments + 1}}) {
    SCOPED_TRACE(count);
    auto encoder = GetResultOrFail(device_.createCommandEncoder());
    auto descriptor = passDescriptor();
    descriptor.colorAttachments.resize(count, descriptor.colorAttachments.front());
    const auto error = encoder->beginRenderPass(descriptor);
    ASSERT_TRUE(error.hasError());
    EXPECT_EQ(error.error().type,
              count == 0 ? GpuErrorType::InvalidDescriptor : GpuErrorType::LimitExceeded);
    EXPECT_THAT(encoder->finish(),
                IsGpuErrorWithMessage(error.error().type, Eq(error.error().message)));
  }
  EXPECT_EQ(device_.lastSubmittedSerial(), 0u);
}

TEST_F(CommandEncoderTests, MalformedAttachmentOperationsPoisonBeforeRecording) {
  for (bool badLoad : {false, true}) {
    auto encoder = GetResultOrFail(device_.createCommandEncoder());
    auto descriptor = passDescriptor();
    if (badLoad)
      descriptor.colorAttachments[0].loadOp = static_cast<LoadOp>(255);
    else
      descriptor.colorAttachments[0].storeOp = static_cast<StoreOp>(255);
    const auto result = encoder->beginRenderPass(descriptor);
    ASSERT_THAT(result, IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                              HasSubstr(badLoad ? "loadOp" : "storeOp")));
    EXPECT_THAT(encoder->beginRenderPass(passDescriptor()),
                IsGpuErrorWithMessage(result.error().type, Eq(result.error().message)));
    EXPECT_THAT(encoder->finish(), IsGpuError(GpuErrorType::InvalidDescriptor));
  }
}

TEST_F(CommandEncoderTests, NonFiniteClearChannelsCannotReachTheBackend) {
  for (double value :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    for (size_t channel = 0; channel < 4; ++channel) {
      SCOPED_TRACE(channel);
      auto encoder = GetResultOrFail(device_.createCommandEncoder());
      auto descriptor = passDescriptor();
      descriptor.colorAttachments[0].clearColor[channel] = value;
      EXPECT_THAT(encoder->beginRenderPass(descriptor),
                  IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("not finite")));
      EXPECT_THAT(encoder->finish(), IsGpuError(GpuErrorType::InvalidDescriptor));
    }
  }
}

TEST_F(CommandEncoderTests, MultipleAttachmentsMustHaveTheSameExtent) {
  const Texture other = GetResultOrFail(device_.createTexture(
      {"other", {4, 3}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
  const TextureView view = GetResultOrFail(device_.createTextureView(other, {}));
  auto descriptor = passDescriptor();
  descriptor.colorAttachments.push_back({view, LoadOp::Clear, StoreOp::Store, {}});
  EXPECT_THAT(encoder_->beginRenderPass(descriptor),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor, HasSubstr("extent")));
  EXPECT_THAT(encoder_->finish(), IsGpuError(GpuErrorType::InvalidDescriptor));
}

TEST_F(CommandEncoderTests, VertexBindingPastEndPoisonsButExactEndCanBind) {
  auto* pass = beginPass();
  ASSERT_NE(pass, nullptr);
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_, 48), IsOk());
  EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_, 49),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("offsetBytes 49")));
  EXPECT_THAT(encoder_->finish(), IsGpuError(GpuErrorType::OutOfBounds));
}

TEST_F(CommandEncoderTests, TextureReadbackNeedsDestinationUsage) {
  const Buffer wrongUsage =
      GetResultOrFail(device_.createBuffer({"not-copyable", 1024, BufferUsage::Uniform}));
  EXPECT_THAT(encoder_->copyTextureToBuffer({target_}, wrongUsage, {0, 256, 4}, {4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::UsageMismatch, HasSubstr("CopyDst")));
  EXPECT_THAT(encoder_->finish(), IsGpuError(GpuErrorType::UsageMismatch));
}

TEST_F(CommandEncoderTests, ReadbackExtentAndOffsetCannotOverflowOrEscapeTheSource) {
  for (const Extent2d extent : {Extent2d{0, 4}, Extent2d{4, 0}, Extent2d{5, 4}, Extent2d{4, 5}}) {
    auto encoder = GetResultOrFail(device_.createCommandEncoder());
    const auto result =
        encoder->copyTextureToBuffer({target_}, readbackBuffer_, {0, 256, 5}, extent);
    const auto expected = extent.width == 0 || extent.height == 0 ? GpuErrorType::InvalidDescriptor
                                                                  : GpuErrorType::OutOfBounds;
    ASSERT_THAT(result, IsGpuError(expected));
    EXPECT_THAT(encoder->finish(), IsGpuErrorWithMessage(expected, Eq(result.error().message)));
  }
  const uint64_t alignedNearMaximum = std::numeric_limits<uint64_t>::max() - 3;
  EXPECT_THAT(encoder_->copyTextureToBuffer({target_}, readbackBuffer_,
                                            {alignedNearMaximum, 256, 4}, {4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("overflows")));
}

}  // namespace
}  // namespace donner::gpu
