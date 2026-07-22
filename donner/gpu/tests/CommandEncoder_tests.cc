/// @file
/// Encoder state machine tests: pass lifecycle, draw prerequisites, error poisoning, and copy
/// validation.

#include "donner/gpu/CommandEncoder.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::Eq;
using testing::HasSubstr;

namespace donner::gpu {
namespace {

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
  EXPECT_THAT(encoder_->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readbackBuffer_,
                                            TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("inside a render pass")));
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

TEST_F(CommandEncoderTests, CopyTextureToBufferRejectsMisalignedOffset) {
  EXPECT_THAT(encoder_->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readbackBuffer_,
                                            TexelCopyBufferLayout{2, 256, 4}, Extent2d{4, 4}),
              IsGpuErrorWithMessage(GpuErrorType::InvalidDescriptor,
                                    HasSubstr("not aligned to the 4-byte texel size")));
}

TEST_F(CommandEncoderTests, SubmitNullCommandBufferFails) {
  EXPECT_THAT(device_.submit(CommandBuffer()), IsGpuError(GpuErrorType::InvalidHandle));
}

}  // namespace
}  // namespace donner::gpu
