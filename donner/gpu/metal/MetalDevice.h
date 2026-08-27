#pragma once
/// @file
/// \c donner::gpu::metal::MetalDevice - the Metal backend for the Donner GPU runtime.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "donner/gpu/Device.h"

namespace donner::gpu::metal {

/**
 * Metal backend of the Donner GPU runtime.
 *
 * Inherits every fail-closed validation check from \ref donner::gpu::Device; the `on*` hooks
 * receive only validated input and translate it to Metal objects. Any Metal-side failure (nil
 * object, compile error, encoder failure) fails closed with a \ref donner::gpu::GpuError; the
 * backend never crashes on such failures.
 *
 * Scope: host-visible buffers and textures, MSL shader modules, render pipelines with a single
 * vertex buffer layout at slot 0 and bind group 0 only, render passes with color attachments,
 * compute pipelines and compute passes, and texture-to-buffer readback copies. Bindings follow
 * the deterministic argument-table mapping in
 * `donner/gpu/shader/MslBindingMap.h`: buffer binding `b` maps to Metal buffer index `1 + b`,
 * texture and sampler bindings map directly, and stage-in vertex data occupies vertex buffer
 * index 30.
 *
 * Memory model: the storage mode of host-visible resources follows what the device reports
 * rather than an assumption about it. Where the CPU and GPU address one copy of a resource
 * (`hasUnifiedMemory`), resources are `MTLStorageModeShared` and queue writes (`memcpy` /
 * `replaceRegion`) and buffer readback need no staging and no publication step. Where they do
 * not, resources are `MTLStorageModeManaged` and each side's changes must be published to the
 * other explicitly: a host buffer write publishes its range, and every submission ends by
 * publishing the device's changes back before it completes, since afterwards there is no encoder
 * left to do it with. Neither omission is an API error, so a missing publication is not reported
 * by validation - it shows up only as the host reading whatever its copy last held.
 *
 * Threading: single-threaded use, matching \ref donner::gpu::Device's thread affinity. The one
 * exception is command-buffer completion handlers, which Metal invokes on an internal queue;
 * they touch only atomics and a mutex-protected error string, observable through
 * \ref completedSerial, \ref waitForSerial, and \ref lastErrorForTest.
 *
 * The header is pure C++ (Objective-C state lives behind a pimpl) so it is includable from C++
 * tests; the implementation is Objective-C++.
 */
class MetalDevice final : public Device {
public:
  /// Which memory model the backend builds its resources for.
  enum class MemoryModel : uint8_t {
    /// Take the model the Metal device reports. Production always uses this.
    Detected,
    /// Build for a device without unified memory whatever it reports, so the host-coherency
    /// steps that model needs are exercised on hardware that would otherwise never take them.
    ForceNonUnified,
  };

  /**
   * Creates a device on the system default Metal device. Returns nullptr if no Metal device is
   * available (for example on a CI host without a GPU).
   *
   * @param memoryModel Which memory model to build resources for; production leaves this
   *   detected, and a test forces the non-unified path to cover it on unified hardware.
   */
  static std::unique_ptr<MetalDevice> Create(MemoryModel memoryModel = MemoryModel::Detected);

  /// Whether this device's resources are built for unified memory. Test accessor.
  [[nodiscard]] bool usesUnifiedMemoryForTest() const;

  /// How many buffer writes have published their range to the device copy. Test accessor.
  ///
  /// Counts the buffer path only, where the write is a `memcpy` through the host pointer and the
  /// range has to be published after it. Texture writes go through `replaceRegion`, which
  /// publishes what it wrote on its own, so they are deliberately absent from this count rather
  /// than missing from it.
  ///
  /// On unified memory nothing is published and this stays zero. The count exists because
  /// hardware that addresses one copy produces correct results whether or not the publication
  /// happened, so the results cannot show whether it did.
  [[nodiscard]] uint64_t hostWritePublishCountForTest() const;

  /// How many submissions have published the device's changes back to the host copy. Test
  /// accessor, for the same reason as above but in the other direction.
  [[nodiscard]] uint64_t deviceWritePublishCountForTest() const;

  /// Destructor; releases all Metal objects still alive.
  ~MetalDevice() override;

  /// Serial of the most recent submission whose Metal command buffer has completed on the GPU
  /// (0 if none). Updated by completion handlers, which may run on another thread.
  uint64_t completedSerial() const override;

  /**
   * Blocks until \ref completedSerial reaches \p serial or \p timeoutSeconds elapses, by polling
   * the completion counter (the completion handler runs on a Metal-internal thread, so a poll
   * loop with a short sleep is sufficient and keeps this backend free of extra sync primitives).
   *
   * Returns false on timeout, and also returns false if any completed command buffer reported an
   * execution error (see \ref lastErrorForTest).
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Maximum time to wait, in seconds.
   */
  bool waitForSerial(uint64_t serial, double timeoutSeconds);

  /**
   * Copies the full contents of \p buffer back to the host and returns the bytes.
   *
   * Test/readback convenience, pending a buffer mapping API: it validates device identity, slot
   * liveness, and the handle generation, then reads
   * the shared-storage Metal buffer contents directly. Callers must ensure relevant GPU work has
   * completed first (see \ref waitForSerial).
   *
   * @param buffer Buffer to read back; must be a live buffer of this device.
   */
  Result<std::vector<uint8_t>> readBackBuffer(const Buffer& buffer);

  /// Message of the most recent asynchronous command-buffer execution error captured by a
  /// completion handler, or an empty string if none occurred. Test/diagnostic accessor.
  std::string lastErrorForTest() const;

protected:
  Status onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) override;
  Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) override;
  Status onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                             const TextureViewDescriptor& descriptor) override;
  Status onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) override;
  Status onCreateBindGroupLayout(uint32_t slotIndex,
                                 const BindGroupLayoutDescriptor& descriptor) override;
  Status onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) override;
  Status onCreatePipelineLayout(uint32_t slotIndex,
                                const PipelineLayoutDescriptor& descriptor) override;
  Status onCreateShaderModule(uint32_t slotIndex,
                              const ShaderModuleDescriptor& descriptor) override;
  Status onCreateRenderPipeline(uint32_t slotIndex,
                                const RenderPipelineDescriptor& descriptor) override;
  Status onCreateComputePipeline(uint32_t slotIndex,
                                 const ComputePipelineDescriptor& descriptor) override;
  void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) override;
  Status onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                       std::span<const uint8_t> data) override;
  Status onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                        const TexelCopyBufferLayout& dataLayout,
                        const Extent2d& writeSize) override;
  Status onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                  std::span<const Command> commands) override;

private:
  /// Constructs an empty device; \ref Create attaches the Metal device.
  MetalDevice();

  struct Impl;  //!< Objective-C++ state (Metal objects and slot tables); defined in the .mm.
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::gpu::metal
