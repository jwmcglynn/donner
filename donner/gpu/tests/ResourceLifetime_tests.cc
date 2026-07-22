/// @file
/// Resource lifetime and submission model tests: deferred destruction by submission serial, slot
/// retirement while work is in flight, and submit-time re-validation of recorded command
/// resources (design 0053 "Core types and ownership", "Command model").

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::AllOf;
using testing::ElementsAre;
using testing::Eq;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Ne;
using testing::Not;

namespace donner::gpu {
namespace {

/**
 * Test backend whose completion serial is advanced manually, modeling a GPU that executes
 * submissions asynchronously. Records every backend release in order so tests can assert exactly
 * when deferred destruction reaches the backend.
 */
class ManualCompletionDevice final : public Device {
public:
  /// Marks every submission up to \p serial as executed by the fake GPU.
  /// @param serial Serial to complete through.
  void completeUpTo(uint64_t serial) { completedSerial_ = serial; }

  /// Serial of the most recent manually completed submission.
  uint64_t completedSerial() const override { return completedSerial_; }

  /// Backend releases observed, as `<name>#<slot>` strings in release order.
  const std::vector<std::string>& backendReleases() const { return backendReleases_; }

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
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override {
    backendReleases_.push_back(std::string(resourceName) + "#" + std::to_string(slotIndex));
  }
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&) override {
    return OkStatus();
  }
  Status onSubmit(uint64_t, uint32_t, std::span<const Command>) override { return OkStatus(); }

private:
  uint64_t completedSerial_ = 0;
  std::vector<std::string> backendReleases_;
};

/// Deferred-destruction tests: a readback copy keeps its source texture and destination buffer
/// referenced by an in-flight submission.
class DeferredDestructionTests : public testing::Test {
protected:
  Texture createSourceTexture() {
    return GetResultOrFail(device_.createTexture(TextureDescriptor{
        "source", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc}));
  }

  Buffer createReadbackBuffer() {
    return GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"readback", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));
  }

  /// Submits one copy from \p texture to \p buffer and returns the submission serial.
  uint64_t submitCopy(const Texture& texture, const Buffer& buffer) {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    Status copyStatus = encoder->copyTextureToBuffer(
        TexelCopyTextureInfo{texture}, buffer, TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4});
    EXPECT_THAT(copyStatus, IsOk());
    CommandBuffer commands = GetResultOrFail(encoder->finish());
    return GetResultOrFail(device_.submit(std::move(commands)));
  }

  ManualCompletionDevice device_;
};

TEST_F(DeferredDestructionTests, DestroyOfUnsubmittedResourceReleasesImmediately) {
  Buffer buffer = createReadbackBuffer();
  const uint32_t slot = buffer.slotIndex();
  EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsOk());
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(slot)));
}

TEST_F(DeferredDestructionTests, DestroyWhileInFlightDefersBackendRelease) {
  Texture texture = createSourceTexture();
  Buffer buffer = createReadbackBuffer();
  const uint32_t bufferSlot = buffer.slotIndex();
  const uint64_t serial = submitCopy(texture, buffer);

  // The destroy retires the handle immediately but must not release the backend object while
  // the submission is incomplete - not even through an explicit poll.
  EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsOk());
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), IsEmpty());

  // The retired slot must not be reused while the backend object is still alive.
  const Buffer unrelated = createReadbackBuffer();
  EXPECT_THAT(unrelated.slotIndex(), Ne(bufferSlot));

  // Completion plus poll releases the backend object and recycles the slot.
  device_.completeUpTo(serial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(bufferSlot)));
  const Buffer recycled = createReadbackBuffer();
  EXPECT_THAT(recycled.slotIndex(), Eq(bufferSlot));
}

TEST_F(DeferredDestructionTests, DestroyAfterCompletionReleasesImmediately) {
  Texture texture = createSourceTexture();
  Buffer buffer = createReadbackBuffer();
  const uint32_t bufferSlot = buffer.slotIndex();
  const uint64_t serial = submitCopy(texture, buffer);

  device_.completeUpTo(serial);
  EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsOk());
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(bufferSlot)));
}

TEST_F(DeferredDestructionTests, RaiiDropWhileInFlightAlsoDefers) {
  Texture texture = createSourceTexture();
  uint64_t serial = 0;
  uint32_t bufferSlot = 0;
  {
    const Buffer buffer = createReadbackBuffer();
    bufferSlot = buffer.slotIndex();
    serial = submitCopy(texture, buffer);
  }

  // The RAII release at scope exit deferred: the backend object survives until completion.
  EXPECT_THAT(device_.backendReleases(), IsEmpty());
  device_.completeUpTo(serial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(bufferSlot)));
}

TEST_F(DeferredDestructionTests, BothCopyOperandsDeferAndReleaseInDestructionOrder) {
  Texture texture = createSourceTexture();
  Buffer buffer = createReadbackBuffer();
  const std::string textureId = "texture#" + std::to_string(texture.slotIndex());
  const std::string bufferId = "buffer#" + std::to_string(buffer.slotIndex());
  const uint64_t serial = submitCopy(texture, buffer);

  EXPECT_THAT(device_.destroyTexture(std::move(texture)), IsOk());
  EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsOk());
  EXPECT_THAT(device_.backendReleases(), IsEmpty());

  device_.completeUpTo(serial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre(textureId, bufferId));
}

TEST_F(DeferredDestructionTests, LaterSubmissionExtendsDeferral) {
  Texture texture = createSourceTexture();
  Buffer buffer = createReadbackBuffer();
  const uint32_t bufferSlot = buffer.slotIndex();
  const uint64_t firstSerial = submitCopy(texture, buffer);
  const uint64_t secondSerial = submitCopy(texture, buffer);
  ASSERT_THAT(secondSerial, Eq(firstSerial + 1));

  EXPECT_THAT(device_.destroyBuffer(std::move(buffer)), IsOk());

  // Completing only the first submission is not enough: the second still references the buffer.
  device_.completeUpTo(firstSerial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), IsEmpty());

  device_.completeUpTo(secondSerial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(bufferSlot)));
}

/// Submit-time staleness tests: the full solid-fill scene is recorded, then one resource is
/// destroyed between finish() and submit(). Submission must fail closed naming the resource.
class SubmitStalenessTests : public testing::Test {
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
    pipeline_ = GetResultOrFail(device_.createRenderPipeline(RenderPipelineDescriptor{
        "solid", pipelineLayout_,
        VertexState{
            shader_,
            "vsMain",
            {VertexBufferLayout{
                8, VertexStepMode::Vertex, {VertexAttribute{VertexFormat::Float32x2, 0, 0}}}}},
        FragmentState{shader_, "fsMain", {ColorTargetState{TextureFormat::RGBA8Unorm}}}}));
  }

  /// Records the full pass plus readback copy and returns the finished command buffer.
  CommandBuffer recordScene() {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
        "mainPass",
        {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store, {0, 0, 0.5, 1}}}}));
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->draw(6), IsOk());
    EXPECT_THAT(pass->end(), IsOk());
    EXPECT_THAT(encoder->copyTextureToBuffer(TexelCopyTextureInfo{target_}, readbackBuffer_,
                                             TexelCopyBufferLayout{0, 256, 4}, Extent2d{4, 4}),
                IsOk());
    return GetResultOrFail(encoder->finish());
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
};

TEST_F(SubmitStalenessTests, IntactSceneSubmits) {
  CommandBuffer commands = recordScene();
  EXPECT_THAT(device_.submit(std::move(commands)), HasResult());
}

TEST_F(SubmitStalenessTests, DestroyedVertexBufferRejectsSubmit) {
  CommandBuffer commands = recordScene();
  ASSERT_THAT(device_.destroyBuffer(std::move(vertexBuffer_)), IsOk());
  EXPECT_THAT(
      device_.submit(std::move(commands)),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                            AllOf(HasSubstr("setVertexBuffer"), HasSubstr("destroyed buffer"))));
}

TEST_F(SubmitStalenessTests, DestroyedPipelineRejectsSubmit) {
  CommandBuffer commands = recordScene();
  ASSERT_THAT(device_.destroyRenderPipeline(std::move(pipeline_)), IsOk());
  EXPECT_THAT(
      device_.submit(std::move(commands)),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("destroyed renderPipeline")));
}

TEST_F(SubmitStalenessTests, DestroyedBindGroupRejectsSubmit) {
  CommandBuffer commands = recordScene();
  ASSERT_THAT(device_.destroyBindGroup(std::move(bindGroup_)), IsOk());
  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("destroyed bindGroup")));
}

TEST_F(SubmitStalenessTests, DestroyedBindGroupLayoutRejectsSubmit) {
  CommandBuffer commands = recordScene();
  // The bind group stays alive; only the layout it was created against dies. The layout is a
  // transitive dependency of the group (backends read it at encode time), so submission must
  // fail closed.
  ASSERT_THAT(device_.destroyBindGroupLayout(std::move(bindGroupLayout_)), IsOk());
  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    AllOf(HasSubstr("bind group \"solidUniforms\""),
                                          HasSubstr("destroyed bindGroupLayout"))));
}

TEST_F(SubmitStalenessTests, RecycledBindGroupLayoutSlotRejectsSubmit) {
  CommandBuffer commands = recordScene();
  const uint32_t layoutSlot = bindGroupLayout_.slotIndex();
  ASSERT_THAT(device_.destroyBindGroupLayout(std::move(bindGroupLayout_)), IsOk());

  // A different layout reuses the freed slot; the group's recorded layout identity must not
  // alias it (the replacement's entries would misbind stages at encode time).
  const BindGroupLayout replacement =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "replacementLayout",
          {BindGroupLayoutEntry{5, ShaderStage::Fragment, BindingType::FilteringSampler}}}));
  ASSERT_THAT(replacement.slotIndex(), Eq(layoutSlot));

  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                                    AllOf(HasSubstr("bind group \"solidUniforms\""),
                                          HasSubstr("destroyed bindGroupLayout"))));
}

TEST_F(SubmitStalenessTests, DestroyedBindGroupEntryBufferRejectsSubmit) {
  CommandBuffer commands = recordScene();
  // The bind group itself stays alive; only the buffer one of its entries references dies.
  ASSERT_THAT(device_.destroyBuffer(std::move(uniformBuffer_)), IsOk());
  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(
                  GpuErrorType::InvalidHandle,
                  AllOf(HasSubstr("bind group \"solidUniforms\""), HasSubstr("destroyed buffer"))));
}

TEST_F(SubmitStalenessTests, DestroyedAttachmentViewRejectsSubmit) {
  CommandBuffer commands = recordScene();
  ASSERT_THAT(device_.destroyTextureView(std::move(targetView_)), IsOk());
  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(
                  GpuErrorType::InvalidHandle,
                  AllOf(HasSubstr("render pass attachment"), HasSubstr("destroyed textureView"))));
}

TEST_F(SubmitStalenessTests, DestroyedAttachmentTextureRejectsSubmit) {
  CommandBuffer commands = recordScene();
  // The view stays alive; the texture behind it dies. The copy source also dies with it, but
  // the attachment walk hits the identity first - either way the submit must fail closed.
  ASSERT_THAT(device_.destroyTexture(std::move(target_)), IsOk());
  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("destroyed texture")));
}

TEST_F(SubmitStalenessTests, DestroyedCopyDestinationRejectsSubmit) {
  CommandBuffer commands = recordScene();
  ASSERT_THAT(device_.destroyBuffer(std::move(readbackBuffer_)), IsOk());
  EXPECT_THAT(
      device_.submit(std::move(commands)),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, AllOf(HasSubstr("copyTextureToBuffer"),
                                                               HasSubstr("destroyed buffer"))));
}

TEST_F(SubmitStalenessTests, RejectedSubmitDoesNotBurnSerial) {
  CommandBuffer staleCommands = recordScene();
  ASSERT_THAT(device_.destroyBuffer(std::move(vertexBuffer_)), IsOk());
  ASSERT_THAT(device_.submit(std::move(staleCommands)), IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(uint64_t{0}));

  // A subsequent valid submission takes serial 1: rejected submissions consume no serial.
  vertexBuffer_ = GetResultOrFail(device_.createBuffer(
      BufferDescriptor{"vertices2", 48, BufferUsage::Vertex | BufferUsage::CopyDst}));
  CommandBuffer freshCommands = recordScene();
  EXPECT_THAT(GetResultOrFail(device_.submit(std::move(freshCommands))), Eq(uint64_t{1}));
}

TEST_F(SubmitStalenessTests, DroppedCommandBufferReleasesSlotWithoutBackendNotification) {
  uint32_t droppedSlot = 0;
  {
    const CommandBuffer dropped = recordScene();
    droppedSlot = dropped.slotIndex();
  }

  // The RAII release freed the slot (the next finished buffer reuses it) and command buffers
  // have no backend object, so no destroy line was recorded.
  const CommandBuffer next = recordScene();
  EXPECT_THAT(next.slotIndex(), Eq(droppedSlot));
  EXPECT_THAT(device_.serialize(), Not(HasSubstr("destroy commandBuffer")));
}

TEST_F(SubmitStalenessTests, DoubleSubmitFailsClosed) {
  CommandBuffer commands = recordScene();
  ASSERT_THAT(device_.submit(std::move(commands)), HasResult());

  // The first submit consumed the handle; the second sees a null handle.
  EXPECT_THAT(device_.submit(std::move(commands)),  // NOLINT(bugprone-use-after-move)
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("null")));
}

TEST_F(SubmitStalenessTests, ForgedConsumedCommandBufferIsStale) {
  CommandBuffer commands = recordScene();
  const uint32_t slot = commands.slotIndex();
  const uint32_t generation = commands.generation();
  const uint64_t deviceId = commands.deviceId();
  ASSERT_THAT(device_.submit(std::move(commands)), HasResult());

  // A forged handle carrying the consumed generation must resolve as stale, not alias a later
  // command buffer occupying the slot.
  CommandBuffer forged = CommandBuffer::CreateForBackend(slot, generation, deviceId);
  EXPECT_THAT(device_.submit(std::move(forged)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("stale")));
}

TEST_F(SubmitStalenessTests, DestroyedSamplerEntryRejectsSubmit) {
  // A second scene with a sampled-texture bind group, so the sampler entry branch is covered.
  Texture sampled = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "sampled", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::Sampled}));
  const TextureView sampledView =
      GetResultOrFail(device_.createTextureView(sampled, TextureViewDescriptor{"sampledView"}));
  Sampler sampler = GetResultOrFail(device_.createSampler(SamplerDescriptor{"linear"}));
  const BindGroupLayout textureLayout =
      GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
          "textureBindings",
          {BindGroupLayoutEntry{0, ShaderStage::Fragment, BindingType::SampledTexture2dFloat},
           BindGroupLayoutEntry{1, ShaderStage::Fragment, BindingType::FilteringSampler}}}));
  const PipelineLayout texturePipelineLayout = GetResultOrFail(
      device_.createPipelineLayout(PipelineLayoutDescriptor{"textureLayout", {textureLayout}}));
  const BindGroup textureGroup = GetResultOrFail(device_.createBindGroup(
      BindGroupDescriptor{"textureGroup",
                          textureLayout,
                          {BindGroupEntry{0, TextureViewBinding{sampledView}},
                           BindGroupEntry{1, SamplerBinding{sampler}}}}));
  const RenderPipeline texturePipeline =
      GetResultOrFail(device_.createRenderPipeline(RenderPipelineDescriptor{
          "textured", texturePipelineLayout,
          VertexState{
              shader_,
              "vsMain",
              {VertexBufferLayout{
                  8, VertexStepMode::Vertex, {VertexAttribute{VertexFormat::Float32x2, 0, 0}}}}},
          FragmentState{shader_, "fsMain", {ColorTargetState{TextureFormat::RGBA8Unorm}}}}));

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
  RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
      "texturedPass",
      {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store, {0, 0, 0, 1}}}}));
  ASSERT_THAT(pass->setPipeline(texturePipeline), IsOk());
  ASSERT_THAT(pass->setBindGroup(0, textureGroup), IsOk());
  ASSERT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
  ASSERT_THAT(pass->draw(3), IsOk());
  ASSERT_THAT(pass->end(), IsOk());
  CommandBuffer commands = GetResultOrFail(encoder->finish());

  ASSERT_THAT(device_.destroySampler(std::move(sampler)), IsOk());
  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(
                  GpuErrorType::InvalidHandle,
                  AllOf(HasSubstr("bind group \"textureGroup\""), HasSubstr("destroyed sampler"))));
}

}  // namespace
}  // namespace donner::gpu
