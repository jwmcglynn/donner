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
 * Metal backend of the Donner GPU runtime (design 0053 packet 6, the vertical-slice backend).
 *
 * Inherits every fail-closed validation check from \ref donner::gpu::Device; the `on*` hooks
 * receive only validated input and translate it to Metal objects. Any Metal-side failure (nil
 * object, compile error, encoder failure) fails closed with a \ref donner::gpu::GpuError; the
 * backend never crashes on such failures.
 *
 * Scope is exactly the solid-fill vertical slice: shared-storage buffers and textures, MSL
 * shader modules, one render pipeline family with a single vertex buffer layout at slot 0 and
 * bind group 0 only, render passes with color attachments, and texture-to-buffer readback
 * copies. Bindings follow the deterministic argument-table mapping in
 * `donner/gpu/shader/MslBindingMap.h`: buffer binding `b` maps to Metal buffer index `1 + b`,
 * texture and sampler bindings map directly, and stage-in vertex data occupies vertex buffer
 * index 30.
 *
 * Resources use `MTLStorageModeShared`: this backend targets Apple Silicon's unified memory, so
 * shared storage keeps queue writes (`memcpy` / `replaceRegion`) and buffer readback simple with
 * no staging copies.
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
  /**
   * Creates a device on the system default Metal device. Returns nullptr if no Metal device is
   * available (for example on a CI host without a GPU).
   */
  static std::unique_ptr<MetalDevice> Create();

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
   * Test/readback convenience for the vertical slice, pending the packet-7 buffer mapping API:
   * it validates only device identity and slot liveness (not the handle generation), then reads
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
