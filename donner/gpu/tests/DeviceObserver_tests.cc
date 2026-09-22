/// @file
/// Device observer tests: every operation the validation layer accepts and the backend accepts is
/// reported once, with the same meaning on every backend, and nothing refused is reported.

#include "donner/gpu/DeviceObserver.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/RecordingDevice.h"
#include "donner/gpu/tests/GpuTestUtils.h"

using testing::ElementsAre;
using testing::Eq;

namespace donner::gpu {
namespace {

/// One reported submission: how many command buffers it carried and how many draws it issued.
struct ObservedSubmission {
  size_t commandBuffers = 0;  //!< Command buffers the submission carried.
  uint64_t draws = 0;         //!< Draws it issued.

  /// Equality operator. @param other Submission to compare against.
  bool operator==(const ObservedSubmission& other) const = default;
};

/// Prints an \ref ObservedSubmission. @param value Submission. @param os Output stream.
void PrintTo(const ObservedSubmission& value, std::ostream* os) {
  *os << "{commandBuffers=" << value.commandBuffers << " draws=" << value.draws << "}";
}

/// Everything an observer was told, in one comparable value.
struct ObservedEvents {
  uint32_t bufferCreates = 0;                   //!< onBufferCreated calls.
  uint32_t textureCreates = 0;                  //!< onTextureCreated calls.
  uint32_t bindGroupCreates = 0;                //!< onBindGroupCreated calls.
  std::vector<uint64_t> bufferWrites;           //!< Byte count of each onBufferWritten.
  std::vector<uint64_t> textureWrites;          //!< Byte count of each onTextureWritten.
  std::vector<ObservedSubmission> submissions;  //!< Each onSubmitted, in order.

  /// Equality operator. @param other Events to compare against.
  bool operator==(const ObservedEvents& other) const = default;
};

/// Prints \ref ObservedEvents field by field, so a mismatch names the event that differs.
/// @param value Events. @param os Output stream.
void PrintTo(const ObservedEvents& value, std::ostream* os) {
  *os << "{bufferCreates=" << value.bufferCreates << " textureCreates=" << value.textureCreates
      << " bindGroupCreates=" << value.bindGroupCreates
      << " bufferWrites=" << testing::PrintToString(value.bufferWrites)
      << " textureWrites=" << testing::PrintToString(value.textureWrites)
      << " submissions=" << testing::PrintToString(value.submissions) << "}";
}

/// Observer that records every notification it receives.
class RecordingObserver final : public DeviceObserver {
public:
  void onBufferCreated() override { ++events.bufferCreates; }
  void onTextureCreated() override { ++events.textureCreates; }
  void onBindGroupCreated() override { ++events.bindGroupCreates; }
  void onBufferWritten(uint64_t byteCount) override { events.bufferWrites.push_back(byteCount); }
  void onTextureWritten(uint64_t byteCount) override { events.textureWrites.push_back(byteCount); }
  void onSubmitted(size_t commandBufferCount, uint64_t drawCount) override {
    events.submissions.push_back(ObservedSubmission{commandBufferCount, drawCount});
  }

  ObservedEvents events;  //!< What this observer has been told so far.
};

/**
 * Test backend with the behaviors that decide what is reported: it can name a texture it was
 * handed instead of allocating one, refuse the next buffer or submission, report a repacked
 * texture upload size, and submit to its queue on its own.
 */
class ScriptedBackendDevice final : public Device {
public:
  /// Makes the next created texture a registration of memory this device does not own.
  void registerNextTexture() { registerNextTexture_ = true; }

  /// Makes the backend refuse the next buffer it is asked to create.
  void refuseNextBuffer() { refuseNextBuffer_ = true; }

  /// Makes the backend refuse the next submission.
  void refuseNextSubmission() { refuseNextSubmission_ = true; }

  /// Makes every accepted texture write report \p byteCount as its uploaded size, the way a
  /// backend that repacks rows before uploading does. @param byteCount Size to report.
  void reportRepackedTextureWrites(uint64_t byteCount) { repackedTextureWriteBytes_ = byteCount; }

  /// Submits to the backend queue without going through \ref submit, the way a backend nudges a
  /// queue so that a pending completion is delivered.
  void submitOnItsOwn() const { notifyObserverOfBackendSubmission(); }

  /// Submissions complete as they are accepted.
  uint64_t completedSerial() const override { return lastSubmittedSerial(); }

protected:
  Status onCreateBuffer(uint32_t, const BufferDescriptor&) override {
    if (std::exchange(refuseNextBuffer_, false)) {
      return GpuError{GpuErrorType::InvalidState, "the test backend refused the buffer"};
    }
    return OkStatus();
  }
  Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor&) override {
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
                        const Extent2d&, const Origin2d&) override {
    return OkStatus();
  }
  uint64_t onTextureWriteByteCount(TextureFormat format, std::span<const uint8_t> data,
                                   const TexelCopyBufferLayout& dataLayout,
                                   const Extent2d& writeSize) const override {
    return repackedTextureWriteBytes_.value_or(
        Device::onTextureWriteByteCount(format, data, dataLayout, writeSize));
  }
  Status onSubmit(uint64_t, std::span<const SubmittedCommandBuffer>) override {
    if (std::exchange(refuseNextSubmission_, false)) {
      return GpuError{GpuErrorType::DeviceLost, "the test backend lost the device"};
    }
    return OkStatus();
  }

private:
  std::vector<bool> ownedTextures_;
  bool registerNextTexture_ = false;
  bool refuseNextBuffer_ = false;
  bool refuseNextSubmission_ = false;
  std::optional<uint64_t> repackedTextureWriteBytes_;
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
    device_.setObserver(&observer_);
  }

  void TearDown() override { device_.setObserver(nullptr); }

  BindGroupDescriptor bindGroupDescriptor() const {
    return BindGroupDescriptor{"solidUniforms",
                               bindGroupLayout_,
                               {BindGroupEntry{0, BufferBinding{uniformBuffer_, 0, 16}}}};
  }

  /**
   * Records one command buffer holding one render pass that issues \p draws plain draws and
   * \p indexedDraws indexed draws, followed by \p emptyIndexedDraws indexed draws of no indices.
   *
   * @param draws Plain draws to record.
   * @param indexedDraws Indexed draws of six indices to record.
   * @param emptyIndexedDraws Indexed draws of zero indices to record.
   */
  CommandBuffer recordPass(uint32_t draws, uint32_t indexedDraws, uint32_t emptyIndexedDraws) {
    std::unique_ptr<CommandEncoder> encoder = GetResultOrFail(device_.createCommandEncoder());
    RenderPassEncoder* pass = GetResultOrFail(encoder->beginRenderPass(RenderPassDescriptor{
        "pass", {RenderPassColorAttachment{targetView_, LoadOp::Clear, StoreOp::Store}}}));
    EXPECT_THAT(pass->setPipeline(pipeline_), IsOk());
    EXPECT_THAT(pass->setBindGroup(0, bindGroup_), IsOk());
    EXPECT_THAT(pass->setVertexBuffer(0, vertexBuffer_), IsOk());
    EXPECT_THAT(pass->setIndexBuffer(indexBuffer_, IndexFormat::Uint16), IsOk());
    for (uint32_t i = 0; i < draws; ++i) {
      EXPECT_THAT(pass->draw(3), IsOk());
    }
    for (uint32_t i = 0; i < indexedDraws; ++i) {
      EXPECT_THAT(pass->drawIndexed(6), IsOk());
    }
    for (uint32_t i = 0; i < emptyIndexedDraws; ++i) {
      EXPECT_THAT(pass->drawIndexed(0), IsOk());
    }
    EXPECT_THAT(pass->end(), IsOk());
    return GetResultOrFail(encoder->finish());
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

TEST_F(DeviceObserverTests, ASubmissionReportsItsBuffersAndTheDrawsItIssues) {
  std::array<CommandBuffer, 2> span{
      recordPass(/*draws=*/2, /*indexedDraws=*/1, /*emptyIndexedDraws=*/1),
      recordPass(/*draws=*/0, /*indexedDraws=*/3, /*emptyIndexedDraws=*/2)};

  ASSERT_THAT(device_.submit(span), HasResult());

  EXPECT_THAT(observer_.events.submissions, ElementsAre(ObservedSubmission{2, 6}))
      << "an indexed draw of no indices is recorded but never issued, so it is not a draw";
}

TEST_F(DeviceObserverTests, RefusedOperationsReportNothing) {
  // Refused by validation.
  EXPECT_THAT(device_.createBuffer(BufferDescriptor{"empty", 0, BufferUsage::CopyDst}),
              IsGpuError(GpuErrorType::InvalidDescriptor));
  const std::array<uint8_t, 64> tooLong{};
  EXPECT_THAT(device_.writeBuffer(uniformBuffer_, 0, tooLong),
              IsGpuError(GpuErrorType::OutOfBounds));
  // Refused by the backend after validation accepted it.
  device_.refuseNextBuffer();
  EXPECT_THAT(device_.createBuffer(BufferDescriptor{"refused", 16, BufferUsage::CopyDst}),
              IsGpuError(GpuErrorType::InvalidState));
  device_.refuseNextSubmission();
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

TEST_F(DeviceObserverTests, RemovingTheObserverStopsReports) {
  device_.setObserver(nullptr);
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
  device.setObserver(&observer);
  const Texture texture = GetResultOrFail(device.createTexture(TextureDescriptor{
      "texture", Extent2d{2, 2}, TextureFormat::RGBA8Unorm, TextureUsage::CopyDst}));
  const std::vector<uint8_t> bytes(256 + 8);
  ASSERT_THAT(device.writeTexture(texture, bytes, TexelCopyBufferLayout{0, 256, 2}, Extent2d{2, 2}),
              IsOk());
  device.setObserver(nullptr);

  EXPECT_THAT(observer.events, Eq(ObservedEvents{.textureCreates = 1, .textureWrites = {264}}));
}

}  // namespace
}  // namespace donner::gpu
