#pragma once
/// @file
/// \c donner::gpu::Device - the abstract GPU device with shared fail-closed validation.

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/Commands.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/GpuResult.h"
#include "donner/gpu/Handles.h"

namespace donner::gpu {

class CommandEncoder;

namespace details {

/**
 * Generation-checked slot storage for one resource type.
 *
 * Slots are reused through a free list; every release bumps the slot's generation so stale
 * handles referencing the previous occupant fail validation.
 *
 * @tparam Record Validated per-resource record stored in each slot.
 */
template <typename Record>
class SlotTable {
public:
  /**
   * Stores \p record in a free slot (reusing released slots first) and returns its index.
   *
   * @param record Record to store.
   */
  uint32_t allocate(Record&& record) {
    if (!freeList_.empty()) {
      const uint32_t slotIndex = freeList_.back();
      freeList_.pop_back();
      Slot& slot = slots_[slotIndex];
      slot.alive = true;
      slot.record = std::move(record);
      return slotIndex;
    }

    slots_.push_back(Slot{1, true, std::move(record)});
    return static_cast<uint32_t>(slots_.size() - 1);
  }

  /**
   * Returns the record at \p slotIndex if it is alive and \p generation is current, otherwise
   * nullptr.
   *
   * @param slotIndex Slot index.
   * @param generation Generation the caller's handle carries.
   */
  const Record* find(uint32_t slotIndex, uint32_t generation) const {
    if (slotIndex >= slots_.size()) {
      return nullptr;
    }
    const Slot& slot = slots_[slotIndex];
    if (!slot.alive || slot.generation != generation) {
      return nullptr;
    }
    return &slot.record.value();
  }

  /**
   * Mutable variant of \ref find, for consuming stored state (e.g. submitting a command buffer).
   *
   * @param slotIndex Slot index.
   * @param generation Generation the caller's handle carries.
   */
  Record* findMutable(uint32_t slotIndex, uint32_t generation) {
    return const_cast<Record*>(std::as_const(*this).find(slotIndex, generation));
  }

  /**
   * Current generation of \p slotIndex.
   *
   * @param slotIndex Slot index; must be in range.
   */
  uint32_t generationOf(uint32_t slotIndex) const { return slots_[slotIndex].generation; }

  /**
   * Releases the record at \p slotIndex: marks the slot dead, bumps its generation so remaining
   * identifiers fail closed, and queues the slot for reuse.
   *
   * @param slotIndex Slot index; must refer to an alive slot.
   */
  void release(uint32_t slotIndex) {
    Slot& slot = slots_[slotIndex];
    slot.alive = false;
    slot.record.reset();
    ++slot.generation;
    freeList_.push_back(slotIndex);
  }

private:
  /// One slot: generation counter plus the stored record while alive.
  struct Slot {
    uint32_t generation = 1;       //!< Bumped on release; a handle matches only its generation.
    bool alive = false;            //!< True while the slot holds a live resource.
    std::optional<Record> record;  //!< Stored record; empty while dead.
  };

  std::vector<Slot> slots_;
  std::vector<uint32_t> freeList_;
};

}  // namespace details

/**
 * Internal: validates a texel copy layout against a copy extent and format, and returns the
 * exclusive end byte offset the copy requires in the buffer or host memory it describes. Shared
 * by `Device::writeTexture` and `CommandEncoder::copyTextureToBuffer`; not part of the public
 * API.
 *
 * @param layout Row layout to validate (256-aligned `bytesPerRow` covering one row, and
 *   `rowsPerImage` covering the copy height).
 * @param copySize Copy extent in texels.
 * @param format Texel format of the texture side of the copy.
 * @param context Operation name for diagnostics.
 */
Result<uint64_t> ValidateTexelCopyInternal(const TexelCopyBufferLayout& layout,
                                           const Extent2d& copySize, TextureFormat format,
                                           std::string_view context);

/**
 * Abstract GPU device: resource creation, queue writes, and submission with shared fail-closed
 * validation (design 0053 "Proposed Architecture", "Validation layer").
 *
 * The public API is non-virtual and validates every descriptor, handle, and byte range before
 * delegating to protected `on*` virtuals (template-method pattern), so every backend - the
 * deterministic \ref RecordingDevice today, platform backends in later packets - inherits
 * identical fail-closed behavior. Validation runs in release builds too: invalid input returns
 * a \ref GpuError, never asserts.
 *
 * Enforced limits are documented in GpuLimits.h. Handle validation checks device identity
 * (\ref GpuErrorType::DeviceMismatch) and slot generation (\ref GpuErrorType::InvalidHandle), so
 * destroyed or foreign handles fail before reaching a backend.
 *
 * Lifetime: resources are freed by explicit `destroy*` calls; device teardown frees everything
 * that remains (handles are not RAII in this packet, see Handles.h). Handles must not be used
 * after their device is destroyed.
 *
 * Thread affinity: a Device and everything created from it must be used from one thread at a
 * time, matching the async-renderer worker ownership model.
 */
class Device {
public:
  /// Destructor; frees all remaining resources.
  virtual ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;

  /// Process-unique identity of this device (starts at 1, never reused). Baked into every handle
  /// for cross-device validation.
  uint64_t deviceId() const { return deviceId_; }

  /**
   * Creates a buffer. Fails closed on zero or oversized `byteSize` or empty usage.
   *
   * @param descriptor Validated buffer descriptor.
   */
  Result<Buffer> createBuffer(const BufferDescriptor& descriptor);

  /**
   * Creates a 2D texture. Fails closed on zero or oversized extents, empty usage, or
   * `sampleCount != 1`.
   *
   * @param descriptor Validated texture descriptor.
   */
  Result<Texture> createTexture(const TextureDescriptor& descriptor);

  /**
   * Creates a view of \p texture covering the whole texture.
   *
   * @param texture Texture to view; must be a live handle of this device.
   * @param descriptor Validated view descriptor.
   */
  Result<TextureView> createTextureView(const Texture& texture,
                                        const TextureViewDescriptor& descriptor);

  /**
   * Creates a sampler.
   *
   * @param descriptor Validated sampler descriptor.
   */
  Result<Sampler> createSampler(const SamplerDescriptor& descriptor);

  /**
   * Creates a bind group layout. Fails closed on duplicate binding indices, out-of-limit binding
   * counts or indices, or empty per-entry visibility.
   *
   * @param descriptor Validated layout descriptor.
   */
  Result<BindGroupLayout> createBindGroupLayout(const BindGroupLayoutDescriptor& descriptor);

  /**
   * Creates a bind group. Fails closed unless every layout binding is matched by exactly one
   * entry whose resource is live, belongs to this device, matches the layout's
   * \ref BindingType, and carries the usage that binding type requires.
   *
   * @param descriptor Validated bind group descriptor.
   */
  Result<BindGroup> createBindGroup(const BindGroupDescriptor& descriptor);

  /**
   * Creates a pipeline layout. Fails closed on out-of-limit group counts or invalid layout
   * references.
   *
   * @param descriptor Validated pipeline layout descriptor.
   */
  Result<PipelineLayout> createPipelineLayout(const PipelineLayoutDescriptor& descriptor);

  /**
   * Creates a shader module from trusted generated source. Fails closed on empty source text.
   *
   * @param descriptor Validated shader module descriptor.
   */
  Result<ShaderModule> createShaderModule(const ShaderModuleDescriptor& descriptor);

  /**
   * Creates a render pipeline. Fails closed on invalid layout/module references, empty entry
   * points, vertex attributes that overflow their stride, duplicate shader locations, empty or
   * out-of-limit target lists, or `multisampleCount != 1`.
   *
   * @param descriptor Validated pipeline descriptor.
   */
  Result<RenderPipeline> createRenderPipeline(const RenderPipelineDescriptor& descriptor);

  /// Destroys a buffer; the handle and all references to it become stale.
  /// @param buffer Handle to destroy.
  Status destroyBuffer(const Buffer& buffer);
  /// Destroys a texture; the handle and all references to it become stale.
  /// @param texture Handle to destroy.
  Status destroyTexture(const Texture& texture);
  /// Destroys a texture view; the handle and all references to it become stale.
  /// @param textureView Handle to destroy.
  Status destroyTextureView(const TextureView& textureView);
  /// Destroys a sampler; the handle and all references to it become stale.
  /// @param sampler Handle to destroy.
  Status destroySampler(const Sampler& sampler);
  /// Destroys a bind group layout; the handle and all references to it become stale.
  /// @param bindGroupLayout Handle to destroy.
  Status destroyBindGroupLayout(const BindGroupLayout& bindGroupLayout);
  /// Destroys a bind group; the handle and all references to it become stale.
  /// @param bindGroup Handle to destroy.
  Status destroyBindGroup(const BindGroup& bindGroup);
  /// Destroys a pipeline layout; the handle and all references to it become stale.
  /// @param pipelineLayout Handle to destroy.
  Status destroyPipelineLayout(const PipelineLayout& pipelineLayout);
  /// Destroys a shader module; the handle and all references to it become stale.
  /// @param shaderModule Handle to destroy.
  Status destroyShaderModule(const ShaderModule& shaderModule);
  /// Destroys a render pipeline; the handle and all references to it become stale.
  /// @param renderPipeline Handle to destroy.
  Status destroyRenderPipeline(const RenderPipeline& renderPipeline);

  /// Creates a command encoder recording against this device. The encoder must not outlive the
  /// device.
  Result<std::unique_ptr<CommandEncoder>> createCommandEncoder();

  /**
   * Writes \p data into \p buffer at \p offsetBytes. Fails closed if the range does not fit
   * (checked arithmetic) or the buffer lacks \ref BufferUsage::CopyDst.
   *
   * @param buffer Destination buffer.
   * @param offsetBytes Destination byte offset.
   * @param data Payload bytes.
   */
  Status writeBuffer(const Buffer& buffer, uint64_t offsetBytes, std::span<const uint8_t> data);

  /**
   * Writes texel rows from \p data into \p texture starting at texel (0, 0). Fails closed unless
   * the texture has \ref TextureUsage::CopyDst, \p dataLayout is 256-aligned and covers
   * \p writeSize, \p writeSize fits in the texture, and the described rows fit inside \p data
   * (checked arithmetic).
   *
   * @param texture Destination texture.
   * @param data Payload bytes laid out per \p dataLayout.
   * @param dataLayout Row layout of \p data.
   * @param writeSize Extent to write in texels.
   */
  Status writeTexture(const Texture& texture, std::span<const uint8_t> data,
                      const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize);

  /**
   * Submits a finished command buffer, consuming it, and returns the assigned submission serial.
   * Serials start at 1 and increase by 1 per submission.
   *
   * @param commandBuffer Command buffer to submit; consumed even on failure.
   */
  Result<uint64_t> submit(CommandBuffer commandBuffer);

  /// Serial assigned to the most recent submission (0 if none yet).
  uint64_t lastSubmittedSerial() const { return lastSubmittedSerial_; }

  /// Serial of the most recent submission the backend has finished executing (0 if none). The
  /// recording backend completes instantly, so this equals \ref lastSubmittedSerial there.
  virtual uint64_t completedSerial() const = 0;

protected:
  /// Constructor for backends; assigns the process-unique device identity.
  Device();

  /// Backend hook: a buffer passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) = 0;
  /// Backend hook: a texture passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) = 0;
  /// Backend hook: a texture view passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource.
  /// @param textureSlotIndex Slot index of the viewed texture.
  /// @param descriptor Validated descriptor.
  virtual Status onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                                     const TextureViewDescriptor& descriptor) = 0;
  /// Backend hook: a sampler passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) = 0;
  /// Backend hook: a bind group layout passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateBindGroupLayout(uint32_t slotIndex,
                                         const BindGroupLayoutDescriptor& descriptor) = 0;
  /// Backend hook: a bind group passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) = 0;
  /// Backend hook: a pipeline layout passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreatePipelineLayout(uint32_t slotIndex,
                                        const PipelineLayoutDescriptor& descriptor) = 0;
  /// Backend hook: a shader module passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateShaderModule(uint32_t slotIndex,
                                      const ShaderModuleDescriptor& descriptor) = 0;
  /// Backend hook: a render pipeline passed validation and occupies \p slotIndex.
  /// @param slotIndex Slot index of the new resource. @param descriptor Validated descriptor.
  virtual Status onCreateRenderPipeline(uint32_t slotIndex,
                                        const RenderPipelineDescriptor& descriptor) = 0;

  /// Backend hook: a validated resource was destroyed.
  /// @param resourceName Resource type name, e.g. `"buffer"`.
  /// @param slotIndex Slot index of the destroyed resource.
  virtual void onDestroyResource(std::string_view resourceName, uint32_t slotIndex) = 0;

  /// Backend hook: a validated buffer write.
  /// @param slotIndex Destination buffer slot. @param offsetBytes Destination byte offset.
  /// @param data Payload bytes.
  virtual Status onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                               std::span<const uint8_t> data) = 0;
  /// Backend hook: a validated texture write.
  /// @param slotIndex Destination texture slot. @param data Payload bytes.
  /// @param dataLayout Row layout of \p data. @param writeSize Extent written in texels.
  virtual Status onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                                const TexelCopyBufferLayout& dataLayout,
                                const Extent2d& writeSize) = 0;

  /// Backend hook: a validated command buffer was submitted.
  /// @param submissionSerial Serial assigned to this submission.
  /// @param commandBufferSlotIndex Slot the command buffer occupied before being consumed.
  /// @param commands Validated commands, in recording order.
  virtual Status onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                          std::span<const Command> commands) = 0;

private:
  friend class CommandEncoder;

  /// Validated per-buffer state.
  struct BufferRecord {
    BufferDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Validated per-texture state.
  struct TextureRecord {
    TextureDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Validated per-view state, including a snapshot of the viewed texture's properties used by
  /// render pass and bind group validation.
  struct TextureViewRecord {
    TextureViewDescriptor descriptor;                         //!< Creation descriptor.
    uint32_t textureSlot = 0;                                 //!< Slot of the viewed texture.
    TextureFormat textureFormat = TextureFormat::RGBA8Unorm;  //!< Viewed texture format.
    Extent2d textureSize;                                     //!< Viewed texture extent.
    TextureUsage textureUsage = TextureUsage::None;           //!< Viewed texture usage.
  };
  /// Validated per-sampler state.
  struct SamplerRecord {
    SamplerDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Validated per-layout state.
  struct BindGroupLayoutRecord {
    BindGroupLayoutDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Identity of a bind group layout at a point in time: slot plus generation, so slot reuse
  /// cannot alias two different layouts.
  struct LayoutIdentity {
    uint32_t slotIndex = 0;   //!< Layout slot index.
    uint32_t generation = 0;  //!< Layout generation at capture time.

    /// Equality operator. @param other Identity to compare against.
    bool operator==(const LayoutIdentity& other) const = default;
  };
  /// Validated per-bind-group state.
  struct BindGroupRecord {
    BindGroupDescriptor descriptor;  //!< Creation descriptor.
    LayoutIdentity layoutIdentity;   //!< Identity of the layout this group was created against.
  };
  /// Validated per-pipeline-layout state.
  struct PipelineLayoutRecord {
    PipelineLayoutDescriptor descriptor;             //!< Creation descriptor.
    std::vector<LayoutIdentity> bindGroupLayoutIds;  //!< Layout identities, by group index.
  };
  /// Validated per-shader-module state.
  struct ShaderModuleRecord {
    ShaderModuleDescriptor descriptor;  //!< Creation descriptor.
  };
  /// Validated per-pipeline state used for draw-time compatibility checks.
  struct RenderPipelineRecord {
    RenderPipelineDescriptor descriptor;             //!< Creation descriptor.
    std::vector<LayoutIdentity> bindGroupLayoutIds;  //!< Pipeline layout's group identities.
  };
  /// A finished, not-yet-submitted command buffer.
  struct CommandBufferRecord {
    std::vector<Command> commands;  //!< Validated commands in recording order.
  };

  /**
   * Resolves a handle or handle reference against \p table: null handles and stale generations
   * return \ref GpuErrorType::InvalidHandle, foreign devices return
   * \ref GpuErrorType::DeviceMismatch.
   *
   * @param table Table for the handle's resource type.
   * @param handleLike Handle or \ref HandleRef to resolve.
   * @param resourceName Resource type name for diagnostics.
   */
  template <typename Record, typename HandleLike>
  Result<const Record*> resolve(const details::SlotTable<Record>& table,
                                const HandleLike& handleLike, std::string_view resourceName) const;

  /// Allocates a slot in \p table and mints a handle carrying this device's identity.
  /// @param table Destination table. @param record Validated record to store.
  template <typename Tag, typename Record>
  Handle<Tag> allocateHandle(details::SlotTable<Record>& table, Record&& record);

  /// Shared implementation of the `destroy*` methods.
  /// @param table Table for the handle's resource type. @param handle Handle to destroy.
  template <typename Record, typename Tag>
  Status destroyResource(details::SlotTable<Record>& table, const Handle<Tag>& handle);

  /// Registers a finished command stream from an encoder and returns its handle.
  /// @param commands Validated commands in recording order.
  CommandBuffer registerCommandBuffer(std::vector<Command>&& commands);

  /// Returns the next process-unique device id.
  static uint64_t NextDeviceId();

  uint64_t deviceId_ = 0;
  uint64_t lastSubmittedSerial_ = 0;

  details::SlotTable<BufferRecord> buffers_;
  details::SlotTable<TextureRecord> textures_;
  details::SlotTable<TextureViewRecord> textureViews_;
  details::SlotTable<SamplerRecord> samplers_;
  details::SlotTable<BindGroupLayoutRecord> bindGroupLayouts_;
  details::SlotTable<BindGroupRecord> bindGroups_;
  details::SlotTable<PipelineLayoutRecord> pipelineLayouts_;
  details::SlotTable<ShaderModuleRecord> shaderModules_;
  details::SlotTable<RenderPipelineRecord> renderPipelines_;
  details::SlotTable<CommandBufferRecord> commandBuffers_;
};

template <typename Record, typename HandleLike>
Result<const Record*> Device::resolve(const details::SlotTable<Record>& table,
                                      const HandleLike& handleLike,
                                      std::string_view resourceName) const {
  if (!handleLike.isValid()) {
    return GpuError{
        GpuErrorType::InvalidHandle,
        std::format("{} handle is null (default-constructed or moved-from)", resourceName)};
  }
  if (handleLike.deviceId() != deviceId_) {
    return GpuError{GpuErrorType::DeviceMismatch,
                    std::format("{} handle belongs to device {} but was used with device {}",
                                resourceName, handleLike.deviceId(), deviceId_)};
  }
  const Record* record = table.find(handleLike.slotIndex(), handleLike.generation());
  if (record == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("{} handle (slot {}) is stale; the resource was destroyed",
                                resourceName, handleLike.slotIndex())};
  }
  return record;
}

}  // namespace donner::gpu
