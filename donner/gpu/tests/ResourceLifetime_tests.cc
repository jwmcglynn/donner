/// @file
/// Resource lifetime and submission model tests: deferred destruction by submission serial, slot
/// retirement while work is in flight, explicit release of a handle's backend allocation, bounded
/// waits for a submission serial, and submit-time re-validation of recorded command resources.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
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
using testing::IsFalse;
using testing::IsTrue;
using testing::Ne;
using testing::Not;

namespace donner::gpu {
namespace {

/**
 * Test backend whose completion serial is advanced manually, modeling a GPU that executes
 * submissions asynchronously. Records every backend release in order so tests can assert exactly
 * when deferred destruction reaches the backend.
 */
class ManualCompletionDevice : public Device {
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
  Status onCreateComputePipeline(uint32_t, const ComputePipelineDescriptor&) override {
    return OkStatus();
  }
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override {
    backendReleases_.push_back(std::string(resourceName) + "#" + std::to_string(slotIndex));
  }
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&, const Origin2d&) override {
    return OkStatus();
  }
  Status onSubmit(uint64_t, std::span<const SubmittedCommandBuffer>) override { return OkStatus(); }

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

TEST_F(DeferredDestructionTests, TextureCopyOperandsDeferAndReleaseInDestructionOrder) {
  Texture source = createSourceTexture();
  Texture destination = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "destination", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  const std::string sourceId = "texture#" + std::to_string(source.slotIndex());
  const std::string destinationId = "texture#" + std::to_string(destination.slotIndex());

  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
  ASSERT_THAT(encoder->copyTextureToTexture(source, destination, Extent2d{4, 4}), IsOk());
  CommandBuffer commands = GetResultOrFail(encoder->finish());
  const uint64_t serial = GetResultOrFail(device_.submit(std::move(commands)));

  // Both copy operands are pinned by the in-flight submission: destruction is deferred.
  EXPECT_THAT(device_.destroyTexture(std::move(source)), IsOk());
  EXPECT_THAT(device_.destroyTexture(std::move(destination)), IsOk());
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), IsEmpty());

  device_.completeUpTo(serial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre(sourceId, destinationId));
}

/// Deferred-destruction tests for an index buffer referenced by an in-flight indexed draw.
class IndexedDrawDeferredDestructionTests : public testing::Test {
protected:
  void SetUp() override {
    target_ = GetResultOrFail(device_.createTexture(TextureDescriptor{
        "target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::RenderAttachment}));
    targetView_ =
        GetResultOrFail(device_.createTextureView(target_, TextureViewDescriptor{"targetView"}));
    vertexBuffer_ = GetResultOrFail(
        device_.createBuffer(BufferDescriptor{"vertices", 48, BufferUsage::Vertex}));
    // The layout and shader stay alive for the whole test: dropping them here would release them
    // to the backend at once and pollute the deferred-release expectations below.
    pipelineLayout_ =
        GetResultOrFail(device_.createPipelineLayout(PipelineLayoutDescriptor{"empty", {}}));
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

  Buffer createIndexBuffer() {
    return GetResultOrFail(
        device_.createBuffer(BufferDescriptor{"indices", 24, BufferUsage::Index}));
  }

  /// Submits one indexed draw reading \p indices and returns the submission serial.
  uint64_t submitIndexedDraw(const Buffer& indices) {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
        "pass", {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store, {}}}}));
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->setIndexBuffer(indices, IndexFormat::Uint16), IsOk());
    EXPECT_THAT(pass->drawIndexed(12), IsOk());
    EXPECT_THAT(pass->end(), IsOk());
    CommandBuffer commands = GetResultOrFail(encoder->finish());
    return GetResultOrFail(device_.submit(std::move(commands)));
  }

  ManualCompletionDevice device_;
  Texture target_;
  TextureView targetView_;
  Buffer vertexBuffer_;
  PipelineLayout pipelineLayout_;
  ShaderModule shader_;
  RenderPipeline pipeline_;
};

TEST_F(IndexedDrawDeferredDestructionTests, DestroyWhileInFlightDefersBackendRelease) {
  Buffer indices = createIndexBuffer();
  const uint32_t slot = indices.slotIndex();
  const uint64_t serial = submitIndexedDraw(indices);

  // The handle retires now, but the backend object outlives the submission that reads it, and
  // the retired slot is not handed out again while that object is alive.
  EXPECT_THAT(device_.destroyBuffer(std::move(indices)), IsOk());
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), IsEmpty());
  const Buffer unrelated = createIndexBuffer();
  EXPECT_THAT(unrelated.slotIndex(), Ne(slot));

  device_.completeUpTo(serial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(slot)));
}

TEST_F(IndexedDrawDeferredDestructionTests, RaiiDropWhileInFlightAlsoDefers) {
  uint64_t serial = 0;
  uint32_t slot = 0;
  {
    const Buffer indices = createIndexBuffer();
    slot = indices.slotIndex();
    serial = submitIndexedDraw(indices);
  }
  EXPECT_THAT(device_.backendReleases(), IsEmpty());
  device_.completeUpTo(serial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(slot)));
}

TEST_F(IndexedDrawDeferredDestructionTests, LaterSubmissionExtendsDeferral) {
  Buffer indices = createIndexBuffer();
  const uint32_t slot = indices.slotIndex();
  const uint64_t firstSerial = submitIndexedDraw(indices);
  const uint64_t secondSerial = submitIndexedDraw(indices);
  ASSERT_THAT(secondSerial, Eq(firstSerial + 1));
  EXPECT_THAT(device_.destroyBuffer(std::move(indices)), IsOk());

  device_.completeUpTo(firstSerial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), IsEmpty());
  device_.completeUpTo(secondSerial);
  device_.poll();
  EXPECT_THAT(device_.backendReleases(), ElementsAre("buffer#" + std::to_string(slot)));
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
    indexBuffer_ =
        GetResultOrFail(device_.createBuffer(BufferDescriptor{"indices", 24, BufferUsage::Index}));

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

  /// Records the pass with an indexed draw instead of a plain one.
  CommandBuffer recordIndexedScene() {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
        "mainPass",
        {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store, {0, 0, 0.5, 1}}}}));
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
    EXPECT_THAT(pass->drawIndexed(12), IsOk());
    EXPECT_THAT(pass->end(), IsOk());
    return GetResultOrFail(encoder->finish());
  }

  RecordingDevice device_;
  Texture target_;
  TextureView targetView_;
  Buffer vertexBuffer_;
  Buffer uniformBuffer_;
  Buffer readbackBuffer_;
  Buffer indexBuffer_;
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

TEST_F(SubmitStalenessTests, IntactIndexedSceneSubmits) {
  CommandBuffer commands = recordIndexedScene();
  EXPECT_THAT(device_.submit(std::move(commands)), HasResult());
}

TEST_F(SubmitStalenessTests, DestroyedIndexBufferRejectsSubmit) {
  CommandBuffer commands = recordIndexedScene();
  ASSERT_THAT(device_.destroyBuffer(std::move(indexBuffer_)), IsOk());
  EXPECT_THAT(
      device_.submit(std::move(commands)),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle,
                            AllOf(HasSubstr("setIndexBuffer"), HasSubstr("destroyed buffer"))));
}

TEST_F(SubmitStalenessTests, RecycledIndexBufferSlotRejectsSubmit) {
  CommandBuffer commands = recordIndexedScene();
  const uint32_t slot = indexBuffer_.slotIndex();
  ASSERT_THAT(device_.destroyBuffer(std::move(indexBuffer_)), IsOk());
  // A new index buffer in the same slot must not satisfy the recorded (slot, generation).
  const Buffer replacement = GetResultOrFail(
      device_.createBuffer(BufferDescriptor{"replacement", 24, BufferUsage::Index}));
  ASSERT_THAT(replacement.slotIndex(), Eq(slot));
  EXPECT_THAT(device_.submit(std::move(commands)),
              IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, HasSubstr("setIndexBuffer")));
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

/// Fixture recording a texture-to-texture copy so each operand can be destroyed between
/// finish() and submit(), mirroring the copyTextureToBuffer staleness coverage.
class TextureCopySubmitStalenessTests : public SubmitStalenessTests {
protected:
  void SetUp() override {
    SubmitStalenessTests::SetUp();
    copyDestination_ = GetResultOrFail(device_.createTexture(TextureDescriptor{
        "copyDestination", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  }

  /// Records just the texture-to-texture copy and returns the finished command buffer.
  CommandBuffer recordTextureCopy() {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    EXPECT_THAT(encoder->copyTextureToTexture(target_, copyDestination_, Extent2d{4, 4}), IsOk());
    return GetResultOrFail(encoder->finish());
  }

  Texture copyDestination_;
};

TEST_F(TextureCopySubmitStalenessTests, IntactCopySubmits) {
  CommandBuffer commands = recordTextureCopy();
  EXPECT_THAT(device_.submit(std::move(commands)), HasResult());
}

TEST_F(TextureCopySubmitStalenessTests, DestroyedCopySourceRejectsSubmit) {
  CommandBuffer commands = recordTextureCopy();
  ASSERT_THAT(device_.destroyTexture(std::move(target_)), IsOk());
  EXPECT_THAT(
      device_.submit(std::move(commands)),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, AllOf(HasSubstr("copyTextureToTexture"),
                                                               HasSubstr("destroyed texture"))));
}

TEST_F(TextureCopySubmitStalenessTests, DestroyedCopyDestinationRejectsSubmit) {
  CommandBuffer commands = recordTextureCopy();
  ASSERT_THAT(device_.destroyTexture(std::move(copyDestination_)), IsOk());
  EXPECT_THAT(
      device_.submit(std::move(commands)),
      IsGpuErrorWithMessage(GpuErrorType::InvalidHandle, AllOf(HasSubstr("copyTextureToTexture"),
                                                               HasSubstr("destroyed texture"))));
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

// == Backing lifetime and bounded submission waits ============================================

/// A backend that can also name memory it did not allocate, so the two answers
/// \ref Device::ownsTextureBacking separates both exist here.
class BorrowingDevice final : public ManualCompletionDevice {
public:
  /// Texture slots whose allocation belongs to someone else.
  std::set<uint32_t> borrowedSlots;
  /// Texture slots whose allocation this device was asked to release, in order.
  std::vector<uint32_t> releasedTextureSlots;
  /// Buffer slots whose allocation this device was asked to release, in order.
  std::vector<uint32_t> releasedBufferSlots;

protected:
  bool onOwnsTextureBacking(uint32_t slotIndex) const override {
    return !borrowedSlots.contains(slotIndex);
  }
  void onDestroyTextureBacking(uint32_t slotIndex) override {
    releasedTextureSlots.push_back(slotIndex);
  }
  void onDestroyBufferBacking(uint32_t slotIndex) override {
    releasedBufferSlots.push_back(slotIndex);
  }
};

/// A backend that has failed terminally, so no submission of its can ever complete.
class FailedCompletionDevice final : public ManualCompletionDevice {
public:
  /// Number of times the runtime asked this backend to wait.
  int waitCalls = 0;

protected:
  bool onWaitForSerial(uint64_t /*serial*/, double /*timeoutSeconds*/) override {
    ++waitCalls;
    return false;
  }
};

class BackingLifetimeTests : public testing::Test {
protected:
  Texture createTexture(const char* label = "target") {
    return GetResultOrFail(device_.createTexture(TextureDescriptor{
        label, Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc}));
  }

  Buffer createBuffer(const char* label = "readback") {
    return GetResultOrFail(device_.createBuffer(
        BufferDescriptor{label, 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));
  }

  BorrowingDevice device_;
};

TEST_F(BackingLifetimeTests, ReleasesTheAllocationOfTheHandleItDestroys) {
  Texture texture = createTexture();
  const uint32_t slotIndex = texture.slotIndex();
  Buffer buffer = createBuffer();
  const uint32_t bufferSlotIndex = buffer.slotIndex();

  EXPECT_THAT(device_.destroyTextureBacking(std::move(texture)), IsOk());
  EXPECT_THAT(device_.destroyBufferBacking(std::move(buffer)), IsOk());

  EXPECT_THAT(device_.releasedTextureSlots, ElementsAre(slotIndex));
  EXPECT_THAT(device_.releasedBufferSlots, ElementsAre(bufferSlotIndex));
  EXPECT_THAT(device_.backendReleases(), ElementsAre("texture#" + std::to_string(slotIndex),
                                                     "buffer#" + std::to_string(bufferSlotIndex)));
}

TEST_F(BackingLifetimeTests, AStaleHandleReachesNeitherTheRecycledSlotNorItsNewOccupant) {
  Texture first = createTexture("first");
  const uint32_t staleSlotIndex = first.slotIndex();
  const uint32_t staleGeneration = first.generation();
  const uint64_t deviceId = first.deviceId();
  ASSERT_THAT(device_.destroyTextureBacking(std::move(first)), IsOk());
  device_.releasedTextureSlots.clear();

  // The freed slot is handed to the next texture, which is exactly the occupant the stale handle
  // must not reach.
  Texture replacement = createTexture("replacement");
  ASSERT_THAT(replacement.slotIndex(), Eq(staleSlotIndex));

  EXPECT_THAT(device_.destroyTextureBacking(
                  Texture::CreateForBackend(staleSlotIndex, staleGeneration, deviceId)),
              IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.releasedTextureSlots, IsEmpty())
      << "a stale handle must not release the allocation of the slot's new occupant";
  EXPECT_THAT(device_.textureExtent(replacement), HasResult())
      << "the replacement must still be live after the stale destroy was refused";

  EXPECT_THAT(device_.destroyTextureBacking(std::move(replacement)), IsOk());
}

TEST_F(BackingLifetimeTests, RefusesAHandleBelongingToAnotherDevice) {
  BorrowingDevice other;
  Texture foreignTexture = GetResultOrFail(other.createTexture(TextureDescriptor{
      "foreign", Extent2d{4, 4}, TextureFormat::RGBA8Unorm, TextureUsage::CopySrc}));
  Buffer foreignBuffer = GetResultOrFail(other.createBuffer(
      BufferDescriptor{"foreign", 1024, BufferUsage::CopyDst | BufferUsage::MapRead}));

  EXPECT_THAT(device_.destroyTextureBacking(std::move(foreignTexture)),
              IsGpuError(GpuErrorType::DeviceMismatch));
  EXPECT_THAT(device_.destroyBufferBacking(std::move(foreignBuffer)),
              IsGpuError(GpuErrorType::DeviceMismatch));

  EXPECT_THAT(device_.releasedTextureSlots, IsEmpty());
  EXPECT_THAT(device_.releasedBufferSlots, IsEmpty());
  EXPECT_THAT(device_.backendReleases(), IsEmpty())
      << "a foreign handle must not destroy anything on the device it was handed to";
  // The handle is consumed either way, so the resource it named goes back to its own device
  // rather than being left behind on this one.
  EXPECT_THAT(other.releasedTextureSlots, IsEmpty());
  EXPECT_THAT(other.backendReleases(), ElementsAre("texture#0", "buffer#0"));
}

TEST_F(BackingLifetimeTests, RefusesASecondDestroyOfAMovedFromHandle) {
  Texture texture = createTexture();
  Buffer buffer = createBuffer();
  ASSERT_THAT(device_.destroyTextureBacking(std::move(texture)), IsOk());
  ASSERT_THAT(device_.destroyBufferBacking(std::move(buffer)), IsOk());
  device_.releasedTextureSlots.clear();
  device_.releasedBufferSlots.clear();

  EXPECT_THAT(texture.isValid(), IsFalse()) << "the destroy contract consumes the handle";
  EXPECT_THAT(device_.destroyTextureBacking(std::move(texture)),
              IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.destroyBufferBacking(std::move(buffer)),
              IsGpuError(GpuErrorType::InvalidHandle));
  EXPECT_THAT(device_.releasedTextureSlots, IsEmpty());
  EXPECT_THAT(device_.releasedBufferSlots, IsEmpty());
}

TEST_F(BackingLifetimeTests, OwnsTextureBackingSeparatesAnAllocationFromARegistration) {
  Texture allocated = createTexture("allocated");
  Texture registered = createTexture("registered");
  device_.borrowedSlots.insert(registered.slotIndex());

  EXPECT_THAT(device_.ownsTextureBacking(allocated), IsTrue());
  EXPECT_THAT(device_.ownsTextureBacking(registered), IsFalse())
      << "a registration names memory whose owner is whoever registered it";

  EXPECT_THAT(device_.destroyTextureBacking(std::move(registered)), IsOk());
  EXPECT_THAT(device_.destroyTextureBacking(std::move(allocated)), IsOk());
}

TEST_F(BackingLifetimeTests, OwnsTextureBackingIsFalseForNullStaleAndForeignHandles) {
  Texture texture = createTexture();
  const Texture stale =
      Texture::CreateForBackend(texture.slotIndex(), texture.generation() + 1, texture.deviceId());
  const Texture foreign =
      Texture::CreateForBackend(texture.slotIndex(), texture.generation(), texture.deviceId() + 1);

  EXPECT_THAT(device_.ownsTextureBacking(Texture()), IsFalse());
  EXPECT_THAT(device_.ownsTextureBacking(stale), IsFalse());
  EXPECT_THAT(device_.ownsTextureBacking(foreign), IsFalse());
  EXPECT_THAT(device_.ownsTextureBacking(texture), IsTrue());

  EXPECT_THAT(device_.destroyTextureBacking(std::move(texture)), IsOk());
}

class SubmissionWaitTests : public testing::Test {
protected:
  /// Submits an empty command stream and returns its serial.
  uint64_t submitEmpty() {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    CommandBuffer commands = GetResultOrFail(encoder->finish());
    return GetResultOrFail(device_.submit(std::move(commands)));
  }

  ManualCompletionDevice device_;
};

TEST_F(SubmissionWaitTests, ReportsAnAlreadyCompletedSerialWithoutSpendingTheBudget) {
  const uint64_t serial = submitEmpty();
  device_.completeUpTo(serial);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(device_.waitForSerial(serial, 30.0), IsTrue());
  EXPECT_THAT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              testing::Lt(1.0))
      << "work that is already done must not be waited on";
}

TEST_F(SubmissionWaitTests, ReturnsFalseWhenTheBudgetElapsesWithTheWorkStillPending) {
  const uint64_t serial = submitEmpty();

  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(device_.waitForSerial(serial, 0.05), IsFalse());
  const double elapsedSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  EXPECT_THAT(elapsedSeconds, testing::Ge(0.05)) << "a timeout means the budget was actually spent";
  EXPECT_THAT(elapsedSeconds, testing::Lt(5.0)) << "and that the wait ended at the budget";
}

TEST_F(SubmissionWaitTests, TreatsABudgetBelowZeroAsNoBudgetRatherThanAnEndlessWait) {
  const uint64_t serial = submitEmpty();

  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(device_.waitForSerial(serial, -1.0), IsFalse());
  EXPECT_THAT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              testing::Lt(1.0));
}

/// The budget belongs to the backend once the runtime hands it over. A backend that knows the
/// work can never complete says so by returning, and the runtime must take that answer rather
/// than asking again until the budget runs out - which is how a backend's fail-fast on a lost
/// device reaches the caller at all.
TEST(SubmissionWait, TakesTheBackendsAnswerWithoutRetryingUntilTheBudgetRunsOut) {
  FailedCompletionDevice device;
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device.createCommandEncoder());
  const uint64_t serial = GetResultOrFail(device.submit(GetResultOrFail(encoder->finish())));

  const auto start = std::chrono::steady_clock::now();
  EXPECT_THAT(device.waitForSerial(serial, 30.0), IsFalse());
  EXPECT_THAT(device.waitCalls, Eq(1)) << "the hook must be asked once, not polled";
  EXPECT_THAT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              testing::Lt(1.0))
      << "and its answer must end the wait rather than start another";
}

TEST_F(SubmissionWaitTests, ClampsABudgetTooLargeForTheClockItIsMeasuredAgainst) {
  const uint64_t serial = submitEmpty();
  device_.completeUpTo(serial);

  // A caller meaning "wait indefinitely" writes a number the clock's own duration cannot hold;
  // converting it unclamped is undefined rather than patient.
  EXPECT_THAT(device_.waitForSerial(serial, 1.0e30), IsTrue());
  EXPECT_THAT(device_.waitForSerial(serial, std::numeric_limits<double>::infinity()), IsTrue());
}

}  // namespace
}  // namespace donner::gpu
