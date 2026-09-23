/// @file
/// A test backend whose runtime devices can name each other's textures over one fake native
/// device, with submission completion, execution failure and queued writes driven by the test.
/// Shared by the runtime's registration tests and by tests of code layered on registration.

#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/Device.h"
#include "donner/gpu/tests/GpuTestUtils.h"

namespace donner::gpu {

/// Backend families: only devices of one family may name each other's textures.
inline constexpr char kSharingFamily = 0;
inline constexpr char kOtherFamily = 1;

/// A native device two runtime devices can share, counting the native textures still alive and
/// the explicit releases a producer deferred until nothing else held the texture.
struct FakeNativeDevice {
  std::atomic<int> liveTextures{0};
  std::atomic<int> deferredBackingReleases{0};
};

/// One native texture allocation; alive while any device slot, export or registration holds it.
class FakeNativeTexture {
public:
  explicit FakeNativeTexture(FakeNativeDevice& device) : device_(device) {
    device_.liveTextures.fetch_add(1);
  }
  ~FakeNativeTexture() { device_.liveTextures.fetch_sub(1); }

  FakeNativeTexture(const FakeNativeTexture&) = delete;
  FakeNativeTexture& operator=(const FakeNativeTexture&) = delete;

private:
  FakeNativeDevice& device_;
};

/// The test backend's export: the only strong reference to the native texture outside its
/// producer, so the allocation outlives the producer only if the runtime keeps the export alive.
class FakeExportedTexture final : public ExportedTextureBacking {
public:
  FakeExportedTexture(std::shared_ptr<FakeNativeTexture> native, FakeNativeDevice& device)
      : native(std::move(native)), device_(device) {}

  void releaseBackingNow() const override { device_.deferredBackingReleases.fetch_add(1); }

  std::shared_ptr<FakeNativeTexture> native;

private:
  FakeNativeDevice& device_;
};

/// Completion the test drives, shared with every export of the device that owns it.
class FakeCompletion final : public SubmissionCompletion {
public:
  uint64_t completedSerial() const override { return completed.load(); }
  bool failed() const override { return failedFlag.load(); }

  std::atomic<uint64_t> completed{0};
  std::atomic<bool> failedFlag{false};
};

/// What a test device is built with.
struct SharingOptions {
  const void* family = &kSharingFamily;                     //!< Backend family tag.
  SourceOrdering ordering = SourceOrdering::WaitForSource;  //!< Ordering its exports report.
  std::shared_ptr<DeviceLostState> lostState;               //!< Shared loss condition, or null.
};

/**
 * Test backend whose devices can share textures over one \ref FakeNativeDevice. Submissions
 * complete at once unless the test holds them, and the test decides whether a texture write waits
 * for the next submission.
 */
class SharingDevice final : public Device {
public:
  SharingDevice(FakeNativeDevice& native, SharingOptions options = {})
      : native_(native), options_(std::move(options)) {
    adoptLostState(options_.lostState);
  }

  /// Submissions stop completing until \ref releaseCompletion.
  void holdCompletion() { held_ = true; }
  /// Completes everything submitted so far, and later submissions as they arrive.
  void releaseCompletion() {
    held_ = false;
    completion_->completed.store(lastSubmittedSerial());
  }
  /// Reports a terminal execution failure, as a backend does for a failed command buffer.
  void failExecution() { completion_->failedFlag.store(true); }
  /// Whether texture writes wait for the next submission.
  void setWritesPending(bool pending) { writesPending_ = pending; }
  /// How many explicit backing releases reached this backend.
  int explicitBackingReleases() const { return explicitBackingReleases_; }

  uint64_t completedSerial() const override { return completion_->completed.load(); }

protected:
  BackendDeviceIdentity backendDeviceIdentity() const override {
    return {options_.family, &native_};
  }
  Result<BackendTextureExport> onExportTexture(uint32_t slotIndex) override {
    BackendTextureExport exported;
    exported.backing =
        std::make_shared<const FakeExportedTexture>(textures_.at(slotIndex).owned, native_);
    exported.ordering = options_.ordering;
    exported.completion = completion_;
    exported.writePending = queuedWrite_;
    return exported;
  }
  Status onRegisterTexture(uint32_t slotIndex, const ExportedTextureBacking& backing) override {
    // A borrowed name, like the transitional adapter's: the registration's lifetime has to come
    // from the runtime's hold on the export, not from this slot.
    slot(slotIndex).alias = static_cast<const FakeExportedTexture&>(backing).native.get();
    return OkStatus();
  }
  bool onTextureWritePending(uint32_t) const override { return writesPending_; }
  void onDestroyTextureBacking(uint32_t slotIndex) override {
    ++explicitBackingReleases_;
    slot(slotIndex) = {};
  }

  Status onCreateBuffer(uint32_t, const BufferDescriptor&) override { return OkStatus(); }
  Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor&) override {
    slot(slotIndex).owned = std::make_shared<FakeNativeTexture>(native_);
    return OkStatus();
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
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override {
    if (resourceName == TextureTag::kName) {
      slot(slotIndex) = {};
    }
  }
  Status onWriteBuffer(uint32_t, uint64_t, std::span<const uint8_t>) override { return OkStatus(); }
  Status onWriteTexture(uint32_t, std::span<const uint8_t>, const TexelCopyBufferLayout&,
                        const Extent2d&, const Origin2d&) override {
    queuedWrite_ = queuedWrite_ || writesPending_;
    return OkStatus();
  }
  Status onSubmit(uint64_t submissionSerial, std::span<const SubmittedCommandBuffer>) override {
    queuedWrite_ = false;
    if (!held_) {
      completion_->completed.store(submissionSerial);
    }
    return OkStatus();
  }

private:
  /// One texture slot: the allocation this device made, or a borrowed name for another's.
  struct TextureSlot {
    std::shared_ptr<FakeNativeTexture> owned;  //!< Allocation this device made, or null.
    FakeNativeTexture* alias = nullptr;        //!< Registration of another device's texture.
  };

  TextureSlot& slot(uint32_t slotIndex) {
    if (slotIndex >= textures_.size()) {
      textures_.resize(slotIndex + 1);
    }
    return textures_[slotIndex];
  }

  FakeNativeDevice& native_;
  SharingOptions options_;
  std::shared_ptr<FakeCompletion> completion_ = std::make_shared<FakeCompletion>();
  std::vector<TextureSlot> textures_;
  bool held_ = false;
  bool writesPending_ = false;
  bool queuedWrite_ = false;  //!< A write waits for the next submission.
  int explicitBackingReleases_ = 0;
};

/// Size of every test texture.
inline constexpr Extent2d kSharedTextureExtent{4, 4};
/// Usage a producer creates its test textures with.
inline constexpr TextureUsage kSharedTextureUsage = TextureUsage::RenderAttachment |
                                                    TextureUsage::Sampled | TextureUsage::CopySrc |
                                                    TextureUsage::CopyDst;

/// Creates a readable, writable test texture on \p device.
/// @param device Device to allocate on. @param usage Usage to create with.
inline Texture MakeSharedTexture(Device& device, TextureUsage usage = kSharedTextureUsage) {
  return GetResultOrFail(device.createTexture(
      TextureDescriptor{"shared", kSharedTextureExtent, TextureFormat::RGBA8Unorm, usage}));
}

/// Submits one copy of \p texture into a fresh buffer, which is a submission that reads it.
/// @param device Device \p texture belongs to. @param texture Texture to read.
inline Result<uint64_t> SubmitSharedTextureRead(Device& device, const Texture& texture) {
  Result<Buffer> buffer = device.createBuffer(BufferDescriptor{
      "readback", 256u * kSharedTextureExtent.height, BufferUsage::CopyDst | BufferUsage::MapRead});
  if (buffer.hasError()) {
    return std::move(buffer).error();
  }
  Result<std::unique_ptr<CommandEncoder>> encoder = device.createCommandEncoder();
  if (encoder.hasError()) {
    return std::move(encoder).error();
  }
  if (Status copied = encoder.result()->copyTextureToBuffer(
          TexelCopyTextureInfo{texture}, buffer.result(),
          TexelCopyBufferLayout{0, 256, kSharedTextureExtent.height}, kSharedTextureExtent);
      copied.hasError()) {
    return std::move(copied).error();
  }
  Result<CommandBuffer> commands = encoder.result()->finish();
  if (commands.hasError()) {
    return std::move(commands).error();
  }
  return device.submit(std::move(commands).result());
}

}  // namespace donner::gpu
