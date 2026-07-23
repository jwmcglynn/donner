#include "donner/gpu/Device.h"

#include <atomic>
#include <cmath>
#include <format>
#include <memory>
#include <utility>

#include "donner/gpu/CheckedArithmetic.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/GpuLimits.h"

namespace donner::gpu {

namespace {

/// Builds a \ref GpuError with the given category and message.
GpuError Err(GpuErrorType type, std::string message) {
  return GpuError{type, std::move(message)};
}

/// Rejects out-of-range enum values arriving through descriptors (central check so unknown
/// values cannot flow into layout or copy math).
template <typename EnumT>
Status CheckEnum(EnumT value, std::string_view fieldName) {
  if (!IsKnownEnumValue(value)) {
    return Err(
        GpuErrorType::InvalidDescriptor,
        std::format("{} has unknown enum value {}", fieldName, static_cast<uint32_t>(value)));
  }
  return OkStatus();
}

/// Rejects bitmasks containing unknown flag bits.
template <typename MaskT>
Status CheckBitmask(MaskT value, std::string_view fieldName) {
  if (!IsValidBitmask(value)) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("{} has unknown flag bits (value {})", fieldName,
                           static_cast<uint32_t>(value)));
  }
  return OkStatus();
}

/// Validates a \ref BufferDescriptor.
Status ValidateBufferDescriptor(const BufferDescriptor& descriptor) {
  if (descriptor.byteSize == 0) {
    return Err(GpuErrorType::InvalidDescriptor, "BufferDescriptor.byteSize is 0");
  }
  if (descriptor.byteSize > kMaxBufferByteSize) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("BufferDescriptor.byteSize {} exceeds kMaxBufferByteSize {}",
                           descriptor.byteSize, kMaxBufferByteSize));
  }
  if (Status status = CheckBitmask(descriptor.usage, "BufferDescriptor.usage"); status.hasError()) {
    return status;
  }
  if (descriptor.usage == BufferUsage::None) {
    return Err(GpuErrorType::InvalidDescriptor, "BufferDescriptor.usage is empty");
  }
  return OkStatus();
}

/// Validates a \ref SamplerDescriptor.
Status ValidateSamplerDescriptor(const SamplerDescriptor& descriptor) {
  if (Status status = CheckEnum(descriptor.magFilter, "SamplerDescriptor.magFilter");
      status.hasError()) {
    return status;
  }
  if (Status status = CheckEnum(descriptor.minFilter, "SamplerDescriptor.minFilter");
      status.hasError()) {
    return status;
  }
  if (Status status = CheckEnum(descriptor.addressModeU, "SamplerDescriptor.addressModeU");
      status.hasError()) {
    return status;
  }
  if (Status status = CheckEnum(descriptor.addressModeV, "SamplerDescriptor.addressModeV");
      status.hasError()) {
    return status;
  }
  return OkStatus();
}

/// Validates a \ref TextureDescriptor.
Status ValidateTextureDescriptor(const TextureDescriptor& descriptor) {
  if (Status status = CheckEnum(descriptor.format, "TextureDescriptor.format"); status.hasError()) {
    return status;
  }
  if (Status status = CheckBitmask(descriptor.usage, "TextureDescriptor.usage");
      status.hasError()) {
    return status;
  }
  if (descriptor.size.width == 0 || descriptor.size.height == 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("TextureDescriptor.size {}x{} has a zero dimension",
                           descriptor.size.width, descriptor.size.height));
  }
  if (descriptor.size.width > kMaxTextureDimension ||
      descriptor.size.height > kMaxTextureDimension) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("TextureDescriptor.size {}x{} exceeds kMaxTextureDimension {}",
                           descriptor.size.width, descriptor.size.height, kMaxTextureDimension));
  }
  if (descriptor.usage == TextureUsage::None) {
    return Err(GpuErrorType::InvalidDescriptor, "TextureDescriptor.usage is empty");
  }
  if (descriptor.sampleCount != 1) {
    return Err(GpuErrorType::Unsupported,
               std::format("TextureDescriptor.sampleCount {} is not supported; only 1 sample per "
                           "texel is available",
                           descriptor.sampleCount));
  }
  return OkStatus();
}

/// Validates a \ref BindGroupLayoutDescriptor.
Status ValidateBindGroupLayoutDescriptor(const BindGroupLayoutDescriptor& descriptor) {
  if (descriptor.entries.empty()) {
    return Err(GpuErrorType::InvalidDescriptor, "BindGroupLayoutDescriptor.entries is empty");
  }
  if (descriptor.entries.size() > kMaxBindings) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("BindGroupLayoutDescriptor has {} entries, exceeding kMaxBindings {}",
                           descriptor.entries.size(), kMaxBindings));
  }
  for (size_t i = 0; i < descriptor.entries.size(); ++i) {
    const BindGroupLayoutEntry& entry = descriptor.entries[i];
    if (entry.binding >= kMaxBindings) {
      return Err(GpuErrorType::LimitExceeded,
                 std::format("BindGroupLayoutEntry.binding {} exceeds kMaxBindings {}",
                             entry.binding, kMaxBindings));
    }
    if (Status status = CheckBitmask(entry.visibility, "BindGroupLayoutEntry.visibility");
        status.hasError()) {
      return status;
    }
    if (Status status = CheckEnum(entry.type, "BindGroupLayoutEntry.type"); status.hasError()) {
      return status;
    }
    if (entry.visibility == ShaderStage::None) {
      return Err(
          GpuErrorType::InvalidDescriptor,
          std::format("BindGroupLayoutEntry binding {} has empty visibility", entry.binding));
    }
    for (size_t j = i + 1; j < descriptor.entries.size(); ++j) {
      if (descriptor.entries[j].binding == entry.binding) {
        return Err(
            GpuErrorType::InvalidDescriptor,
            std::format("BindGroupLayoutDescriptor has duplicate binding index {}", entry.binding));
      }
    }
  }
  return OkStatus();
}

/// Validates the vertex buffer layouts of a \ref RenderPipelineDescriptor.
Status ValidateVertexBufferLayouts(const std::vector<VertexBufferLayout>& buffers) {
  if (buffers.size() > kMaxVertexBuffers) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("RenderPipelineDescriptor has {} vertex buffers, exceeding "
                           "kMaxVertexBuffers {}",
                           buffers.size(), kMaxVertexBuffers));
  }

  size_t totalAttributes = 0;
  for (const VertexBufferLayout& layout : buffers) {
    totalAttributes += layout.attributes.size();
  }
  if (totalAttributes > kMaxVertexAttributes) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("RenderPipelineDescriptor has {} vertex attributes, exceeding "
                           "kMaxVertexAttributes {}",
                           totalAttributes, kMaxVertexAttributes));
  }

  for (size_t bufferIndex = 0; bufferIndex < buffers.size(); ++bufferIndex) {
    const VertexBufferLayout& layout = buffers[bufferIndex];
    if (Status status = CheckEnum(layout.stepMode, "VertexBufferLayout.stepMode");
        status.hasError()) {
      return status;
    }
    if (layout.strideBytes == 0) {
      return Err(GpuErrorType::InvalidDescriptor,
                 std::format("VertexBufferLayout {} has strideBytes 0", bufferIndex));
    }
    if (layout.attributes.empty()) {
      return Err(GpuErrorType::InvalidDescriptor,
                 std::format("VertexBufferLayout {} has no attributes", bufferIndex));
    }
    for (const VertexAttribute& attribute : layout.attributes) {
      if (Status status = CheckEnum(attribute.format, "VertexAttribute.format");
          status.hasError()) {
        return status;
      }
      if (attribute.shaderLocation >= kMaxVertexAttributes) {
        return Err(GpuErrorType::LimitExceeded,
                   std::format("VertexAttribute shaderLocation {} exceeds kMaxVertexAttributes {}",
                               attribute.shaderLocation, kMaxVertexAttributes));
      }
      const std::optional<uint64_t> attributeEnd =
          CheckedAdd(attribute.offsetBytes, VertexFormatByteSize(attribute.format));
      if (!attributeEnd || *attributeEnd > layout.strideBytes) {
        return Err(GpuErrorType::InvalidDescriptor,
                   std::format("VertexAttribute at shaderLocation {} (offsetBytes {}, {}) "
                               "overflows strideBytes {}",
                               attribute.shaderLocation, attribute.offsetBytes,
                               VertexFormatByteSize(attribute.format), layout.strideBytes));
      }
    }
  }

  for (size_t bufferIndex = 0; bufferIndex < buffers.size(); ++bufferIndex) {
    for (const VertexAttribute& attribute : buffers[bufferIndex].attributes) {
      for (size_t otherIndex = 0; otherIndex < buffers.size(); ++otherIndex) {
        for (const VertexAttribute& other : buffers[otherIndex].attributes) {
          if (&attribute != &other && attribute.shaderLocation == other.shaderLocation) {
            return Err(GpuErrorType::InvalidDescriptor,
                       std::format("RenderPipelineDescriptor has duplicate vertex shaderLocation "
                                   "{}",
                                   attribute.shaderLocation));
          }
        }
      }
    }
  }
  return OkStatus();
}

}  // namespace

Result<uint64_t> ValidateTexelCopyInternal(const TexelCopyBufferLayout& layout,
                                           const Extent2d& copySize, TextureFormat format,
                                           std::string_view context) {
  if (copySize.width == 0 || copySize.height == 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("{}: copy size {}x{} has a zero dimension", context, copySize.width,
                           copySize.height));
  }
  const std::optional<uint64_t> rowBytes =
      CheckedMul(copySize.width, TextureFormatBytesPerTexel(format));
  if (!rowBytes) {
    return Err(GpuErrorType::OutOfBounds, std::format("{}: row byte size overflows", context));
  }
  if (layout.bytesPerRow % kTexelRowPitchAlignment != 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("{}: bytesPerRow {} is not a multiple of {}", context,
                           layout.bytesPerRow, kTexelRowPitchAlignment));
  }
  // Texel-size offset alignment is a portability rule like the 256-byte row pitch: every native
  // API this runtime targets requires copy offsets aligned to the texel block size.
  if (layout.offsetBytes % TextureFormatBytesPerTexel(format) != 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("{}: offsetBytes {} is not aligned to the {}-byte texel size", context,
                           layout.offsetBytes, TextureFormatBytesPerTexel(format)));
  }
  if (layout.bytesPerRow < *rowBytes) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("{}: bytesPerRow {} does not cover one row of {} bytes", context,
                           layout.bytesPerRow, *rowBytes));
  }
  if (layout.rowsPerImage < copySize.height) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("{}: rowsPerImage {} does not cover {} rows", context,
                           layout.rowsPerImage, copySize.height));
  }

  const std::optional<uint64_t> interiorBytes =
      CheckedMul(static_cast<uint64_t>(copySize.height) - 1, layout.bytesPerRow);
  const std::optional<uint64_t> imageBytes =
      interiorBytes ? CheckedAdd(*interiorBytes, *rowBytes) : std::nullopt;
  const std::optional<uint64_t> endByte =
      imageBytes ? CheckedAdd(layout.offsetBytes, *imageBytes) : std::nullopt;
  if (!endByte) {
    return Err(GpuErrorType::OutOfBounds,
               std::format("{}: byte range (offsetBytes {} + rows) overflows", context,
                           layout.offsetBytes));
  }
  return *endByte;
}

uint64_t Device::NextDeviceId() {
  static std::atomic<uint64_t> counter{1};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

Device::Device() : deviceId_(NextDeviceId()), aliveToken_(std::make_shared<Device*>(this)) {}

Device::~Device() {
  // Expire the device-alive token first: handles destroyed after this point release nothing.
  // Deferred backend releases still pending are dropped with the device - backend destructors
  // own the teardown of any remaining backend state (waiting for in-flight submissions first
  // where the backend executes asynchronously).
  aliveToken_.reset();
}

template <typename Tag, typename Record>
Handle<Tag> Device::allocateHandle(details::SlotTable<Record>& table, Record&& record) {
  const uint32_t slotIndex = table.allocate(std::move(record));
  return Handle<Tag>::CreateForBackend(slotIndex, table.generationOf(slotIndex), deviceId_,
                                       aliveToken_);
}

void Device::recycleRetiredSlot(ResourceKind kind, uint32_t slotIndex) {
  switch (kind) {
    case ResourceKind::Buffer:
      onDestroyResource(BufferTag::kName, slotIndex);
      buffers_.recycle(slotIndex);
      return;
    case ResourceKind::Texture:
      onDestroyResource(TextureTag::kName, slotIndex);
      textures_.recycle(slotIndex);
      return;
    case ResourceKind::TextureView:
      onDestroyResource(TextureViewTag::kName, slotIndex);
      textureViews_.recycle(slotIndex);
      return;
    case ResourceKind::Sampler:
      onDestroyResource(SamplerTag::kName, slotIndex);
      samplers_.recycle(slotIndex);
      return;
    case ResourceKind::BindGroupLayout:
      onDestroyResource(BindGroupLayoutTag::kName, slotIndex);
      bindGroupLayouts_.recycle(slotIndex);
      return;
    case ResourceKind::BindGroup:
      onDestroyResource(BindGroupTag::kName, slotIndex);
      bindGroups_.recycle(slotIndex);
      return;
    case ResourceKind::PipelineLayout:
      onDestroyResource(PipelineLayoutTag::kName, slotIndex);
      pipelineLayouts_.recycle(slotIndex);
      return;
    case ResourceKind::ShaderModule:
      onDestroyResource(ShaderModuleTag::kName, slotIndex);
      shaderModules_.recycle(slotIndex);
      return;
    case ResourceKind::RenderPipeline:
      onDestroyResource(RenderPipelineTag::kName, slotIndex);
      renderPipelines_.recycle(slotIndex);
      return;
  }
}

void Device::retireResource(ResourceKind kind, uint32_t slotIndex, uint64_t lastUseSerial) {
  if (lastUseSerial <= completedSerial()) {
    recycleRetiredSlot(kind, slotIndex);
  } else {
    pendingDestroys_.push_back(PendingDestroy{lastUseSerial, kind, slotIndex});
  }
}

void Device::poll() {
  const uint64_t completed = completedSerial();
  size_t writeIndex = 0;
  for (const PendingDestroy& pending : pendingDestroys_) {
    if (pending.readySerial <= completed) {
      recycleRetiredSlot(pending.kind, pending.slotIndex);
    } else {
      pendingDestroys_[writeIndex++] = pending;
    }
  }
  pendingDestroys_.resize(writeIndex);
}

template <typename Record, typename Tag>
Status Device::destroyResource(details::SlotTable<Record>& table, Handle<Tag>&& handle,
                               ResourceKind kind) {
  // Take ownership so the caller's handle is null afterwards. On any early return the local's
  // RAII release runs: a stale no-op after a successful retire below, an actual release on the
  // owning device when the handle belongs to a different device.
  const Handle<Tag> consumed = std::move(handle);
  poll();
  auto resolved = resolve(table, consumed, Tag::kName);
  if (resolved.hasError()) {
    return std::move(resolved).error();
  }
  const uint32_t slotIndex = consumed.slotIndex();
  const uint64_t lastUseSerial = table.lastUseOf(slotIndex);
  table.retire(slotIndex);
  retireResource(kind, slotIndex, lastUseSerial);
  return OkStatus();
}

template <typename Record>
void Device::releaseFromRaii(details::SlotTable<Record>& table, uint32_t slotIndex,
                             uint32_t generation, ResourceKind kind) {
  if (table.find(slotIndex, generation) == nullptr) {
    return;  // Already destroyed explicitly (or consumed); RAII release is a no-op.
  }
  const uint64_t lastUseSerial = table.lastUseOf(slotIndex);
  table.retire(slotIndex);
  retireResource(kind, slotIndex, lastUseSerial);
}

namespace details {

/// Defines the RAII release hook for one handle tag.
#define DONNER_GPU_DEFINE_RAII_RELEASE(TagType, tableMember, kindValue)                           \
  template <>                                                                                     \
  void ReleaseHandleFromRaii<TagType>(Device & device, uint32_t slotIndex, uint32_t generation) { \
    device.releaseFromRaii(device.tableMember, slotIndex, generation,                             \
                           Device::ResourceKind::kindValue);                                      \
  }

DONNER_GPU_DEFINE_RAII_RELEASE(BufferTag, buffers_, Buffer)
DONNER_GPU_DEFINE_RAII_RELEASE(TextureTag, textures_, Texture)
DONNER_GPU_DEFINE_RAII_RELEASE(TextureViewTag, textureViews_, TextureView)
DONNER_GPU_DEFINE_RAII_RELEASE(SamplerTag, samplers_, Sampler)
DONNER_GPU_DEFINE_RAII_RELEASE(BindGroupLayoutTag, bindGroupLayouts_, BindGroupLayout)
DONNER_GPU_DEFINE_RAII_RELEASE(BindGroupTag, bindGroups_, BindGroup)
DONNER_GPU_DEFINE_RAII_RELEASE(PipelineLayoutTag, pipelineLayouts_, PipelineLayout)
DONNER_GPU_DEFINE_RAII_RELEASE(ShaderModuleTag, shaderModules_, ShaderModule)
DONNER_GPU_DEFINE_RAII_RELEASE(RenderPipelineTag, renderPipelines_, RenderPipeline)

#undef DONNER_GPU_DEFINE_RAII_RELEASE

/// Command buffers have no backend object until submission, so a dropped unsubmitted command
/// buffer releases its recorded commands immediately with no backend notification.
template <>
void ReleaseHandleFromRaii<CommandBufferTag>(Device& device, uint32_t slotIndex,
                                             uint32_t generation) {
  if (device.commandBuffers_.find(slotIndex, generation) == nullptr) {
    return;  // Already submitted (consumed); nothing to release.
  }
  device.commandBuffers_.release(slotIndex);
}

}  // namespace details

Result<Buffer> Device::createBuffer(const BufferDescriptor& descriptor) {
  if (Status status = ValidateBufferDescriptor(descriptor); status.hasError()) {
    return std::move(status).error();
  }

  Buffer handle = allocateHandle<BufferTag>(buffers_, BufferRecord{descriptor});
  if (Status status = onCreateBuffer(handle.slotIndex(), descriptor); status.hasError()) {
    buffers_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<Texture> Device::createTexture(const TextureDescriptor& descriptor) {
  if (Status status = ValidateTextureDescriptor(descriptor); status.hasError()) {
    return std::move(status).error();
  }

  Texture handle = allocateHandle<TextureTag>(textures_, TextureRecord{descriptor});
  if (Status status = onCreateTexture(handle.slotIndex(), descriptor); status.hasError()) {
    textures_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<const Device::TextureRecord*> Device::resolveViewedTexture(
    const TextureViewRecord& viewRecord) const {
  const TextureRecord* record =
      textures_.find(viewRecord.textureIdentity.slotIndex, viewRecord.textureIdentity.generation);
  if (record == nullptr) {
    return Err(
        GpuErrorType::InvalidHandle,
        std::format("textureView \"{}\" is stale; the view's texture was destroyed "
                    "(texture slot {})",
                    viewRecord.descriptor.label.str(), viewRecord.textureIdentity.slotIndex));
  }
  return record;
}

Result<TextureView> Device::createTextureView(const Texture& texture,
                                              const TextureViewDescriptor& descriptor) {
  auto textureRecord = resolve(textures_, texture, TextureTag::kName);
  if (textureRecord.hasError()) {
    return std::move(textureRecord).error();
  }

  TextureViewRecord record{descriptor, ResourceIdentity{texture.slotIndex(), texture.generation()}};
  TextureView handle = allocateHandle<TextureViewTag>(textureViews_, std::move(record));
  if (Status status = onCreateTextureView(handle.slotIndex(), texture.slotIndex(), descriptor);
      status.hasError()) {
    textureViews_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<Sampler> Device::createSampler(const SamplerDescriptor& descriptor) {
  if (Status status = ValidateSamplerDescriptor(descriptor); status.hasError()) {
    return std::move(status).error();
  }

  Sampler handle = allocateHandle<SamplerTag>(samplers_, SamplerRecord{descriptor});
  if (Status status = onCreateSampler(handle.slotIndex(), descriptor); status.hasError()) {
    samplers_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<BindGroupLayout> Device::createBindGroupLayout(const BindGroupLayoutDescriptor& descriptor) {
  if (Status status = ValidateBindGroupLayoutDescriptor(descriptor); status.hasError()) {
    return std::move(status).error();
  }

  BindGroupLayout handle =
      allocateHandle<BindGroupLayoutTag>(bindGroupLayouts_, BindGroupLayoutRecord{descriptor});
  if (Status status = onCreateBindGroupLayout(handle.slotIndex(), descriptor); status.hasError()) {
    bindGroupLayouts_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<BindGroup> Device::createBindGroup(const BindGroupDescriptor& descriptor) {
  auto layoutRecord = resolve(bindGroupLayouts_, descriptor.layout, BindGroupLayoutTag::kName);
  if (layoutRecord.hasError()) {
    return std::move(layoutRecord).error();
  }

  const std::vector<BindGroupLayoutEntry>& layoutEntries =
      layoutRecord.result()->descriptor.entries;
  if (descriptor.entries.size() != layoutEntries.size()) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupDescriptor has {} entries but the layout requires {}",
                           descriptor.entries.size(), layoutEntries.size()));
  }

  for (const BindGroupLayoutEntry& layoutEntry : layoutEntries) {
    const BindGroupEntry* match = nullptr;
    for (const BindGroupEntry& entry : descriptor.entries) {
      if (entry.binding == layoutEntry.binding) {
        if (match != nullptr) {
          return Err(GpuErrorType::InvalidDescriptor,
                     std::format("BindGroupDescriptor has duplicate entries for binding {}",
                                 layoutEntry.binding));
        }
        match = &entry;
      }
    }
    if (match == nullptr) {
      return Err(GpuErrorType::InvalidDescriptor,
                 std::format("BindGroupDescriptor is missing an entry for layout binding {}",
                             layoutEntry.binding));
    }

    switch (layoutEntry.type) {
      case BindingType::UniformBuffer:
      case BindingType::ReadOnlyStorageBuffer: {
        const BufferBinding* bufferBinding = std::get_if<BufferBinding>(&match->resource);
        if (bufferBinding == nullptr) {
          return Err(GpuErrorType::InvalidDescriptor,
                     std::format("BindGroupEntry binding {} must bind a buffer to match the "
                                 "layout type {}",
                                 match->binding,
                                 layoutEntry.type == BindingType::UniformBuffer
                                     ? "UniformBuffer"
                                     : "ReadOnlyStorageBuffer"));
        }
        auto bufferRecord = resolve(buffers_, bufferBinding->buffer, BufferTag::kName);
        if (bufferRecord.hasError()) {
          return std::move(bufferRecord).error();
        }
        const bool isUniform = layoutEntry.type == BindingType::UniformBuffer;
        const BufferUsage requiredUsage = isUniform ? BufferUsage::Uniform : BufferUsage::Storage;
        if (!HasAllFlags(bufferRecord.result()->descriptor.usage, requiredUsage)) {
          return Err(GpuErrorType::UsageMismatch,
                     std::format("BindGroupEntry binding {}: buffer \"{}\" lacks the {} usage",
                                 match->binding, bufferRecord.result()->descriptor.label.str(),
                                 isUniform ? "Uniform" : "Storage"));
        }
        if (bufferBinding->sizeBytes == 0) {
          return Err(GpuErrorType::InvalidDescriptor,
                     std::format("BindGroupEntry binding {}: sizeBytes is 0", match->binding));
        }
        if (bufferBinding->offsetBytes % kBindingOffsetAlignment != 0) {
          return Err(
              GpuErrorType::InvalidDescriptor,
              std::format("BindGroupEntry binding {}: offsetBytes {} is not a multiple of "
                          "the {}-byte binding offset alignment",
                          match->binding, bufferBinding->offsetBytes, kBindingOffsetAlignment));
        }
        const std::optional<uint64_t> bindingEnd =
            CheckedAdd(bufferBinding->offsetBytes, bufferBinding->sizeBytes);
        if (!bindingEnd || *bindingEnd > bufferRecord.result()->descriptor.byteSize) {
          return Err(
              GpuErrorType::OutOfBounds,
              std::format("BindGroupEntry binding {}: range offsetBytes={} sizeBytes={} "
                          "does not fit in buffer \"{}\" of {} bytes",
                          match->binding, bufferBinding->offsetBytes, bufferBinding->sizeBytes,
                          bufferRecord.result()->descriptor.label.str(),
                          bufferRecord.result()->descriptor.byteSize));
        }
        break;
      }
      case BindingType::SampledTexture2dFloat: {
        const TextureViewBinding* viewBinding = std::get_if<TextureViewBinding>(&match->resource);
        if (viewBinding == nullptr) {
          return Err(GpuErrorType::InvalidDescriptor,
                     std::format("BindGroupEntry binding {} must bind a texture view to match "
                                 "the layout type SampledTexture2dFloat",
                                 match->binding));
        }
        auto viewRecord = resolve(textureViews_, viewBinding->view, TextureViewTag::kName);
        if (viewRecord.hasError()) {
          return std::move(viewRecord).error();
        }
        auto viewedTexture = resolveViewedTexture(*viewRecord.result());
        if (viewedTexture.hasError()) {
          return std::move(viewedTexture).error();
        }
        if (!HasAllFlags(viewedTexture.result()->descriptor.usage, TextureUsage::Sampled)) {
          return Err(GpuErrorType::UsageMismatch,
                     std::format("BindGroupEntry binding {}: texture view \"{}\" lacks the "
                                 "Sampled usage",
                                 match->binding, viewRecord.result()->descriptor.label.str()));
        }
        break;
      }
      case BindingType::FilteringSampler: {
        const SamplerBinding* samplerBinding = std::get_if<SamplerBinding>(&match->resource);
        if (samplerBinding == nullptr) {
          return Err(GpuErrorType::InvalidDescriptor,
                     std::format("BindGroupEntry binding {} must bind a sampler to match the "
                                 "layout type FilteringSampler",
                                 match->binding));
        }
        auto samplerRecord = resolve(samplers_, samplerBinding->sampler, SamplerTag::kName);
        if (samplerRecord.hasError()) {
          return std::move(samplerRecord).error();
        }
        break;
      }
    }
  }

  BindGroupRecord record{
      descriptor, ResourceIdentity{descriptor.layout.slotIndex(), descriptor.layout.generation()}};
  BindGroup handle = allocateHandle<BindGroupTag>(bindGroups_, std::move(record));
  if (Status status = onCreateBindGroup(handle.slotIndex(), descriptor); status.hasError()) {
    bindGroups_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<PipelineLayout> Device::createPipelineLayout(const PipelineLayoutDescriptor& descriptor) {
  if (descriptor.bindGroupLayouts.size() > kMaxBindGroups) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("PipelineLayoutDescriptor has {} bind group layouts, exceeding "
                           "kMaxBindGroups {}",
                           descriptor.bindGroupLayouts.size(), kMaxBindGroups));
  }

  std::vector<ResourceIdentity> layoutIds;
  layoutIds.reserve(descriptor.bindGroupLayouts.size());
  for (const BindGroupLayoutRef& layoutRef : descriptor.bindGroupLayouts) {
    auto layoutRecord = resolve(bindGroupLayouts_, layoutRef, BindGroupLayoutTag::kName);
    if (layoutRecord.hasError()) {
      return std::move(layoutRecord).error();
    }
    layoutIds.push_back(ResourceIdentity{layoutRef.slotIndex(), layoutRef.generation()});
  }

  PipelineLayout handle = allocateHandle<PipelineLayoutTag>(
      pipelineLayouts_, PipelineLayoutRecord{descriptor, std::move(layoutIds)});
  if (Status status = onCreatePipelineLayout(handle.slotIndex(), descriptor); status.hasError()) {
    pipelineLayouts_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<ShaderModule> Device::createShaderModule(const ShaderModuleDescriptor& descriptor) {
  if (Status status = CheckEnum(descriptor.sourceKind, "ShaderModuleDescriptor.sourceKind");
      status.hasError()) {
    return std::move(status).error();
  }
  // Exactly one source representation must be populated for the descriptor's kind: binary kinds
  // must not smuggle text and text kinds must not smuggle words, so a backend never has to guess
  // which representation is authoritative.
  if (descriptor.sourceKind == ShaderSourceKind::Spirv) {
    if (descriptor.spirvWords.empty()) {
      return Err(GpuErrorType::InvalidDescriptor,
                 "ShaderModuleDescriptor.spirvWords is empty for sourceKind Spirv");
    }
    if (!descriptor.sourceText.empty()) {
      return Err(GpuErrorType::InvalidDescriptor,
                 "ShaderModuleDescriptor.sourceText must be empty for sourceKind Spirv");
    }
  } else {
    if (descriptor.sourceText.empty()) {
      return Err(GpuErrorType::InvalidDescriptor, "ShaderModuleDescriptor.sourceText is empty");
    }
    if (!descriptor.spirvWords.empty()) {
      return Err(GpuErrorType::InvalidDescriptor,
                 "ShaderModuleDescriptor.spirvWords must be empty for text source kinds");
    }
  }

  ShaderModule handle =
      allocateHandle<ShaderModuleTag>(shaderModules_, ShaderModuleRecord{descriptor});
  if (Status status = onCreateShaderModule(handle.slotIndex(), descriptor); status.hasError()) {
    shaderModules_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<RenderPipeline> Device::createRenderPipeline(const RenderPipelineDescriptor& descriptor) {
  auto layoutRecord = resolve(pipelineLayouts_, descriptor.layout, PipelineLayoutTag::kName);
  if (layoutRecord.hasError()) {
    return std::move(layoutRecord).error();
  }
  auto vertexModule = resolve(shaderModules_, descriptor.vertex.module, ShaderModuleTag::kName);
  if (vertexModule.hasError()) {
    return std::move(vertexModule).error();
  }
  auto fragmentModule = resolve(shaderModules_, descriptor.fragment.module, ShaderModuleTag::kName);
  if (fragmentModule.hasError()) {
    return std::move(fragmentModule).error();
  }
  if (descriptor.vertex.entryPoint.empty()) {
    return Err(GpuErrorType::InvalidDescriptor,
               "RenderPipelineDescriptor.vertex.entryPoint is empty");
  }
  if (descriptor.fragment.entryPoint.empty()) {
    return Err(GpuErrorType::InvalidDescriptor,
               "RenderPipelineDescriptor.fragment.entryPoint is empty");
  }
  if (Status status = ValidateVertexBufferLayouts(descriptor.vertex.buffers); status.hasError()) {
    return std::move(status).error();
  }
  if (descriptor.fragment.targets.empty()) {
    return Err(GpuErrorType::InvalidDescriptor,
               "RenderPipelineDescriptor.fragment.targets is empty");
  }
  if (descriptor.fragment.targets.size() > kMaxColorAttachments) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("RenderPipelineDescriptor has {} color targets, exceeding "
                           "kMaxColorAttachments {}",
                           descriptor.fragment.targets.size(), kMaxColorAttachments));
  }
  for (const ColorTargetState& target : descriptor.fragment.targets) {
    if (Status status = CheckEnum(target.format, "ColorTargetState.format"); status.hasError()) {
      return std::move(status).error();
    }
    if (Status status = CheckBitmask(target.writeMask, "ColorTargetState.writeMask");
        status.hasError()) {
      return std::move(status).error();
    }
    if (target.blend) {
      for (const BlendComponent& component : {target.blend->color, target.blend->alpha}) {
        if (Status status = CheckEnum(component.srcFactor, "BlendComponent.srcFactor");
            status.hasError()) {
          return std::move(status).error();
        }
        if (Status status = CheckEnum(component.dstFactor, "BlendComponent.dstFactor");
            status.hasError()) {
          return std::move(status).error();
        }
        if (Status status = CheckEnum(component.operation, "BlendComponent.operation");
            status.hasError()) {
          return std::move(status).error();
        }
      }
    }
  }
  if (Status status = CheckEnum(descriptor.topology, "RenderPipelineDescriptor.topology");
      status.hasError()) {
    return std::move(status).error();
  }
  if (Status status = CheckEnum(descriptor.cullMode, "RenderPipelineDescriptor.cullMode");
      status.hasError()) {
    return std::move(status).error();
  }
  if (descriptor.multisampleCount != 1) {
    return Err(GpuErrorType::Unsupported,
               std::format("RenderPipelineDescriptor.multisampleCount {} is not supported; only "
                           "1 sample per pixel is available",
                           descriptor.multisampleCount));
  }

  RenderPipelineRecord record{descriptor, layoutRecord.result()->bindGroupLayoutIds};
  RenderPipeline handle = allocateHandle<RenderPipelineTag>(renderPipelines_, std::move(record));
  if (Status status = onCreateRenderPipeline(handle.slotIndex(), descriptor); status.hasError()) {
    renderPipelines_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

// Each destroy* consumes the handle: destroyResource retires the identity (so the RAII release
// of the moved-in parameter is a stale no-op), and on validation failure the dropped parameter's
// RAII release still runs against the handle's own device, so nothing leaks.

Status Device::destroyBuffer(Buffer&& buffer) {
  return destroyResource(buffers_, std::move(buffer), ResourceKind::Buffer);
}

Status Device::destroyTexture(Texture&& texture) {
  return destroyResource(textures_, std::move(texture), ResourceKind::Texture);
}

Status Device::destroyTextureView(TextureView&& textureView) {
  return destroyResource(textureViews_, std::move(textureView), ResourceKind::TextureView);
}

Status Device::destroySampler(Sampler&& sampler) {
  return destroyResource(samplers_, std::move(sampler), ResourceKind::Sampler);
}

Status Device::destroyBindGroupLayout(BindGroupLayout&& bindGroupLayout) {
  return destroyResource(bindGroupLayouts_, std::move(bindGroupLayout),
                         ResourceKind::BindGroupLayout);
}

Status Device::destroyBindGroup(BindGroup&& bindGroup) {
  return destroyResource(bindGroups_, std::move(bindGroup), ResourceKind::BindGroup);
}

Status Device::destroyPipelineLayout(PipelineLayout&& pipelineLayout) {
  return destroyResource(pipelineLayouts_, std::move(pipelineLayout), ResourceKind::PipelineLayout);
}

Status Device::destroyShaderModule(ShaderModule&& shaderModule) {
  return destroyResource(shaderModules_, std::move(shaderModule), ResourceKind::ShaderModule);
}

Status Device::destroyRenderPipeline(RenderPipeline&& renderPipeline) {
  return destroyResource(renderPipelines_, std::move(renderPipeline), ResourceKind::RenderPipeline);
}

Result<std::unique_ptr<CommandEncoder>> Device::createCommandEncoder() {
  return std::unique_ptr<CommandEncoder>(new CommandEncoder(*this));
}

Status Device::writeBuffer(const Buffer& buffer, uint64_t offsetBytes,
                           std::span<const uint8_t> data) {
  auto record = resolve(buffers_, buffer, BufferTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  if (!HasAllFlags(record.result()->descriptor.usage, BufferUsage::CopyDst)) {
    return Err(GpuErrorType::UsageMismatch,
               std::format("writeBuffer: buffer \"{}\" lacks the CopyDst usage",
                           record.result()->descriptor.label.str()));
  }
  const std::optional<uint64_t> endByte = CheckedAdd(offsetBytes, data.size());
  if (!endByte || *endByte > record.result()->descriptor.byteSize) {
    return Err(GpuErrorType::OutOfBounds,
               std::format("writeBuffer: range offsetBytes={} byteCount={} does not fit in "
                           "buffer \"{}\" of {} bytes",
                           offsetBytes, data.size(), record.result()->descriptor.label.str(),
                           record.result()->descriptor.byteSize));
  }

  return onWriteBuffer(buffer.slotIndex(), offsetBytes, data);
}

Status Device::writeTexture(const Texture& texture, std::span<const uint8_t> data,
                            const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize) {
  auto record = resolve(textures_, texture, TextureTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  const TextureDescriptor& textureDescriptor = record.result()->descriptor;
  if (!HasAllFlags(textureDescriptor.usage, TextureUsage::CopyDst)) {
    return Err(GpuErrorType::UsageMismatch,
               std::format("writeTexture: texture \"{}\" lacks the CopyDst usage",
                           textureDescriptor.label.str()));
  }
  if (writeSize.width > textureDescriptor.size.width ||
      writeSize.height > textureDescriptor.size.height) {
    return Err(GpuErrorType::OutOfBounds,
               std::format("writeTexture: write size {}x{} exceeds texture \"{}\" size {}x{}",
                           writeSize.width, writeSize.height, textureDescriptor.label.str(),
                           textureDescriptor.size.width, textureDescriptor.size.height));
  }
  Result<uint64_t> requiredEnd =
      ValidateTexelCopyInternal(dataLayout, writeSize, textureDescriptor.format, "writeTexture");
  if (requiredEnd.hasError()) {
    return std::move(requiredEnd).error();
  }
  if (requiredEnd.result() > data.size()) {
    return Err(GpuErrorType::OutOfBounds,
               std::format("writeTexture: layout requires {} bytes but data has {} bytes",
                           requiredEnd.result(), data.size()));
  }

  return onWriteTexture(texture.slotIndex(), data, dataLayout, writeSize);
}

Result<uint64_t> Device::submit(CommandBuffer commandBuffer) {
  poll();

  auto record = resolve(commandBuffers_, commandBuffer, CommandBufferTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }

  // Take ownership of the commands and consume the command buffer slot: submission is one-shot.
  const uint32_t slotIndex = commandBuffer.slotIndex();
  std::vector<Command> commands =
      std::move(commandBuffers_.findMutable(slotIndex, commandBuffer.generation())->commands);
  commandBuffers_.release(slotIndex);

  // Re-validate every recorded resource identity: a resource destroyed between recording and
  // submission fails closed here, before the backend sees the commands.
  Result<std::vector<SubmissionUse>> uses = validateSubmissionResources(commands);
  if (uses.hasError()) {
    return std::move(uses).error();
  }

  // Advance the serial only after the backend accepts the submission: a failed submit must not
  // burn a serial, or completion waiters would treat the failed work as finished. Resources are
  // marked in-use only for accepted submissions for the same reason.
  const uint64_t serial = lastSubmittedSerial_ + 1;
  if (Status status = onSubmit(serial, slotIndex, commands); status.hasError()) {
    return std::move(status).error();
  }
  lastSubmittedSerial_ = serial;
  markSubmissionUses(uses.result(), serial);
  return serial;
}

Result<std::vector<Device::SubmissionUse>> Device::validateSubmissionResources(
    std::span<const Command> commands) const {
  std::vector<SubmissionUse> uses;

  // Resolves one recorded identity against its table, failing closed when it is stale.
  const auto check = [&uses]<typename Record>(const details::SlotTable<Record>& table,
                                              const ResourceIdentity& identity, ResourceKind kind,
                                              std::string_view resourceName,
                                              std::string_view context) -> Result<const Record*> {
    const Record* record = table.find(identity.slotIndex, identity.generation);
    if (record == nullptr) {
      return GpuError{GpuErrorType::InvalidHandle,
                      std::format("submit: {} references destroyed {} (slot {})", context,
                                  resourceName, identity.slotIndex)};
    }
    uses.push_back(SubmissionUse{kind, identity.slotIndex});
    return record;
  };

  // Resolves a texture view plus the texture behind it (a view of a destroyed texture must fail
  // even if the view itself is alive).
  const auto checkViewAndTexture = [this, &check](const ResourceIdentity& viewIdentity,
                                                  std::string_view context) -> Status {
    auto viewRecord = check(textureViews_, viewIdentity, ResourceKind::TextureView,
                            TextureViewTag::kName, context);
    if (viewRecord.hasError()) {
      return std::move(viewRecord).error();
    }
    auto textureRecord = check(textures_, viewRecord.result()->textureIdentity,
                               ResourceKind::Texture, TextureTag::kName, context);
    if (textureRecord.hasError()) {
      return std::move(textureRecord).error();
    }
    return OkStatus();
  };

  for (const Command& command : commands) {
    Status status = std::visit(
        [&](const auto& typedCommand) -> Status {
          using CommandType = std::remove_cvref_t<decltype(typedCommand)>;

          if constexpr (std::is_same_v<CommandType, BeginRenderPassCommand>) {
            for (const RenderPassColorAttachment& attachment :
                 typedCommand.descriptor.colorAttachments) {
              const ResourceIdentity viewIdentity{attachment.view.slotIndex(),
                                                  attachment.view.generation()};
              if (Status attachmentStatus =
                      checkViewAndTexture(viewIdentity, "render pass attachment");
                  attachmentStatus.hasError()) {
                return attachmentStatus;
              }
            }
            return OkStatus();
          } else if constexpr (std::is_same_v<CommandType, SetPipelineCommand>) {
            auto pipelineRecord =
                check(renderPipelines_, typedCommand.pipelineId, ResourceKind::RenderPipeline,
                      RenderPipelineTag::kName, "recorded setPipeline");
            if (pipelineRecord.hasError()) {
              return std::move(pipelineRecord).error();
            }
            return OkStatus();
          } else if constexpr (std::is_same_v<CommandType, SetBindGroupCommand>) {
            auto groupRecord = check(bindGroups_, typedCommand.bindGroupId, ResourceKind::BindGroup,
                                     BindGroupTag::kName, "recorded setBindGroup");
            if (groupRecord.hasError()) {
              return std::move(groupRecord).error();
            }
            // Re-validate everything the group references so a destroyed dependency fails
            // closed even though the group object itself is alive: the layout the group was
            // created against (backends read it at encode time) and every entry resource.
            auto layoutRecord = check(
                bindGroupLayouts_, groupRecord.result()->layoutIdentity,
                ResourceKind::BindGroupLayout, BindGroupLayoutTag::kName,
                std::format("bind group \"{}\"", groupRecord.result()->descriptor.label.str()));
            if (layoutRecord.hasError()) {
              return std::move(layoutRecord).error();
            }
            for (const BindGroupEntry& entry : groupRecord.result()->descriptor.entries) {
              const std::string context =
                  std::format("bind group \"{}\" entry binding {}",
                              groupRecord.result()->descriptor.label.str(), entry.binding);
              if (const BufferBinding* bufferBinding =
                      std::get_if<BufferBinding>(&entry.resource)) {
                const ResourceIdentity bufferIdentity{bufferBinding->buffer.slotIndex(),
                                                      bufferBinding->buffer.generation()};
                auto bufferRecord = check(buffers_, bufferIdentity, ResourceKind::Buffer,
                                          BufferTag::kName, context);
                if (bufferRecord.hasError()) {
                  return std::move(bufferRecord).error();
                }
              } else if (const TextureViewBinding* viewBinding =
                             std::get_if<TextureViewBinding>(&entry.resource)) {
                const ResourceIdentity viewIdentity{viewBinding->view.slotIndex(),
                                                    viewBinding->view.generation()};
                if (Status entryStatus = checkViewAndTexture(viewIdentity, context);
                    entryStatus.hasError()) {
                  return entryStatus;
                }
              } else if (const SamplerBinding* samplerBinding =
                             std::get_if<SamplerBinding>(&entry.resource)) {
                const ResourceIdentity samplerIdentity{samplerBinding->sampler.slotIndex(),
                                                       samplerBinding->sampler.generation()};
                auto samplerRecord = check(samplers_, samplerIdentity, ResourceKind::Sampler,
                                           SamplerTag::kName, context);
                if (samplerRecord.hasError()) {
                  return std::move(samplerRecord).error();
                }
              }
            }
            return OkStatus();
          } else if constexpr (std::is_same_v<CommandType, SetVertexBufferCommand>) {
            auto bufferRecord = check(buffers_, typedCommand.bufferId, ResourceKind::Buffer,
                                      BufferTag::kName, "recorded setVertexBuffer");
            if (bufferRecord.hasError()) {
              return std::move(bufferRecord).error();
            }
            return OkStatus();
          } else if constexpr (std::is_same_v<CommandType, CopyTextureToBufferCommand>) {
            auto textureRecord = check(textures_, typedCommand.textureId, ResourceKind::Texture,
                                       TextureTag::kName, "recorded copyTextureToBuffer");
            if (textureRecord.hasError()) {
              return std::move(textureRecord).error();
            }
            auto bufferRecord = check(buffers_, typedCommand.bufferId, ResourceKind::Buffer,
                                      BufferTag::kName, "recorded copyTextureToBuffer");
            if (bufferRecord.hasError()) {
              return std::move(bufferRecord).error();
            }
            return OkStatus();
          } else if constexpr (std::is_same_v<CommandType, CopyTextureToTextureCommand>) {
            auto sourceRecord = check(textures_, typedCommand.textureSrcId, ResourceKind::Texture,
                                      TextureTag::kName, "recorded copyTextureToTexture");
            if (sourceRecord.hasError()) {
              return std::move(sourceRecord).error();
            }
            auto destinationRecord =
                check(textures_, typedCommand.textureDstId, ResourceKind::Texture,
                      TextureTag::kName, "recorded copyTextureToTexture");
            if (destinationRecord.hasError()) {
              return std::move(destinationRecord).error();
            }
            return OkStatus();
          } else {
            return OkStatus();
          }
        },
        command);
    if (status.hasError()) {
      return std::move(status).error();
    }
  }
  return uses;
}

void Device::markSubmissionUses(std::span<const SubmissionUse> uses, uint64_t submissionSerial) {
  for (const SubmissionUse& use : uses) {
    switch (use.kind) {
      case ResourceKind::Buffer: buffers_.markUsed(use.slotIndex, submissionSerial); break;
      case ResourceKind::Texture: textures_.markUsed(use.slotIndex, submissionSerial); break;
      case ResourceKind::TextureView:
        textureViews_.markUsed(use.slotIndex, submissionSerial);
        break;
      case ResourceKind::Sampler: samplers_.markUsed(use.slotIndex, submissionSerial); break;
      case ResourceKind::BindGroupLayout:
        bindGroupLayouts_.markUsed(use.slotIndex, submissionSerial);
        break;
      case ResourceKind::BindGroup: bindGroups_.markUsed(use.slotIndex, submissionSerial); break;
      case ResourceKind::PipelineLayout:
        pipelineLayouts_.markUsed(use.slotIndex, submissionSerial);
        break;
      case ResourceKind::ShaderModule:
        shaderModules_.markUsed(use.slotIndex, submissionSerial);
        break;
      case ResourceKind::RenderPipeline:
        renderPipelines_.markUsed(use.slotIndex, submissionSerial);
        break;
    }
  }
}

Status Device::validateBufferHandleForBackend(const Buffer& buffer) const {
  auto record = resolve(buffers_, buffer, BufferTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  return OkStatus();
}

CommandBuffer Device::registerCommandBuffer(std::vector<Command>&& commands) {
  return allocateHandle<CommandBufferTag>(commandBuffers_,
                                          CommandBufferRecord{std::move(commands)});
}

}  // namespace donner::gpu
