/// @file
/// Device observer tests: every operation the validation layer accepts and the backend accepts is
/// reported once, with the same meaning on every backend, and nothing refused is reported.

#include "donner/gpu/DeviceObserver.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"
#include "donner/gpu/tests/RecordingDeviceObserver.h"

using testing::ElementsAre;
using testing::Eq;
using testing::HasSubstr;

namespace donner::gpu {
namespace {

using tests::ObservedEvents;
using tests::ObservedSubmission;
using tests::RecordingObserver;

/// A step the scripted backend can refuse after validation accepted it.
enum class BackendStep {
  Buffer,        //!< onCreateBuffer.
  Texture,       //!< onCreateTexture.
  BindGroup,     //!< onCreateBindGroup.
  TextureWrite,  //!< onWriteTexture.
  Submission,    //!< onSubmit.
};

/**
 * Test backend with the behaviors that decide what is reported: it can name a texture it was
 * handed instead of allocating one, refuse one step after validation accepted it, report a
 * repacked texture upload size, and submit to its queue on its own.
 */
class ScriptedBackendDevice final : public Device {
public:
  /// Makes the next created texture a registration of memory this device does not own.
  void registerNextTexture() { registerNextTexture_ = true; }

  /// Makes the backend refuse the next \p step it is asked to perform. @param step Step to refuse.
  void refuseNext(BackendStep step) { refuseNext_ = step; }

  /// Makes every accepted texture write report \p byteCount as its uploaded size, the way a
  /// backend that repacks rows before uploading does. @param byteCount Size to report.
  void reportRepackedTextureWrites(uint64_t byteCount) { repackedTextureWriteBytes_ = byteCount; }

  /// Submits to the backend queue without going through \ref submit, the way a backend nudges a
  /// queue so that a pending completion is delivered.
  void submitOnItsOwn() const { notifyObserverOfBackendSubmission(); }

  /// Holds submissions at or after \p serial incomplete until \ref completeAll, the way a
  /// backend still executing them does. @param serial First submission to hold.
  void holdCompletionsFrom(uint64_t serial) { heldFrom_ = serial; }

  /// Lets every held submission complete.
  void completeAll() { heldFrom_.reset(); }

  /// Submissions complete as they are accepted, unless held.
  uint64_t completedSerial() const override {
    return heldFrom_.has_value() ? *heldFrom_ - 1 : lastSubmittedSerial();
  }

protected:
  Status onCreateBuffer(uint32_t, const BufferDescriptor&) override {
    return outcomeOf(BackendStep::Buffer);
  }
  Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor&) override {
    if (Status status = outcomeOf(BackendStep::Texture); status.hasError()) {
      return status;
    }
    if (ownedTextures_.size() <= slotIndex) {
      ownedTextures_.resize(slotIndex + 1, true);
    }
    ownedTextures_[slotIndex] = !std::exchange(registerNextTexture_, false);
    return OkStatus();
  }
  bool onOwnsTextureBacking(uint32_t slotIndex) const override {
    return slotIndex >= ownedTextures_.size() || ownedTextures_[slotIndex];
  }
  Status onCreateTextureView(uint32_t, uint32_t, const TextureViewDescriptor&) override {
    return OkStatus();
  }
  Status onCreateSampler(uint32_t, const SamplerDescriptor&) override { return OkStatus(); }
  Status onCreateBindGroupLayout(uint32_t, const BindGroupLayoutDescriptor&) override {
    return OkStatus();
  }
  Status onCreateBindGroup(uint32_t, const BindGroupDescriptor&) override {
    return outcomeOf(BackendStep::BindGroup);
  }
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
                        const Extent2d&, const Origin2d&) override {
    return outcomeOf(BackendStep::TextureWrite);
  }
  uint64_t onTextureWriteByteCount(std::span<const uint8_t> data) const override {
    return repackedTextureWriteBytes_.value_or(Device::onTextureWriteByteCount(data));
  }
  Status onSubmit(uint64_t, std::span<const SubmittedCommandBuffer>) override {
    return outcomeOf(BackendStep::Submission);
  }

private:
  /// Refuses \p step when it is the one scripted to be refused next. @param step Step performed.
  Status outcomeOf(BackendStep step) {
    if (refuseNext_ != step) {
      return OkStatus();
    }
    refuseNext_.reset();
    return GpuError{GpuErrorType::DeviceLost, "the test backend refused the step"};
  }

  std::vector<bool> ownedTextures_;
  bool registerNextTexture_ = false;
  std::optional<BackendStep> refuseNext_;
  std::optional<uint64_t> repackedTextureWriteBytes_;
  std::optional<uint64_t> heldFrom_;
};

/// A drawable scene (target, vertex, index and uniform buffers, one bind group, one pipeline)
/// built before the observer is installed, so each case sees only the operations it performs.
class DeviceObserverTests : public testing::Test {
protected:
  void SetUp() override {
    target_ = GetResultOrFail(device_.createTexture(
        TextureDescriptor{"target", Extent2d{4, 4}, TextureFormat::RGBA8Unorm,
                          TextureUsage::RenderAttachment | TextureUsage::CopyDst}));
    targetView_ =
        GetResultOrFail(device_.createTextureView(target_, TextureViewDescriptor{"targetView"}));
    vertexBuffer_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"vertices", 48, BufferUsage::Vertex | BufferUsage::CopyDst}));
    indexBuffer_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"indices", 24, BufferUsage::Index | BufferUsage::CopyDst}));
    uniformBuffer_ = GetResultOrFail(device_.createBuffer(
        BufferDescriptor{"uniforms", 16, BufferUsage::Uniform | BufferUsage::CopyDst}));
    bindGroupLayout_ = GetResultOrFail(device_.createBindGroupLayout(BindGroupLayoutDescriptor{
        "uniforms",
        {BindGroupLayoutEntry{0, ShaderStage::Vertex | ShaderStage::Fragment,
                              BindingType::UniformBuffer}}}));
    pipelineLayout_ = GetResultOrFail(
        device_.createPipelineLayout(PipelineLayoutDescriptor{"solidLayout", {bindGroupLayout_}}));
    bindGroup_ = GetResultOrFail(device_.createBindGroup(bindGroupDescriptor()));
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
    ASSERT_THAT(device_.installObserver(observer_), IsOk());
  }

  void TearDown() override { device_.removeObserver(observer_); }

  BindGroupDescriptor bindGroupDescriptor() const {
    return BindGroupDescriptor{"solidUniforms",
                               bindGroupLayout_,
                               {BindGroupEntry{0, BufferBinding{uniformBuffer_, 0, 16}}}};
  }

  /**
   * Records one command buffer holding one render pass, with the pipeline, bind group, vertex
   * buffer and index buffer bound, whose draws \p recordDraws records.
   *
   * @param recordDraws Records the pass's draws.
   */
  CommandBuffer recordPass(const std::function<void(RenderPassEncoder&)>& recordDraws) {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
        "pass", {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store}}}));
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
    recordDraws(*pass);
    EXPECT_THAT(pass->end(), IsOk());
    return GetResultOrFail(encoder->finish());
  }

  /**
   * Records a pass of \p draws plain draws, \p indexedDraws indexed draws of six indices, and
   * \p emptyIndexedDraws indexed draws of no indices.
   *
   * @param draws Plain draws. @param indexedDraws Indexed draws. @param emptyIndexedDraws Empty
   *   indexed draws.
   */
  CommandBuffer recordPass(uint32_t draws, uint32_t indexedDraws, uint32_t emptyIndexedDraws) {
    return recordPass([&](RenderPassEncoder& pass) {
      for (uint32_t i = 0; i < draws; ++i) {
        EXPECT_THAT(pass.draw(3), IsOk());
      }
      for (uint32_t i = 0; i < indexedDraws; ++i) {
        EXPECT_THAT(pass.drawIndexed(6), IsOk());
      }
      for (uint32_t i = 0; i < emptyIndexedDraws; ++i) {
        EXPECT_THAT(pass.drawIndexed(0), IsOk());
      }
    });
  }

  ScriptedBackendDevice device_;
  RecordingObserver observer_;
  Texture target_;
  TextureView targetView_;
  Buffer vertexBuffer_;
  Buffer indexBuffer_;
  Buffer uniformBuffer_;
  BindGroupLayout bindGroupLayout_;
  PipelineLayout pipelineLayout_;
  BindGroup bindGroup_;
  ShaderModule shader_;
  RenderPipeline pipeline_;
};

TEST_F(DeviceObserverTests, EachAcceptedOperationIsReportedOnce) {
  const Buffer buffer =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"extra", 64, BufferUsage::CopyDst}));
  const Texture texture = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "extra", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  const BindGroup bindGroup = GetResultOrFail(device_.createBindGroup(bindGroupDescriptor()));
  const std::array<uint8_t, 12> bufferBytes{};
  ASSERT_THAT(device_.writeBuffer(buffer, 4, bufferBytes), IsOk());
  // Two rows of two texels at the minimum row pitch, the second row ending at its last texel.
  const std::vector<uint8_t> textureBytes(256 + 8);
  ASSERT_THAT(
      device_.writeTexture(texture, textureBytes, TexelCopyBufferLayout{0, 256, 2}, Extent2d{2, 2}),
      IsOk());
  ASSERT_THAT(device_.submit(recordPass(/*draws=*/1, /*indexedDraws=*/0, /*emptyIndexedDraws=*/0)),
              HasResult());

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{.bufferCreates = 1,
                                                  .textureCreates = 1,
                                                  .bindGroupCreates = 1,
                                                  .bufferWrites = {12},
                                                  .textureWrites = {264},
                                                  .submissions = {{1, 1}}}));
}

TEST_F(DeviceObserverTests, ASubmissionReportsItsBuffersAndTheDrawsItCounts) {
  std::array<CommandBuffer, 2> span{
      recordPass(/*draws=*/2, /*indexedDraws=*/1, /*emptyIndexedDraws=*/1),
      recordPass(/*draws=*/0, /*indexedDraws=*/3, /*emptyIndexedDraws=*/2)};

  ASSERT_THAT(device_.submit(span), HasResult());

  EXPECT_THAT(observer_.events.submissions, ElementsAre(ObservedSubmission{2, 6}))
      << "an empty indexed draw is recorded but does not count as a draw";
}

TEST_F(DeviceObserverTests, AnEmptyPlainDrawStillCounts) {
  ASSERT_THAT(device_.submit(recordPass([](RenderPassEncoder& pass) {
    EXPECT_THAT(pass.draw(0), IsOk());
    EXPECT_THAT(pass.draw(3, 0), IsOk());
    EXPECT_THAT(pass.drawIndexed(6, 0), IsOk());
  })),
              HasResult());

  EXPECT_THAT(observer_.events.submissions, ElementsAre(ObservedSubmission{1, 2}))
      << "a draw command counts even with no vertices or instances; an indexed draw with no "
         "instances does not";
}

TEST_F(DeviceObserverTests, OperationsValidationRefusesReportNothing) {
  EXPECT_THAT(device_.createBuffer(BufferDescriptor{"empty", 0, BufferUsage::CopyDst}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
  EXPECT_THAT(device_.createTexture(TextureDescriptor{
                  "empty", Extent2d{0, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
  EXPECT_THAT(
      device_.createBindGroup(BindGroupDescriptor{"noEntries", bindGroupLayout_, /*entries=*/{}}),
      IsGpuError(GpuErrorType::InvalidDescriptor));
  const std::array<uint8_t, 64> tooLong{};
  EXPECT_THAT(device_.writeBuffer(uniformBuffer_, 0, tooLong),
              IsGpuError(GpuErrorType::OutOfBounds));
  const std::vector<uint8_t> textureBytes(256 * 4);
  EXPECT_THAT(
      device_.writeTexture(target_, textureBytes, TexelCopyBufferLayout{0, 256, 8}, Extent2d{4, 8}),
      IsGpuError(GpuErrorType::OutOfBounds));

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{}));
}

TEST_F(DeviceObserverTests, OperationsTheBackendRefusesReportNothing) {
  device_.refuseNext(BackendStep::Buffer);
  EXPECT_THAT(device_.createBuffer(BufferDescriptor{"refused", 16, BufferUsage::CopyDst}),
              IsGpuError(GpuErrorType::DeviceLost));
  device_.refuseNext(BackendStep::Texture);
  EXPECT_THAT(device_.createTexture(TextureDescriptor{
                  "refused", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}),
              IsGpuError(GpuErrorType::DeviceLost));
  device_.refuseNext(BackendStep::BindGroup);
  EXPECT_THAT(device_.createBindGroup(bindGroupDescriptor()), IsGpuError(GpuErrorType::DeviceLost));
  device_.refuseNext(BackendStep::TextureWrite);
  const std::vector<uint8_t> textureBytes(256 * 2);
  EXPECT_THAT(
      device_.writeTexture(target_, textureBytes, TexelCopyBufferLayout{0, 256, 2}, Extent2d{4, 2}),
      IsGpuError(GpuErrorType::DeviceLost));
  device_.refuseNext(BackendStep::Submission);
  EXPECT_THAT(device_.submit(recordPass(1, 1, 0)), IsGpuError(GpuErrorType::DeviceLost));

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{}));
}

TEST_F(DeviceObserverTests, ARegisteredTextureIsNotAnAllocation) {
  device_.registerNextTexture();
  const Texture registered = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "registered", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  EXPECT_THAT(observer_.events.textureCreates, Eq(0u))
      << "naming a texture the device does not own allocates nothing";

  const Texture owned = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "owned", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  EXPECT_THAT(observer_.events.textureCreates, Eq(1u));
}

/// Creates a 2x2 texture the device owns, the subject of the release cases below.
/// @param device Device to create it on.
Texture CreateOwnedTexture(Device& device) {
  return GetResultOrFail(device.createTexture(TextureDescriptor{
      "released", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
}

TEST_F(DeviceObserverTests, ADestroyedTextureReportsItsReleaseOnce) {
  Texture texture = CreateOwnedTexture(device_);
  ASSERT_THAT(device_.destroyTexture(std::move(texture)), IsOk());
  device_.poll();

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{.textureCreates = 1, .textureReleases = 1}));
}

TEST_F(DeviceObserverTests, ADroppedTextureHandleReportsItsReleaseOnce) {
  { const Texture texture = CreateOwnedTexture(device_); }
  device_.poll();

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{.textureCreates = 1, .textureReleases = 1}));
}

/// A destroyed texture that submitted work still uses keeps its allocation until that work
/// completes, so its release is reported then rather than when the handle goes.
TEST_F(DeviceObserverTests, ATextureInUseReportsItsReleaseWhenItsLastSubmissionCompletes) {
  Texture texture = GetResultOrFail(
      device_.createTexture(TextureDescriptor{"inUse", Extent2d{2, 2}, TextureFormat::RGBA8Unorm,
                                              TextureUsage::CopySrc | TextureUsage::CopyDst}));
  const Texture sink = CreateOwnedTexture(device_);
  std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
  ASSERT_THAT(encoder->copyTextureToTexture(texture, sink, Extent2d{2, 2}), IsOk());
  device_.holdCompletionsFrom(device_.lastSubmittedSerial() + 1);
  ASSERT_THAT(device_.submit(GetResultOrFail(encoder->finish())), HasResult());
  ASSERT_THAT(device_.destroyTexture(std::move(texture)), IsOk());
  device_.poll();
  EXPECT_THAT(observer_.events.textureReleases, Eq(0u))
      << "the backend still uses the allocation, so it has not been released";

  device_.completeAll();
  device_.poll();
  EXPECT_THAT(observer_.events.textureReleases, Eq(1u));
}

/// Destroying the backing releases the allocation at once. The slot the handle named is recycled
/// later, and that does not release the same allocation a second time.
TEST_F(DeviceObserverTests, DestroyingATexturesBackingReportsItsReleaseOnce) {
  Texture texture = CreateOwnedTexture(device_);
  ASSERT_THAT(device_.destroyTextureBacking(std::move(texture)), IsOk());
  EXPECT_THAT(observer_.events.textureReleases, Eq(1u));

  device_.poll();
  EXPECT_THAT(observer_.events, Eq(ObservedEvents{.textureCreates = 1, .textureReleases = 1}));
}

TEST_F(DeviceObserverTests, ReleasingARegisteredTextureReleasesNoAllocation) {
  device_.registerNextTexture();
  Texture registered = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "registered", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  device_.registerNextTexture();
  Texture registeredBacking = GetResultOrFail(device_.createTexture(TextureDescriptor{
      "registered", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));

  ASSERT_THAT(device_.destroyTexture(std::move(registered)), IsOk());
  ASSERT_THAT(device_.destroyTextureBacking(std::move(registeredBacking)), IsOk());
  device_.poll();

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{}))
      << "naming a texture the device does not own allocated nothing, so dropping it releases "
         "nothing";
}

TEST_F(DeviceObserverTests, ATextureWriteReportsWhatTheBackendUploaded) {
  const std::vector<uint8_t> bytes(256 + 16);
  ASSERT_THAT(
      device_.writeTexture(target_, bytes, TexelCopyBufferLayout{0, 256, 2}, Extent2d{4, 2}),
      IsOk());
  device_.reportRepackedTextureWrites(512);
  ASSERT_THAT(
      device_.writeTexture(target_, bytes, TexelCopyBufferLayout{0, 256, 2}, Extent2d{4, 2}),
      IsOk());

  EXPECT_THAT(observer_.events.textureWrites, ElementsAre(272u, 512u))
      << "the caller's span by default, the repacked size from a backend that repacks";
}

TEST_F(DeviceObserverTests, ABackendsOwnQueueSubmissionCarriesNoBuffersOrDraws) {
  device_.submitOnItsOwn();

  EXPECT_THAT(observer_.events.submissions, ElementsAre(ObservedSubmission{0, 0}));
  EXPECT_THAT(device_.lastSubmittedSerial(), Eq(0u))
      << "a backend's own queue submission is not a runtime submission and takes no serial";
}

TEST_F(DeviceObserverTests, ADifferentObserverIsRefusedAndTheInstalledOneKeepsReporting) {
  RecordingObserver other;
  EXPECT_THAT(device_.installObserver(other),
              IsGpuErrorWithMessage(GpuErrorType::InvalidState, HasSubstr("another observer")));
  EXPECT_THAT(device_.observer(), Eq(&observer_));

  const Buffer buffer =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"b", 16, BufferUsage::CopyDst}));

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{.bufferCreates = 1}));
  EXPECT_THAT(other.events, Eq(ObservedEvents{}));
}

TEST_F(DeviceObserverTests, InstallingTheInstalledObserverAgainChangesNothing) {
  ASSERT_THAT(device_.installObserver(observer_), IsOk());

  const Buffer buffer =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"b", 16, BufferUsage::CopyDst}));

  EXPECT_THAT(observer_.events, Eq(ObservedEvents{.bufferCreates = 1}))
      << "one installation reports each operation once";
}

TEST_F(DeviceObserverTests, OnlyTheInstalledObserverCanBeRemoved) {
  RecordingObserver other;
  device_.removeObserver(other);
  EXPECT_THAT(device_.observer(), Eq(&observer_)) << "removing another observer changes nothing";

  device_.removeObserver(observer_);
  EXPECT_THAT(device_.observer(), Eq(nullptr));
  ASSERT_THAT(device_.installObserver(other), IsOk());
  const Buffer buffer =
      GetResultOrFail(device_.createBuffer(BufferDescriptor{"b", 16, BufferUsage::CopyDst}));
  device_.removeObserver(other);

  EXPECT_THAT(other.events, Eq(ObservedEvents{.bufferCreates = 1}));
  EXPECT_THAT(observer_.events, Eq(ObservedEvents{}));
}

TEST_F(DeviceObserverTests, RemovingTheObserverStopsReports) {
  device_.removeObserver(observer_);
  const Buffer buffer = GetResultOrFail(
      device_.createBuffer(BufferDescriptor{"unobserved", 16, BufferUsage::CopyDst}));
  ASSERT_THAT(device_.submit(recordPass(1, 0, 0)), HasResult());
  device_.submitOnItsOwn();

  EXPECT_THAT(device_.observer(), Eq(nullptr));
  EXPECT_THAT(observer_.events, Eq(ObservedEvents{}));
}

TEST(DeviceObserverRecordingDeviceTests, TheRecordingBackendReportsTheCallersSpan) {
  RecordingDevice device;
  RecordingObserver observer;
  ASSERT_THAT(device.installObserver(observer), IsOk());
  const Texture texture = GetResultOrFail(device.createTexture(TextureDescriptor{
      "texture", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  const std::vector<uint8_t> bytes(256 + 8);
  ASSERT_THAT(device.writeTexture(texture, bytes, TexelCopyBufferLayout{0, 256, 2}, Extent2d{2, 2}),
              IsOk());
  device.removeObserver(observer);

  EXPECT_THAT(observer.events, Eq(ObservedEvents{.textureCreates = 1, .textureWrites = {264}}));
}

}  // namespace
}  // namespace donner::gpu
