#pragma once
/// @file
/// \c donner::gpu::vulkan::VulkanDevice - the Vulkan backend for the Donner GPU runtime.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "donner/gpu/Device.h"

namespace donner::gpu::vulkan {

/**
 * Vulkan backend of the Donner GPU runtime (design 0053 packet 7, the Vulkan vertical slice).
 *
 * Inherits every fail-closed validation check from \ref donner::gpu::Device; the `on*` hooks
 * receive only validated input and translate it to Vulkan objects. Every Vulkan call's VkResult
 * is checked and any failure fails closed with a \ref donner::gpu::GpuError; the backend never
 * crashes on such failures.
 *
 * Scope is exactly the solid-fill vertical slice: buffers and 2D single-sample textures, SPIR-V
 * shader modules (from \ref donner::gpu::shader::EmitSpirv), the solid-fill render pipeline
 * family, render passes with color attachments, and texture-to-buffer readback copies. Bind
 * group index N maps directly to Vulkan descriptor set N; binding numbers map directly to
 * SPIR-V binding decorations (the SPIR-V emitter uses DescriptorSet 0 / Binding b).
 *
 * Targets Vulkan 1.1 core only: classic VkRenderPass + VkFramebuffer (no dynamic rendering),
 * per-submission VkFence completion tracking (no timeline semaphores), and the core
 * negative-viewport-height feature (VK_KHR_maintenance1, promoted to 1.1) to present WebGPU
 * clip-space semantics - identical SPIR-V positions land on identical pixels as the wgpu
 * baseline.
 *
 * Memory model (documented simplification for this slice): every buffer lives in
 * HOST_VISIBLE | HOST_COHERENT memory and stays persistently mapped, so queue writes are a
 * `memcpy` and readback needs no staging. Such a memory type is guaranteed by the Vulkan
 * specification ("Device Memory": at least one memory type has both HOST_VISIBLE and
 * HOST_COHERENT), and both the CI software rasterizer and desktop GPUs expose it. Textures are
 * DEVICE_LOCAL (when available) with staged uploads through a transient host-visible buffer and
 * explicit image layout transitions.
 *
 * Synchronization is intentionally conservative and validation-clean (design 0053 "Vulkan"):
 * full ALL_COMMANDS / memory-availability barriers around image layout transitions, explicit
 * external subpass dependencies on every render pass, and a HOST-domain buffer barrier after
 * readback copies. Barrier elision is out of scope for this slice.
 *
 * Threading: single-threaded use, matching \ref donner::gpu::Device's thread affinity.
 * Completion is tracked by polling per-submission fences from the owning thread; there are no
 * cross-thread callbacks.
 *
 * The header is pure C++ (Vulkan state lives behind a pimpl) so it is includable without the
 * Vulkan headers on any platform; the implementation compiles against the hermetic
 * Vulkan-Headers module everywhere but can only link and execute where a Vulkan loader exists
 * (Linux in CI).
 */
class VulkanDevice final : public Device {
public:
  /**
   * Creates a headless device: a VkInstance without surface extensions (enabling
   * VK_LAYER_KHRONOS_validation only when the loader enumerates it), the first physical device
   * exposing a graphics queue family, and a single VkDevice + VkQueue. Returns nullptr if no
   * Vulkan 1.1 instance or graphics-capable physical device is available.
   */
  static std::unique_ptr<VulkanDevice> Create();

  /// Destructor; waits for in-flight submissions (vkDeviceWaitIdle), drains deferred
  /// destructions, then destroys all remaining Vulkan objects in dependency-safe order.
  ~VulkanDevice() override;

  /// Serial of the most recent submission whose fence has signaled (0 if none). Polls pending
  /// fences with vkGetFenceStatus and advances a monotonic counter.
  uint64_t completedSerial() const override;

  /**
   * Blocks until \ref completedSerial reaches \p serial or \p timeoutSeconds elapses, using
   * vkWaitForFences on the pending submission fences.
   *
   * Returns false on timeout or if any Vulkan wait call fails (see \ref lastErrorForTest).
   *
   * @param serial Submission serial to wait for.
   * @param timeoutSeconds Maximum time to wait, in seconds.
   */
  bool waitForSerial(uint64_t serial, double timeoutSeconds);

  /**
   * Copies the full contents of \p buffer back to the host and returns the bytes.
   *
   * Test/readback convenience for the vertical slice, pending the buffer mapping API: validates
   * the handle (null, device identity, and generation) through the base class, then reads the
   * persistently mapped host-visible allocation. Callers must ensure relevant GPU work has
   * completed first (see \ref waitForSerial); fence completion makes device writes visible to
   * the host for HOST_COHERENT memory.
   *
   * @param buffer Buffer to read back; must be a live buffer of this device.
   */
  Result<std::vector<uint8_t>> readBackBuffer(const Buffer& buffer);

  /// Message of the most recent asynchronous Vulkan failure observed while polling or waiting
  /// on fences (e.g. VK_ERROR_DEVICE_LOST), or an empty string if none occurred.
  /// Test/diagnostic accessor.
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
  /// Constructs an empty device; \ref Create attaches the Vulkan instance/device.
  VulkanDevice();

  struct Impl;  //!< Vulkan state (handles and slot tables); defined in the .cc.
  std::unique_ptr<Impl> impl_;
};

}  // namespace donner::gpu::vulkan
