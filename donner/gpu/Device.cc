#include "donner/gpu/Device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <span>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>

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

/// Keeps floating-point filter intermediates out of rendering and presentation.
Status ValidateRenderTargetFormat(TextureFormat format) {
  if (format == TextureFormat::RGBA32Float) {
    return Err(GpuErrorType::Unsupported,
               "RGBA32Float supports sampled/storage textures and copies, not render targets");
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
  if (HasAllFlags(descriptor.usage, TextureUsage::RenderAttachment)) {
    if (Status status = ValidateRenderTargetFormat(descriptor.format); status.hasError()) {
      return status;
    }
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
    // Checked for every entry, not just storage-texture ones: an out-of-range cast in an ignored
    // field would otherwise survive validation and reach a backend's format translation the day
    // the field starts being read.
    if (Status status =
            CheckEnum(entry.storageTextureFormat, "BindGroupLayoutEntry.storageTextureFormat");
        status.hasError()) {
      return status;
    }
    if (entry.visibility == ShaderStage::None) {
      return Err(
          GpuErrorType::InvalidDescriptor,
          std::format("BindGroupLayoutEntry binding {} has empty visibility", entry.binding));
    }
    if (entry.type == BindingType::WriteOnlyStorageTexture2d &&
        entry.visibility != ShaderStage::Compute) {
      return Err(GpuErrorType::Unsupported,
                 std::format("BindGroupLayoutEntry binding {}: storage texture writes are "
                             "compute-only",
                             entry.binding));
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

/// Validates one color target's blend state.
/// @param blend Blend state to check.
Status ValidateBlendState(const BlendState& blend) {
  for (const BlendComponent& component : {blend.color, blend.alpha}) {
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
  return OkStatus();
}

/// Validates the color targets of a \ref RenderPipelineDescriptor's fragment state.
/// @param targets Color targets to check.
Status ValidateColorTargets(const std::vector<ColorTargetState>& targets) {
  if (targets.empty()) {
    return Err(GpuErrorType::InvalidDescriptor,
               "RenderPipelineDescriptor.fragment.targets is empty");
  }
  if (targets.size() > kMaxColorAttachments) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("RenderPipelineDescriptor has {} color targets, exceeding "
                           "kMaxColorAttachments {}",
                           targets.size(), kMaxColorAttachments));
  }
  for (const ColorTargetState& target : targets) {
    if (Status status = CheckEnum(target.format, "ColorTargetState.format"); status.hasError()) {
      return std::move(status).error();
    }
    if (Status status = ValidateRenderTargetFormat(target.format); status.hasError()) {
      return status;
    }
    if (Status status = CheckBitmask(target.writeMask, "ColorTargetState.writeMask");
        status.hasError()) {
      return std::move(status).error();
    }
    if (target.blend) {
      if (Status status = ValidateBlendState(*target.blend); status.hasError()) {
        return std::move(status).error();
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

/// Validates the parts of a \ref RenderPipelineDescriptor that depend only on the descriptor
/// itself, not on any resolved layout or module record.
/// @param descriptor Descriptor to check.
Status ValidateRenderPipelineDescriptor(const RenderPipelineDescriptor& descriptor) {
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
  if (Status status = ValidateColorTargets(descriptor.fragment.targets); status.hasError()) {
    return std::move(status).error();
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
  return OkStatus();
}

Status ValidateShaderBufferLocation(const ShaderBufferBindingInfo& info) {
  if (info.entryPoint.empty() || info.group >= kMaxBindGroups || info.binding >= kMaxBindings) {
    return Err(GpuErrorType::InvalidDescriptor,
               "Shader buffer metadata has an invalid entry point or binding location");
  }
  switch (info.stage) {
    case ShaderStage::Vertex:
    case ShaderStage::Fragment:
    case ShaderStage::Compute: return OkStatus();
    default:
      return Err(GpuErrorType::InvalidDescriptor,
                 "Shader buffer metadata requires one shader stage");
  }
}

Status ValidateShaderBufferRange(const ShaderBufferBindingInfo& info) {
  if (info.type != BindingType::UniformBuffer && info.type != BindingType::ReadOnlyStorageBuffer) {
    return Err(GpuErrorType::InvalidDescriptor,
               "Shader buffer metadata has a non-buffer binding type");
  }
  if (info.minSizeBytes == 0 || info.minSizeBytes > kMaxBufferByteSize) {
    return Err(GpuErrorType::InvalidDescriptor,
               "Shader buffer metadata has an invalid minimum size");
  }
  if (info.runtimeArrayStrideBytes != 0 && (info.type != BindingType::ReadOnlyStorageBuffer ||
                                            info.minSizeBytes != info.runtimeArrayStrideBytes)) {
    return Err(GpuErrorType::InvalidDescriptor,
               "Shader runtime-array metadata requires one storage element as its minimum");
  }
  return OkStatus();
}

Status ValidateShaderBufferMetadata(const ShaderModuleDescriptor& descriptor) {
  if (!descriptor.bufferBindings) {
    return OkStatus();
  }
  for (const ShaderBufferBindingInfo& info : *descriptor.bufferBindings) {
    if (Status status = ValidateShaderBufferLocation(info); status.hasError()) {
      return status;
    }
    if (Status status = ValidateShaderBufferRange(info); status.hasError()) {
      return status;
    }
  }
  return OkStatus();
}

/// Validates a declared workgroup size against the per-dimension and total-invocation caps.
Status ValidateWorkgroupSize(const WorkgroupSize& size) {
  if (size.x == 0 || size.y == 0 || size.z == 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("ComputePipelineDescriptor.workgroupSize {}x{}x{} has a zero dimension",
                           size.x, size.y, size.z));
  }
  if (size.x > kMaxComputeWorkgroupSizeXY || size.y > kMaxComputeWorkgroupSizeXY ||
      size.z > kMaxComputeWorkgroupSizeZ) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("ComputePipelineDescriptor.workgroupSize {}x{}x{} exceeds the "
                           "per-dimension caps ({}, {}, {})",
                           size.x, size.y, size.z, kMaxComputeWorkgroupSizeXY,
                           kMaxComputeWorkgroupSizeXY, kMaxComputeWorkgroupSizeZ));
  }
  // Multiplied in 64 bits so a product that overflows 32 bits is rejected rather than wrapping
  // under the cap.
  const uint64_t invocations = uint64_t{size.x} * size.y * size.z;
  if (invocations > kMaxComputeInvocationsPerWorkgroup) {
    return Err(
        GpuErrorType::LimitExceeded,
        std::format("ComputePipelineDescriptor.workgroupSize {}x{}x{} declares {} "
                    "invocations, exceeding kMaxComputeInvocationsPerWorkgroup {}",
                    size.x, size.y, size.z, invocations, kMaxComputeInvocationsPerWorkgroup));
  }
  return OkStatus();
}

/// Rejects a compute pipeline whose declared workgroup size the shader module does not back.
///
/// Metal dispatches with the descriptor's size while every other backend uses the size compiled
/// into the shader, so a disagreement is a different invocation grid per backend rather than an
/// error anything reports.
///
/// @param descriptor Pipeline descriptor being created.
/// @param moduleDescriptor Descriptor the referenced shader module was created with.
Status ValidateWorkgroupSizeAgainstModule(const ComputePipelineDescriptor& descriptor,
                                          const ShaderModuleDescriptor& moduleDescriptor) {
  if (moduleDescriptor.computeEntryPoints.empty()) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("shader module \"{}\" declares no compute entry points, so the "
                           "workgroup size of \"{}\" cannot be checked against the shader",
                           moduleDescriptor.label.str(), descriptor.compute.entryPoint.str()));
  }
  for (const ComputeEntryPointInfo& entryPoint : moduleDescriptor.computeEntryPoints) {
    if (entryPoint.name != descriptor.compute.entryPoint) {
      continue;
    }
    if (!(entryPoint.workgroupSize == descriptor.workgroupSize)) {
      std::ostringstream sizes;
      sizes << descriptor.workgroupSize << " disagrees with the " << entryPoint.workgroupSize;
      return Err(GpuErrorType::InvalidDescriptor,
                 std::format("ComputePipelineDescriptor.workgroupSize {} that shader module "
                             "\"{}\" declares for entry point \"{}\"",
                             sizes.str(), moduleDescriptor.label.str(),
                             descriptor.compute.entryPoint.str()));
    }
    return OkStatus();
  }
  return Err(GpuErrorType::InvalidDescriptor,
             std::format("shader module \"{}\" does not declare a compute entry point named "
                         "\"{}\"",
                         moduleDescriptor.label.str(), descriptor.compute.entryPoint.str()));
}

/// Draws \p commandBuffers count under \ref DeviceObserver::onSubmitted's rule: every draw
/// command, and every indexed draw command that is not empty.
/// @param commandBuffers Command buffers of one accepted submission.
uint64_t CountDraws(std::span<const SubmittedCommandBuffer> commandBuffers) {
  uint64_t draws = 0;
  for (const SubmittedCommandBuffer& commandBuffer : commandBuffers) {
    for (const Command& command : commandBuffer.commands) {
      if (std::holds_alternative<DrawCommand>(command)) {
        ++draws;
      } else if (const auto* indexed = std::get_if<DrawIndexedCommand>(&command);
                 indexed != nullptr && !IsEmptyIndexedDraw(*indexed)) {
        ++draws;
      }
    }
  }
  return draws;
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

void Device::markLostAfterWaitTimeout(DeviceLostWaitSite site, std::chrono::milliseconds elapsed,
                                      const char* reason) const {
  if (DeclareDeviceLostAfterWaitTimeout(*lostState_, site, elapsed)) {
    LogDeclaredDeviceLoss(reason);
  }
}

void Device::adoptLostState(std::shared_ptr<DeviceLostState> state) {
  if (state) {
    lostState_ = std::move(state);
  }
}

Device::~Device() {
  // Expire the device-alive token first: handles destroyed after this point release nothing.
  // Deferred backend releases still pending are dropped with the device - backend destructors
  // own the teardown of any remaining backend state (waiting for in-flight submissions first
  // where the backend executes asynchronously).
  aliveToken_.reset();
  for (uint32_t slotIndex = 0; slotIndex < textureShares_.size(); ++slotIndex) {
    releaseTextureShare(slotIndex);
  }
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
      releaseTextureRegistration(slotIndex);
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
    case ResourceKind::ComputePipeline:
      onDestroyResource(ComputePipelineTag::kName, slotIndex);
      computePipelines_.recycle(slotIndex);
      return;
  }
}

void Device::reportTextureRelease(bool released) const {
  if (released && observer_ != nullptr) {
    observer_->onTextureReleased();
  }
}

void Device::onRetireBuffer(uint32_t) {}

void Device::onRetireTexture(uint32_t) {}

void Device::retireResource(ResourceKind kind, uint32_t slotIndex, uint64_t lastUseSerial,
                            bool releasesTextureAllocation) {
  if (kind == ResourceKind::Buffer) {
    // A mapping names bytes of this buffer, so it cannot outlive the allocation. The handle stays
    // resolvable and fails closed on use, which tells its holder what happened; keeping the
    // buffer alive instead would let a reader that never unmaps pin it forever.
    bufferMappings_.forEachLive([&](MappingRecord& mappingRecord) {
      if (mappingRecord.bufferSlotIndex == slotIndex) {
        mappingRecord.bufferRetired = true;
      }
    });
    onRetireBuffer(slotIndex);
  } else if (kind == ResourceKind::Texture) {
    onRetireTexture(slotIndex);
    releaseTextureShare(slotIndex);
  }
  if (lastUseSerial <= completedSerial()) {
    recycleRetiredSlot(kind, slotIndex);
    reportTextureRelease(releasesTextureAllocation);
  } else {
    pendingDestroys_.push_back(
        PendingDestroy{lastUseSerial, kind, slotIndex, releasesTextureAllocation});
  }
}

void Device::poll() {
  const uint64_t completed = completedSerial();
  size_t writeIndex = 0;
  for (const PendingDestroy& pending : pendingDestroys_) {
    if (pending.readySerial <= completed) {
      recycleRetiredSlot(pending.kind, pending.slotIndex);
      reportTextureRelease(pending.releasesTextureAllocation);
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
  const bool releasesTextureAllocation = ReleasesTextureAllocation(*resolved.result());
  table.retire(slotIndex);
  retireResource(kind, slotIndex, lastUseSerial, releasesTextureAllocation);
  return OkStatus();
}

template <typename Record>
void Device::releaseFromRaii(details::SlotTable<Record>& table, uint32_t slotIndex,
                             uint32_t generation, ResourceKind kind) {
  const Record* record = table.find(slotIndex, generation);
  if (record == nullptr) {
    return;  // Already destroyed explicitly (or consumed); RAII release is a no-op.
  }
  const uint64_t lastUseSerial = table.lastUseOf(slotIndex);
  const bool releasesTextureAllocation = ReleasesTextureAllocation(*record);
  table.retire(slotIndex);
  retireResource(kind, slotIndex, lastUseSerial, releasesTextureAllocation);
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
DONNER_GPU_DEFINE_RAII_RELEASE(ComputePipelineTag, computePipelines_, ComputePipeline)

#undef DONNER_GPU_DEFINE_RAII_RELEASE

/// A dropped surface hands its frame back, releases whatever texture it had acquired, tells the
/// backend to let go of the platform object, and then releases its own slot. There is no backend
/// object to defer against a submission: a surface's platform object outlives the runtime's
/// handle to it.
template <>
void ReleaseHandleFromRaii<SurfaceTag>(Device& device, uint32_t slotIndex, uint32_t generation) {
  const Device::SurfaceRecord* record = device.surfaces_.find(slotIndex, generation);
  if (record == nullptr) {
    return;  // Already destroyed (consumed); nothing to release.
  }
  // The platform holds the frame it handed out until it is presented or given back, and holds
  // exactly one, so a surface that still names one returns it before its state goes away.
  if (record->acquired.isValid()) {
    device.onAbandonCurrentTexture(slotIndex);
  }
  device.releaseAcquiredSurfaceTextureBySlot(slotIndex, generation);
  device.onDestroySurface(slotIndex);
  device.surfaces_.release(slotIndex);
}

/// A dropped mapping tells the backend to unmap and releases its slot immediately. There is no
/// submission lifetime to defer against: a mapping is host-side access to a buffer, not a
/// resource a recorded command can still name.
template <>
void ReleaseHandleFromRaii<BufferMappingTag>(Device& device, uint32_t slotIndex,
                                             uint32_t generation) {
  if (device.bufferMappings_.find(slotIndex, generation) == nullptr) {
    return;  // Already released (consumed); nothing to unmap.
  }
  device.onUnmapBuffer(slotIndex);
  device.bufferMappings_.release(slotIndex);
}

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
  if (observer_ != nullptr) {
    observer_->onBufferCreated();
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
  // A backend that names a texture it was handed, rather than allocating one, says so through
  // the same ownership answer that keeps destroyTextureBacking from freeing it. The answer is kept
  // with the record, because a backend's own answer changes once it has freed the backing.
  const bool ownsAllocation = onOwnsTextureBacking(handle.slotIndex());
  textures_.findMutable(handle.slotIndex(), handle.generation())->ownsAllocation = ownsAllocation;
  if (observer_ != nullptr && ownsAllocation) {
    observer_->onTextureCreated();
  }
  return handle;
}

Result<TextureDescriptor> Device::textureDescriptor(const Texture& texture) const {
  Result<const TextureRecord*> record = resolve(textures_, texture, TextureTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  return record.result()->descriptor;
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

Status Device::findBindGroupEntryForBinding(const BindGroupDescriptor& descriptor,
                                            const BindGroupLayoutEntry& layoutEntry,
                                            const BindGroupEntry*& match) const {
  match = nullptr;
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
  return OkStatus();
}

Status Device::validateBufferBindingRange(const BindGroupEntry& entry,
                                          const BufferBinding& bufferBinding,
                                          std::string_view bufferLabel,
                                          uint64_t bufferByteSize) const {
  if (bufferBinding.sizeBytes == 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupEntry binding {}: sizeBytes is 0", entry.binding));
  }
  if (bufferBinding.offsetBytes % kBindingOffsetAlignment != 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupEntry binding {}: offsetBytes {} is not a multiple of "
                           "the {}-byte binding offset alignment",
                           entry.binding, bufferBinding.offsetBytes, kBindingOffsetAlignment));
  }
  const std::optional<uint64_t> bindingEnd =
      CheckedAdd(bufferBinding.offsetBytes, bufferBinding.sizeBytes);
  if (!bindingEnd || *bindingEnd > bufferByteSize) {
    return Err(GpuErrorType::OutOfBounds,
               std::format("BindGroupEntry binding {}: range offsetBytes={} sizeBytes={} "
                           "does not fit in buffer \"{}\" of {} bytes",
                           entry.binding, bufferBinding.offsetBytes, bufferBinding.sizeBytes,
                           bufferLabel, bufferByteSize));
  }
  return OkStatus();
}

Status Device::validateBufferBindingEntry(const BindGroupLayoutEntry& layoutEntry,
                                          const BindGroupEntry& entry) const {
  const BufferBinding* bufferBinding = std::get_if<BufferBinding>(&entry.resource);
  if (bufferBinding == nullptr) {
    return Err(
        GpuErrorType::InvalidDescriptor,
        std::format("BindGroupEntry binding {} must bind a buffer to match the "
                    "layout type {}",
                    entry.binding,
                    layoutEntry.type == BindingType::UniformBuffer ? "UniformBuffer"
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
                           entry.binding, bufferRecord.result()->descriptor.label.str(),
                           isUniform ? "Uniform" : "Storage"));
  }
  return validateBufferBindingRange(entry, *bufferBinding,
                                    bufferRecord.result()->descriptor.label.str(),
                                    bufferRecord.result()->descriptor.byteSize);
}

Status Device::validateSampledTextureBindingEntry(const BindGroupLayoutEntry& layoutEntry,
                                                  const BindGroupEntry& entry) const {
  const TextureViewBinding* viewBinding = std::get_if<TextureViewBinding>(&entry.resource);
  if (viewBinding == nullptr) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupEntry binding {} must bind a texture view to match "
                           "the sampled-texture layout type",
                           entry.binding));
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
                           entry.binding, viewRecord.result()->descriptor.label.str()));
  }
  if (viewedTexture.result()->descriptor.format == TextureFormat::RGBA32Float &&
      layoutEntry.type != BindingType::SampledTexture2dUnfilterableFloat) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupEntry binding {}: RGBA32Float requires an unfilterable "
                           "sampled-texture binding",
                           entry.binding));
  }
  return OkStatus();
}

Status Device::validateStorageTextureBindingEntry(const BindGroupLayoutEntry& layoutEntry,
                                                  const BindGroupEntry& entry) const {
  const TextureViewBinding* viewBinding = std::get_if<TextureViewBinding>(&entry.resource);
  if (viewBinding == nullptr) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupEntry binding {} must bind a texture view to match "
                           "the layout type WriteOnlyStorageTexture2d",
                           entry.binding));
  }
  auto viewRecord = resolve(textureViews_, viewBinding->view, TextureViewTag::kName);
  if (viewRecord.hasError()) {
    return std::move(viewRecord).error();
  }
  auto viewedTexture = resolveViewedTexture(*viewRecord.result());
  if (viewedTexture.hasError()) {
    return std::move(viewedTexture).error();
  }
  const TextureDescriptor& textureDescriptor = viewedTexture.result()->descriptor;
  if (!HasAllFlags(textureDescriptor.usage, TextureUsage::StorageBinding)) {
    return Err(GpuErrorType::UsageMismatch,
               std::format("BindGroupEntry binding {}: texture view \"{}\" lacks the "
                           "StorageBinding usage",
                           entry.binding, viewRecord.result()->descriptor.label.str()));
  }
  // The shader's storage-texture declaration names one texel format; a texture of a different
  // format would reinterpret the stores, so the mismatch fails closed here.
  if (textureDescriptor.format != layoutEntry.storageTextureFormat) {
    std::ostringstream formats;
    formats << textureDescriptor.format << " vs " << layoutEntry.storageTextureFormat;
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupEntry binding {}: texture \"{}\" format does not match the "
                           "layout's storageTextureFormat ({})",
                           entry.binding, textureDescriptor.label.str(), formats.str()));
  }
  return OkStatus();
}

Status Device::validateSamplerBindingEntry(const BindGroupEntry& entry) const {
  const SamplerBinding* samplerBinding = std::get_if<SamplerBinding>(&entry.resource);
  if (samplerBinding == nullptr) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("BindGroupEntry binding {} must bind a sampler to match the "
                           "layout type FilteringSampler",
                           entry.binding));
  }
  auto samplerRecord = resolve(samplers_, samplerBinding->sampler, SamplerTag::kName);
  if (samplerRecord.hasError()) {
    return std::move(samplerRecord).error();
  }
  return OkStatus();
}

Status Device::validateBindGroupEntryForLayout(const BindGroupLayoutEntry& layoutEntry,
                                               const BindGroupEntry& entry) const {
  switch (layoutEntry.type) {
    case BindingType::UniformBuffer:
    case BindingType::ReadOnlyStorageBuffer: return validateBufferBindingEntry(layoutEntry, entry);
    case BindingType::SampledTexture2dFloat:
    case BindingType::SampledTexture2dUnfilterableFloat:
      return validateSampledTextureBindingEntry(layoutEntry, entry);
    case BindingType::WriteOnlyStorageTexture2d:
      return validateStorageTextureBindingEntry(layoutEntry, entry);
    case BindingType::FilteringSampler: return validateSamplerBindingEntry(entry);
  }
  return OkStatus();
}

void Device::collectBoundTextures(
    const BindGroupDescriptor& descriptor, const std::vector<BindGroupLayoutEntry>& layoutEntries,
    SmallVector<BoundTextureBinding, kMaxBindings>& sampledOut,
    SmallVector<BoundTextureBinding, kMaxBindings>& storageOut) const {
  for (const BindGroupLayoutEntry& layoutEntry : layoutEntries) {
    const bool sampled = layoutEntry.type == BindingType::SampledTexture2dFloat ||
                         layoutEntry.type == BindingType::SampledTexture2dUnfilterableFloat;
    const bool storage = layoutEntry.type == BindingType::WriteOnlyStorageTexture2d;
    if (!sampled && !storage) {
      continue;
    }
    const BindGroupEntry* entry = nullptr;
    if (findBindGroupEntryForBinding(descriptor, layoutEntry, entry).hasError()) {
      continue;  // Already reported by the per-binding pass.
    }
    const TextureViewBinding* viewBinding = std::get_if<TextureViewBinding>(&entry->resource);
    if (viewBinding == nullptr) {
      continue;  // Already reported by the per-binding pass.
    }
    const TextureViewRecord* view =
        textureViews_.find(viewBinding->view.slotIndex(), viewBinding->view.generation());
    if (view == nullptr) {
      continue;
    }
    (sampled ? sampledOut : storageOut)
        .push_back(BoundTextureBinding{layoutEntry.binding, view->textureIdentity});
  }
}

Status Device::validateNoTextureAliasing(
    const BindGroupDescriptor& descriptor,
    const std::vector<BindGroupLayoutEntry>& layoutEntries) const {
  // Each binding declares the layout its texture must be in, and one image can only be in one
  // layout at a time, so a texture named by both a sampled and a storage-write binding is in the
  // wrong layout for one of them however the backend transitions it. Nothing this runtime serves
  // aliases a texture both ways inside one group, so the shape is rejected rather than modeled.
  SmallVector<BoundTextureBinding, kMaxBindings> sampled;
  SmallVector<BoundTextureBinding, kMaxBindings> storage;
  collectBoundTextures(descriptor, layoutEntries, sampled, storage);

  for (const BoundTextureBinding& sampledBinding : sampled) {
    for (const BoundTextureBinding& storageBinding : storage) {
      if (!(sampledBinding.textureIdentity == storageBinding.textureIdentity)) {
        continue;
      }
      const TextureRecord* texture = textures_.find(sampledBinding.textureIdentity.slotIndex,
                                                    sampledBinding.textureIdentity.generation);
      return Err(GpuErrorType::InvalidDescriptor,
                 std::format("BindGroupDescriptor: texture \"{}\" is bound as a sampled texture "
                             "at binding {} and as a storage texture at binding {}; one texture "
                             "cannot be in both layouts at once",
                             texture != nullptr ? texture->descriptor.label.str() : "",
                             sampledBinding.binding, storageBinding.binding));
    }
  }
  return OkStatus();
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
    if (Status matchStatus = findBindGroupEntryForBinding(descriptor, layoutEntry, match);
        matchStatus.hasError()) {
      return std::move(matchStatus).error();
    }
    if (Status entryStatus = validateBindGroupEntryForLayout(layoutEntry, *match);
        entryStatus.hasError()) {
      return std::move(entryStatus).error();
    }
  }
  if (Status aliasStatus = validateNoTextureAliasing(descriptor, layoutEntries);
      aliasStatus.hasError()) {
    return std::move(aliasStatus).error();
  }

  BindGroupRecord record{
      descriptor, ResourceIdentity{descriptor.layout.slotIndex(), descriptor.layout.generation()}};
  BindGroup handle = allocateHandle<BindGroupTag>(bindGroups_, std::move(record));
  if (Status status = onCreateBindGroup(handle.slotIndex(), descriptor); status.hasError()) {
    bindGroups_.release(handle.slotIndex());
    return std::move(status).error();
  }
  if (observer_ != nullptr) {
    observer_->onBindGroupCreated();
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

  if (Status status = ValidateShaderBufferMetadata(descriptor); status.hasError()) {
    return std::move(status).error();
  }

  ShaderModule handle =
      allocateHandle<ShaderModuleTag>(shaderModules_, ShaderModuleRecord{descriptor});
  if (Status status = onCreateShaderModule(handle.slotIndex(), descriptor); status.hasError()) {
    shaderModules_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Status Device::validatePipelineBufferBinding(const PipelineLayoutRecord& layout,
                                             const ShaderBufferBindingInfo& info) const {
  if (info.group >= layout.bindGroupLayoutIds.size()) {
    return Err(GpuErrorType::InvalidDescriptor,
               "Shader buffer metadata requires a missing pipeline bind group");
  }
  const ResourceIdentity identity = layout.bindGroupLayoutIds[info.group];
  const auto* group = bindGroupLayouts_.find(identity.slotIndex, identity.generation);
  if (group == nullptr) {
    return Err(GpuErrorType::InvalidHandle,
               "Shader buffer metadata references a destroyed bind group layout");
  }
  const auto entry =
      std::ranges::find(group->descriptor.entries, info.binding, &BindGroupLayoutEntry::binding);
  if (entry == group->descriptor.entries.end() || entry->type != info.type ||
      !HasAllFlags(entry->visibility, info.stage)) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("Shader buffer group {} binding {} does not match the pipeline layout's "
                           "type and stage visibility",
                           info.group, info.binding));
  }
  return OkStatus();
}

Status Device::appendPipelineBufferRequirements(
    const PipelineLayoutRecord& layout, const ShaderModuleDescriptor& module,
    std::string_view entryPoint, ShaderStage stage,
    std::vector<PipelineBufferRequirement>& requirements) const {
  if (!module.bufferBindings) {
    return OkStatus();
  }
  for (const ShaderBufferBindingInfo& info : *module.bufferBindings) {
    if (info.entryPoint != entryPoint || info.stage != stage) {
      continue;
    }
    if (Status status = validatePipelineBufferBinding(layout, info); status.hasError()) {
      return status;
    }
    const auto previous = std::ranges::find_if(requirements, [&](const auto& requirement) {
      return requirement.group == info.group && requirement.binding == info.binding;
    });
    if (previous == requirements.end()) {
      requirements.push_back({info.group, info.binding, info.minSizeBytes});
    } else {
      previous->minSizeBytes = std::max(previous->minSizeBytes, info.minSizeBytes);
    }
  }
  return OkStatus();
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
  if (Status status = ValidateRenderPipelineDescriptor(descriptor); status.hasError()) {
    return std::move(status).error();
  }

  RenderPipelineRecord record{descriptor, layoutRecord.result()->bindGroupLayoutIds};
  if (Status status = appendPipelineBufferRequirements(
          *layoutRecord.result(), vertexModule.result()->descriptor, descriptor.vertex.entryPoint,
          ShaderStage::Vertex, record.bufferRequirements);
      status.hasError()) {
    return std::move(status).error();
  }
  if (Status status = appendPipelineBufferRequirements(
          *layoutRecord.result(), fragmentModule.result()->descriptor,
          descriptor.fragment.entryPoint, ShaderStage::Fragment, record.bufferRequirements);
      status.hasError()) {
    return std::move(status).error();
  }
  RenderPipeline handle = allocateHandle<RenderPipelineTag>(renderPipelines_, std::move(record));
  if (Status status = onCreateRenderPipeline(handle.slotIndex(), descriptor); status.hasError()) {
    renderPipelines_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<ComputePipeline> Device::createComputePipeline(const ComputePipelineDescriptor& descriptor) {
  auto layoutRecord = resolve(pipelineLayouts_, descriptor.layout, PipelineLayoutTag::kName);
  if (layoutRecord.hasError()) {
    return std::move(layoutRecord).error();
  }
  auto computeModule = resolve(shaderModules_, descriptor.compute.module, ShaderModuleTag::kName);
  if (computeModule.hasError()) {
    return std::move(computeModule).error();
  }
  if (descriptor.compute.entryPoint.empty()) {
    return Err(GpuErrorType::InvalidDescriptor,
               "ComputePipelineDescriptor.compute.entryPoint is empty");
  }
  if (Status status = ValidateWorkgroupSize(descriptor.workgroupSize); status.hasError()) {
    return std::move(status).error();
  }
  if (Status status =
          ValidateWorkgroupSizeAgainstModule(descriptor, computeModule.result()->descriptor);
      status.hasError()) {
    return std::move(status).error();
  }

  ComputePipelineRecord record{descriptor, layoutRecord.result()->bindGroupLayoutIds};
  if (Status status = appendPipelineBufferRequirements(
          *layoutRecord.result(), computeModule.result()->descriptor, descriptor.compute.entryPoint,
          ShaderStage::Compute, record.bufferRequirements);
      status.hasError()) {
    return std::move(status).error();
  }
  ComputePipeline handle = allocateHandle<ComputePipelineTag>(computePipelines_, std::move(record));
  if (Status status = onCreateComputePipeline(handle.slotIndex(), descriptor); status.hasError()) {
    computePipelines_.release(handle.slotIndex());
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

Status Device::destroyComputePipeline(ComputePipeline&& computePipeline) {
  return destroyResource(computePipelines_, std::move(computePipeline),
                         ResourceKind::ComputePipeline);
}

void Device::onDestroyBufferBacking(uint32_t /*slotIndex*/) {}

void Device::onDestroyTextureBacking(uint32_t /*slotIndex*/) {}

bool Device::onOwnsTextureBacking(uint32_t /*slotIndex*/) const {
  return true;
}

Status Device::destroyBufferBacking(Buffer&& buffer) {
  // Validated before the slot is touched, so a stale or foreign handle cannot free whatever
  // occupies that slot now. The handle still goes through destroyBuffer either way, which is what
  // keeps this operation on the same destroy contract as the rest of the family: the handle is
  // consumed, and one belonging to another device is released on that device rather than left
  // behind here.
  const Status validated = validateBufferHandleForBackend(buffer);
  if (!validated.hasError()) {
    onDestroyBufferBacking(buffer.slotIndex());
  }
  const Status destroyed = destroyBuffer(std::move(buffer));
  return validated.hasError() ? validated : destroyed;
}

Status Device::destroyTextureBacking(Texture&& texture) {
  // Ownership decides whether the allocation is ours to free at all: a registration of host
  // memory, and a frame a surface handed out, are both named by a perfectly valid handle whose
  // memory belongs to someone else. Checking here rather than in each backend hook is what makes
  // the guarantee hold on every backend instead of on the ones that remembered.
  const Status validated = validateTextureHandleForBackend(texture);
  if (!validated.hasError() && ownsTextureBacking(texture)) {
    releaseTextureBackingOrDefer(texture.slotIndex());
    // This device's claim on the allocation ends here, whether the backend frees it now or an
    // export keeps it alive until its last holder lets go. The record forgets the claim, so the
    // slot's later recycle does not report it a second time.
    if (TextureRecord* record = textures_.findMutable(texture.slotIndex(), texture.generation());
        record != nullptr) {
      reportTextureRelease(std::exchange(record->ownsAllocation, false));
    }
  }
  const Status destroyed = destroyTexture(std::move(texture));
  return validated.hasError() ? validated : destroyed;
}

bool Device::namesAcquiredSurfaceFrame(const Texture& texture) const {
  bool acquired = false;
  surfaces_.forEachLive([&](const SurfaceRecord& record) {
    acquired = acquired ||
               (record.acquired.isValid() && record.acquired.slotIndex() == texture.slotIndex() &&
                record.acquired.generation() == texture.generation());
  });
  return acquired;
}

bool Device::ownsTextureBacking(const Texture& texture) const {
  if (validateTextureHandleForBackend(texture).hasError()) {
    return false;
  }
  // The swapchain hands a frame out and takes it back; a caller that freed it would free memory
  // the surface still owns, so no backend gets asked about one.
  if (namesAcquiredSurfaceFrame(texture)) {
    return false;
  }
  if (textureRegistrationOf(texture.slotIndex()) != nullptr) {
    return false;
  }
  return onOwnsTextureBacking(texture.slotIndex());
}

bool Device::onWaitForSerial(uint64_t serial, double timeoutSeconds) {
  // Completions this backend does not signal arrive on their own, so the only thing left to do
  // is look again; the rest is what keeps looking again from becoming a spin.
  constexpr std::chrono::milliseconds kRecheckInterval{1};
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(timeoutSeconds));
  for (;;) {
    if (completedSerial() >= serial) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kRecheckInterval);
  }
}

bool Device::waitForSerial(uint64_t serial, double timeoutSeconds) {
  // Clamped at both ends before any backend converts it: the low end so a negative budget is a
  // question rather than an endless wait, the high end so a caller that means "indefinitely" and
  // writes a very large number does not overflow the clock duration every backend converts into.
  // A NaN budget fails both comparisons and clamps to zero, which is the safe reading of it.
  const double clampedSeconds =
      timeoutSeconds > 0.0 ? std::min(timeoutSeconds, kMaxWaitSeconds) : 0.0;
  return onWaitForSerial(serial, clampedSeconds);
}

namespace {

/// Rejects wait bounds that could never terminate or could never wait.
/// @param params Bounds to check.
Status ValidateMapWaitParams(const MapWaitParams& params) {
  if (!(params.sliceSeconds > 0.0)) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("waitForMapping: sliceSeconds {} must be greater than zero",
                           params.sliceSeconds));
  }
  if (!(params.timeoutSeconds > 0.0)) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("waitForMapping: timeoutSeconds {} must be greater than zero",
                           params.timeoutSeconds));
  }
  return OkStatus();
}

/// The clock a wait measures its budget against: the caller's, or the real one.
/// @param testHooks Hooks the caller supplied; may be empty.
std::function<std::chrono::steady_clock::time_point()> WaitClock(
    const Device::MapWaitTestHooks& testHooks) {
  if (testHooks.now) {
    return testHooks.now;
  }
  return [] { return std::chrono::steady_clock::now(); };
}

/// How a wait spends the remainder of a slice the backend returned from early.
/// @param testHooks Hooks the caller supplied; may be empty.
std::function<void(std::chrono::microseconds)> WaitRest(const Device::MapWaitTestHooks& testHooks) {
  if (testHooks.rest) {
    return testHooks.rest;
  }
  return [](std::chrono::microseconds duration) { std::this_thread::sleep_for(duration); };
}

/// Maps what one backend slice reported onto the outcome a waiter sees, or nullopt to keep
/// waiting.
/// @param state What the backend reported.
std::optional<MapWaitOutcome> OutcomeForSlice(MapSliceState state) {
  switch (state) {
    case MapSliceState::Ready: return MapWaitOutcome::Ready;
    case MapSliceState::DeviceLost: return MapWaitOutcome::DeviceLost;
    case MapSliceState::Failed: return MapWaitOutcome::Failed;
    case MapSliceState::Pending: break;
  }
  return std::nullopt;
}

}  // namespace

Status Device::onSubmitAfterSources(uint64_t submissionSerial,
                                    std::span<const SubmittedCommandBuffer> commandBuffers,
                                    std::span<const SourceWait> waits) {
  if (!waits.empty()) {
    return Err(GpuErrorType::Unsupported,
               "submit: this backend cannot make a submission wait on the device for another "
               "device's work");
  }
  return onSubmit(submissionSerial, commandBuffers);
}

Status Device::onMapBufferAsync(uint32_t /*mappingSlotIndex*/, uint32_t /*bufferSlotIndex*/,
                                MapMode /*mode*/, uint64_t /*offsetBytes*/,
                                uint64_t /*byteCount*/) {
  return Err(GpuErrorType::Unsupported, "this backend does not support host buffer mapping");
}

MapSliceReport Device::onWaitMappingSlice(uint32_t /*mappingSlotIndex*/, double /*sliceSeconds*/) {
  return MapSliceReport{.state = MapSliceState::Failed, .waitKind = MapWaitKind::Polled};
}

Result<std::span<const uint8_t>> Device::onMappedBytes(uint32_t /*mappingSlotIndex*/) const {
  return GpuError{GpuErrorType::Unsupported, "this backend does not support host buffer mapping"};
}

void Device::onUnmapBuffer(uint32_t /*mappingSlotIndex*/) {}

Result<BufferMapping> Device::mapBufferAsync(const Buffer& buffer, MapMode mode,
                                             uint64_t offsetBytes, uint64_t byteCount) {
  auto record = resolve(buffers_, buffer, BufferTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  if (!HasAllFlags(record.result()->descriptor.usage, BufferUsage::MapRead)) {
    return GpuError{GpuErrorType::UsageMismatch,
                    std::format("mapBufferAsync: buffer \"{}\" lacks the MapRead usage",
                                record.result()->descriptor.label.str())};
  }
  if (byteCount == 0) {
    return GpuError{GpuErrorType::InvalidDescriptor, "mapBufferAsync: byteCount must be nonzero"};
  }
  const std::optional<uint64_t> endByte = CheckedAdd(offsetBytes, byteCount);
  if (!endByte || *endByte > record.result()->descriptor.byteSize) {
    return GpuError{
        GpuErrorType::OutOfBounds,
        std::format("mapBufferAsync: range offsetBytes={} byteCount={} does not fit in buffer "
                    "\"{}\" of {} bytes",
                    offsetBytes, byteCount, record.result()->descriptor.label.str(),
                    record.result()->descriptor.byteSize)};
  }

  if (bufferHasOpenMapping(buffer.slotIndex())) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("mapBufferAsync: buffer \"{}\" already has an open mapping",
                                record.result()->descriptor.label.str())};
  }

  BufferMapping handle = allocateHandle<BufferMappingTag>(
      bufferMappings_, MappingRecord{.bufferSlotIndex = buffer.slotIndex(),
                                     .mode = mode,
                                     .offsetBytes = offsetBytes,
                                     .byteCount = byteCount});
  if (Status status =
          onMapBufferAsync(handle.slotIndex(), buffer.slotIndex(), mode, offsetBytes, byteCount);
      status.hasError()) {
    bufferMappings_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

bool Device::bufferHasOpenMapping(uint32_t bufferSlotIndex) const {
  // A mapping whose buffer was destroyed still names that buffer's slot, and the slot is handed
  // to the next buffer created, so a retired mapping must not speak for the slot's new occupant.
  bool open = false;
  bufferMappings_.forEachLive([&](const MappingRecord& existing) {
    open = open || (!existing.bufferRetired && existing.bufferSlotIndex == bufferSlotIndex);
  });
  return open;
}

void Device::noteMappingOutcome(const BufferMapping& mapping, MapWaitOutcome outcome) {
  if (outcome != MapWaitOutcome::Ready) {
    return;
  }
  // Reading is gated on a wait having seen the mapping complete, so that observation is what the
  // runtime records here.
  if (MappingRecord* record =
          bufferMappings_.findMutable(mapping.slotIndex(), mapping.generation())) {
    record->ready = true;
  }
}

Result<MapWaitReport> Device::waitForMapping(const BufferMapping& mapping,
                                             const MapWaitParams& params,
                                             const std::function<bool()>& shouldCancel,
                                             const MapWaitTestHooks& testHooks) {
  if (Status status = ValidateMapWaitParams(params); status.hasError()) {
    return std::move(status).error();
  }
  auto record = resolve(bufferMappings_, mapping, BufferMappingTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  if (record.result()->bufferRetired) {
    return GpuError{GpuErrorType::InvalidHandle,
                    "waitForMapping: the mapped buffer was destroyed; the mapping can never "
                    "complete"};
  }

  // The budget is wall time that actually passed, not slices counted off. A backend whose slice
  // returns as soon as it has polled did not wait the slice it was offered, so counting it as a
  // full one declares the budget spent in a burst of fast calls microseconds after the wait
  // began. Whatever a slice leaves unused is rested here instead, which keeps the budget honest
  // and keeps a fast slice from turning the wait into a spin.
  const std::function<std::chrono::steady_clock::time_point()> now = WaitClock(testHooks);
  const std::function<void(std::chrono::microseconds)> rest = WaitRest(testHooks);

  const std::chrono::steady_clock::time_point start = now();
  // Sticky across the wait's own slices: one event wait is what says this path was not reduced to
  // polling, so a polled slice after it does not take that back.
  MapWaitKind waitKind = MapWaitKind::Polled;
  while (true) {
    if (shouldCancel && shouldCancel()) {
      return MapWaitReport{.outcome = MapWaitOutcome::Cancelled, .waitKind = waitKind};
    }
    const std::chrono::steady_clock::time_point sliceStart = now();
    const MapSliceReport slice = onWaitMappingSlice(mapping.slotIndex(), params.sliceSeconds);
    if (slice.waitKind == MapWaitKind::CompletionEvent) {
      waitKind = MapWaitKind::CompletionEvent;
    }
    const std::optional<MapWaitOutcome> outcome = OutcomeForSlice(slice.state);
    if (outcome.has_value()) {
      noteMappingOutcome(mapping, *outcome);
      return MapWaitReport{.outcome = *outcome, .waitKind = waitKind};
    }

    const double sliceUsedSeconds = std::chrono::duration<double>(now() - sliceStart).count();
    if (sliceUsedSeconds < params.sliceSeconds) {
      rest(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::duration<double>(params.sliceSeconds - sliceUsedSeconds)));
    }
    if (std::chrono::duration<double>(now() - start).count() >= params.timeoutSeconds) {
      return MapWaitReport{.outcome = MapWaitOutcome::TimedOut, .waitKind = waitKind};
    }
  }
}

Result<std::span<const uint8_t>> Device::mappedBytes(const BufferMapping& mapping) const {
  auto record = resolve(bufferMappings_, mapping, BufferMappingTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  if (record.result()->bufferRetired) {
    return GpuError{GpuErrorType::InvalidHandle,
                    "mappedBytes: the mapped buffer was destroyed while the mapping was open"};
  }
  if (!record.result()->ready) {
    return GpuError{GpuErrorType::InvalidState,
                    "mappedBytes: the mapping has not completed; wait for it with waitForMapping "
                    "before reading its bytes"};
  }
  return onMappedBytes(mapping.slotIndex());
}

Status Device::unmapBuffer(BufferMapping&& mapping) {
  const BufferMapping consumed = std::move(mapping);
  auto record = resolve(bufferMappings_, consumed, BufferMappingTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  // Letting the consumed handle go out of scope here releases the mapping through exactly the
  // path a dropped handle takes, so explicit release and RAII cannot disagree.
  return OkStatus();
}

namespace {

/// Which payload slots of a \ref NativeSurfaceHandle one kind uses.
struct NativeSurfacePayload {
  bool usesDisplay = false;   //!< The kind names a platform object pointer.
  bool usesWindow = false;    //!< The kind names a window or surface by integer handle.
  bool usesSelector = false;  //!< The kind names its target by string.
};

/// Returns the payload slots \p kind uses. @param kind Surface kind.
NativeSurfacePayload PayloadForKind(NativeSurfaceKind kind) {
  switch (kind) {
    case NativeSurfaceKind::MetalLayer: return NativeSurfacePayload{true, false, false};
    case NativeSurfaceKind::XlibWindow:
    case NativeSurfaceKind::WaylandSurface: return NativeSurfacePayload{true, true, false};
    case NativeSurfaceKind::CanvasSelector: return NativeSurfacePayload{false, false, true};
    case NativeSurfaceKind::EmbedderSurface: return NativeSurfacePayload{false, true, false};
    case NativeSurfaceKind::Headless: return NativeSurfacePayload{false, false, false};
  }
  return NativeSurfacePayload{};
}

/// Reports one payload slot that disagrees with its kind, or nullopt when the slot is consistent.
///
/// A slot the kind needs but that is empty, and a slot the kind does not use but that is filled,
/// are both disagreements between the caller and the handle about what it names.
///
/// @param used Whether the kind uses this slot.
/// @param populated Whether the caller filled it.
/// @param slotName Slot name, for the message.
std::optional<std::string> NativeSurfaceSlotProblem(bool used, bool populated,
                                                    std::string_view slotName) {
  if (used && !populated) {
    return std::format("this surface kind needs {}", slotName);
  }
  if (!used && populated) {
    return std::format("this surface kind does not use {}", slotName);
  }
  return std::nullopt;
}

/// Rejects a native handle whose populated payload slots do not match its kind.
/// @param native Handle to check.
Status ValidateNativeSurfaceHandle(const NativeSurfaceHandle& native) {
  if (Status status = CheckEnum(native.kind, "NativeSurfaceHandle.kind"); status.hasError()) {
    return status;
  }
  const NativeSurfacePayload payload = PayloadForKind(native.kind);
  const std::array<std::optional<std::string>, 3> problems = {
      NativeSurfaceSlotProblem(payload.usesDisplay, native.display != nullptr, "a platform object"),
      NativeSurfaceSlotProblem(payload.usesWindow, native.window != 0, "a window handle"),
      NativeSurfaceSlotProblem(payload.usesSelector, !native.selector.empty(), "a selector")};
  for (const std::optional<std::string>& problem : problems) {
    if (problem.has_value()) {
      return Err(GpuErrorType::InvalidDescriptor, std::format("createSurface: {}", *problem));
    }
  }
  return OkStatus();
}

/// Rejects a configuration a surface could not present under.
/// @param configuration Configuration to check.
Status ValidateSurfaceConfiguration(const SurfaceConfiguration& configuration) {
  if (Status status = CheckEnum(configuration.format, "SurfaceConfiguration.format");
      status.hasError()) {
    return status;
  }
  if (Status status = ValidateRenderTargetFormat(configuration.format); status.hasError()) {
    return status;
  }
  if (Status status = CheckBitmask(configuration.usage, "SurfaceConfiguration.usage");
      status.hasError()) {
    return status;
  }
  if (configuration.usage == TextureUsage::None) {
    return Err(GpuErrorType::InvalidDescriptor, "SurfaceConfiguration.usage is empty");
  }
  // Pacing and alpha compositing are checked here rather than left to the backend: the mappings
  // onto backend values fall back to Fifo and Opaque for anything they do not recognise, so an
  // unknown value would otherwise be presented under a configuration nobody asked for.
  if (Status status = CheckEnum(configuration.presentMode, "SurfaceConfiguration.presentMode");
      status.hasError()) {
    return status;
  }
  if (Status status = CheckEnum(configuration.alphaMode, "SurfaceConfiguration.alphaMode");
      status.hasError()) {
    return status;
  }
  if (configuration.size.width == 0 || configuration.size.height == 0) {
    return Err(GpuErrorType::InvalidDescriptor,
               std::format("SurfaceConfiguration.size {}x{} has a zero dimension",
                           configuration.size.width, configuration.size.height));
  }
  // Bounded by the same limit as any other texture, because the frames a surface hands out are
  // textures the rest of the runtime validates against this configuration: a platform that
  // clamped an oversized request to what it can allocate would leave every later range check
  // measuring a frame against an extent nothing ever allocated.
  if (configuration.size.width > kMaxTextureDimension ||
      configuration.size.height > kMaxTextureDimension) {
    return Err(
        GpuErrorType::LimitExceeded,
        std::format("SurfaceConfiguration.size {}x{} exceeds kMaxTextureDimension {}",
                    configuration.size.width, configuration.size.height, kMaxTextureDimension));
  }
  return OkStatus();
}

}  // namespace

Status Device::onCreateSurface(uint32_t /*slotIndex*/, const SurfaceDescriptor& /*descriptor*/) {
  return Err(GpuErrorType::Unsupported, "this backend does not support presentation");
}

Result<SurfaceCapabilities> Device::onSurfaceCapabilities(uint32_t /*slotIndex*/) const {
  return GpuError{GpuErrorType::Unsupported, "this backend does not support presentation"};
}

Status Device::onConfigureSurface(uint32_t /*slotIndex*/,
                                  const SurfaceConfiguration& /*configuration*/) {
  return Err(GpuErrorType::Unsupported, "this backend does not support presentation");
}

Result<SurfaceStatus> Device::onAcquireCurrentTexture(uint32_t /*slotIndex*/,
                                                      uint32_t /*textureSlotIndex*/) {
  return GpuError{GpuErrorType::Unsupported, "this backend does not support presentation"};
}

Result<SurfaceStatus> Device::onPresentSurface(uint32_t /*slotIndex*/) {
  return GpuError{GpuErrorType::Unsupported, "this backend does not support presentation"};
}

void Device::onAbandonCurrentTexture(uint32_t /*slotIndex*/) {}

void Device::onDestroySurface(uint32_t /*slotIndex*/) {}

uint64_t Device::lastTextureUseSerial(uint32_t textureSlotIndex) const {
  return textures_.lastUseOf(textureSlotIndex);
}

Result<Surface> Device::createSurface(const SurfaceDescriptor& descriptor) {
  if (Status status = ValidateNativeSurfaceHandle(descriptor.native); status.hasError()) {
    return std::move(status).error();
  }

  Surface handle =
      allocateHandle<SurfaceTag>(surfaces_, SurfaceRecord{descriptor, std::nullopt, Texture()});
  if (Status status = onCreateSurface(handle.slotIndex(), descriptor); status.hasError()) {
    surfaces_.release(handle.slotIndex());
    return std::move(status).error();
  }
  return handle;
}

Result<SurfaceCapabilities> Device::surfaceCapabilities(const Surface& surface) const {
  auto record = resolve(surfaces_, surface, SurfaceTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  return onSurfaceCapabilities(surface.slotIndex());
}

Status Device::configureSurface(const Surface& surface, const SurfaceConfiguration& configuration) {
  if (Status status = ValidateSurfaceConfiguration(configuration); status.hasError()) {
    return status;
  }
  auto record = resolve(surfaces_, surface, SurfaceTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  // A texture acquired under the previous configuration describes a surface that no longer
  // exists in that shape, so reconfiguring invalidates it rather than leaving it usable. The
  // platform is holding that frame as well, and holds exactly one, so it is handed back rather
  // than merely forgotten - otherwise the next acquire is refused by the backend. What decides
  // that is the reference itself, not whether its texture is still live: the caller disposing of
  // the handle does not take the frame back off the platform.
  if (record.result()->acquired.isValid()) {
    onAbandonCurrentTexture(surface.slotIndex());
  }
  releaseAcquiredSurfaceTexture(surface);
  if (Status status = onConfigureSurface(surface.slotIndex(), configuration); status.hasError()) {
    return status;
  }
  if (SurfaceRecord* mutableRecord = mutableSurfaceRecord(surface); mutableRecord != nullptr) {
    mutableRecord->configured = configuration;
  }
  return OkStatus();
}

Result<SurfaceTexture> Device::acquireCurrentTexture(const Surface& surface) {
  auto record = resolve(surfaces_, surface, SurfaceTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  if (!record.result()->configured.has_value()) {
    return GpuError{GpuErrorType::InvalidState,
                    "acquireCurrentTexture: the surface has not been configured"};
  }
  if (record.result()->acquired.isValid() && !hasOutstandingFrame(*record.result())) {
    // The caller disposed of the frame's texture through its handle, so the runtime has nothing
    // left to resolve it with - but the platform is still holding the frame itself, and holds
    // exactly one. Hand it back before asking for the next, or the backend refuses.
    onAbandonCurrentTexture(surface.slotIndex());
    releaseAcquiredSurfaceTexture(surface);
  }
  if (record.result()->acquired.isValid()) {
    return GpuError{GpuErrorType::InvalidState,
                    "acquireCurrentTexture: the previous texture has not been presented or "
                    "abandoned"};
  }

  const SurfaceConfiguration& configuration = *record.result()->configured;
  Texture texture = allocateHandle<TextureTag>(
      textures_,
      TextureRecord{TextureDescriptor{record.result()->descriptor.label, configuration.size,
                                      configuration.format, configuration.usage, 1}});
  Result<SurfaceStatus> status = onAcquireCurrentTexture(surface.slotIndex(), texture.slotIndex());
  if (status.hasError()) {
    textures_.release(texture.slotIndex());
    return std::move(status).error();
  }
  if (status.result() == SurfaceStatus::Lost || status.result() == SurfaceStatus::DeviceLost ||
      status.result() == SurfaceStatus::Timeout) {
    // No frame came back, so there is nothing to keep alive and nothing for the caller to draw
    // into; the status alone says what to do next.
    textures_.release(texture.slotIndex());
    return SurfaceTexture{Texture(), status.result()};
  }

  SurfaceTexture acquired{std::move(texture), status.result()};
  if (SurfaceRecord* mutableRecord = mutableSurfaceRecord(surface); mutableRecord != nullptr) {
    mutableRecord->acquired = TextureRef(acquired.texture);
  }
  return acquired;
}

Result<SurfaceStatus> Device::presentSurface(const Surface& surface) {
  auto record = resolve(surfaces_, surface, SurfaceTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  if (!record.result()->acquired.isValid()) {
    return GpuError{GpuErrorType::InvalidState, "presentSurface: no texture has been acquired"};
  }

  Result<SurfaceStatus> status = onPresentSurface(surface.slotIndex());
  // The platform owns the texture once it has been handed over, whether or not presenting
  // reported success, so it stops being usable either way.
  releaseAcquiredSurfaceTexture(surface);
  return status;
}

Status Device::abandonCurrentTexture(const Surface& surface) {
  auto record = resolve(surfaces_, surface, SurfaceTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  if (!record.result()->acquired.isValid()) {
    return Err(GpuErrorType::InvalidState, "abandonCurrentTexture: no texture has been acquired");
  }
  onAbandonCurrentTexture(surface.slotIndex());
  releaseAcquiredSurfaceTexture(surface);
  return OkStatus();
}

Status Device::destroySurface(Surface&& surface) {
  const Surface consumed = std::move(surface);
  auto record = resolve(surfaces_, consumed, SurfaceTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  // Destroying is the same teardown a dropped handle takes, so it is left to the handle this
  // consumed the caller's into: one sequence hands the frame back, releases the texture, tells
  // the backend to let the platform object go, and retires the slot.
  return OkStatus();
}

void Device::releaseAcquiredSurfaceTexture(const Surface& surface) {
  releaseAcquiredSurfaceTextureBySlot(surface.slotIndex(), surface.generation());
}

bool Device::hasOutstandingFrame(const SurfaceRecord& record) const {
  return record.acquired.isValid() &&
         textures_.find(record.acquired.slotIndex(), record.acquired.generation()) != nullptr;
}

void Device::releaseAcquiredSurfaceTextureBySlot(uint32_t slotIndex, uint32_t generation) {
  SurfaceRecord* record = surfaces_.findMutable(slotIndex, generation);
  if (record == nullptr || !record->acquired.isValid()) {
    return;
  }
  if (textures_.find(record->acquired.slotIndex(), record->acquired.generation()) != nullptr) {
    // The surface takes the frame back here, bypassing the serial-deferred retirement other
    // textures take, so an export of it is released here too: the slot's next frame must never
    // be handed the old frame's share.
    releaseTextureShare(record->acquired.slotIndex());
    textures_.release(record->acquired.slotIndex());
  }
  record->acquired = TextureRef();
}

Device::SurfaceRecord* Device::mutableSurfaceRecord(const Surface& surface) {
  return surfaces_.findMutable(surface.slotIndex(), surface.generation());
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
  if (bufferHasOpenMapping(buffer.slotIndex())) {
    return Err(GpuErrorType::InvalidState,
               std::format("writeBuffer: buffer \"{}\" has an open mapping; the write would change "
                           "bytes the host is holding a view of",
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

  if (Status status = onWriteBuffer(buffer.slotIndex(), offsetBytes, data); status.hasError()) {
    return status;
  }
  if (observer_ != nullptr) {
    observer_->onBufferWritten(data.size());
  }
  return OkStatus();
}

Status Device::writeTexture(const Texture& texture, std::span<const uint8_t> data,
                            const TexelCopyBufferLayout& dataLayout, const Extent2d& writeSize,
                            const Origin2d& destinationOrigin) {
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
  // The origin is caller-supplied, so the far edge is computed in 64 bits: a near-UINT32_MAX
  // origin plus a large extent wraps in 32-bit arithmetic and would compare as in-bounds.
  const std::optional<uint64_t> right =
      CheckedAdd(uint64_t{destinationOrigin.x}, uint64_t{writeSize.width});
  const std::optional<uint64_t> bottom =
      CheckedAdd(uint64_t{destinationOrigin.y}, uint64_t{writeSize.height});
  if (!right || !bottom || *right > textureDescriptor.size.width ||
      *bottom > textureDescriptor.size.height) {
    return Err(GpuErrorType::OutOfBounds,
               std::format("writeTexture: write rectangle {}x{} at ({}, {}) does not fit texture "
                           "\"{}\" size {}x{}",
                           writeSize.width, writeSize.height, destinationOrigin.x,
                           destinationOrigin.y, textureDescriptor.label.str(),
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

  if (Status status =
          onWriteTexture(texture.slotIndex(), data, dataLayout, writeSize, destinationOrigin);
      status.hasError()) {
    return status;
  }
  noteTextureWriteForShares(texture.slotIndex());
  if (observer_ != nullptr) {
    observer_->onTextureWritten(onTextureWriteByteCount(data));
  }
  return OkStatus();
}

Status Device::installObserver(DeviceObserver& observer) {
  if (observer_ != nullptr && observer_ != &observer) {
    return GpuError{GpuErrorType::InvalidState,
                    "installObserver: this device already reports to another observer; its owner "
                    "removes it before a different one can be installed"};
  }
  observer_ = &observer;
  return OkStatus();
}

void Device::removeObserver(const DeviceObserver& observer) {
  if (observer_ == &observer) {
    observer_ = nullptr;
  }
}

uint64_t Device::onTextureWriteByteCount(std::span<const uint8_t> data) const {
  return data.size();
}

void Device::notifyObserverOfBackendSubmission() const {
  if (observer_ != nullptr) {
    observer_->onSubmitted(0, 0);
  }
}

Status Device::consumeSubmissionCommandBuffers(std::span<CommandBuffer> commandBuffers,
                                               std::vector<ConsumedCommandBuffer>& consumed) {
  Status firstRefusal = OkStatus();
  consumed.reserve(commandBuffers.size());
  for (CommandBuffer& element : commandBuffers) {
    // Moving out of the caller's element is what consumes it: the handle is left null whether
    // its slot is taken here or its own destructor releases a buffer this submission refused.
    CommandBuffer taken = std::move(element);
    auto record = resolve(commandBuffers_, taken, CommandBufferTag::kName);
    if (record.hasError()) {
      if (!firstRefusal.hasError()) {
        firstRefusal = std::move(record).error();
      }
      continue;
    }

    // Take ownership of the commands and release the slot: submission is one-shot, so a buffer
    // named twice in one span is stale by the time the second mention resolves.
    const uint32_t slotIndex = taken.slotIndex();
    consumed.push_back(ConsumedCommandBuffer{
        slotIndex,
        std::move(commandBuffers_.findMutable(slotIndex, taken.generation())->commands)});
    commandBuffers_.release(slotIndex);
  }
  return firstRefusal;
}

Status Device::checkSubmissionMappings(std::span<const SubmissionUse> uses) const {
  for (const SubmissionUse& use : uses) {
    if (use.kind == ResourceKind::Buffer && bufferHasOpenMapping(use.slotIndex)) {
      return GpuError{
          GpuErrorType::InvalidState,
          std::format("submit: buffer (slot {}) has an open mapping; release it and record the "
                      "work again before submitting work that uses the buffer",
                      use.slotIndex)};
    }
  }
  return OkStatus();
}

Result<uint64_t> Device::submit(CommandBuffer commandBuffer) {
  return submit(std::span<CommandBuffer>(&commandBuffer, 1));
}

Result<uint64_t> Device::submit(std::span<CommandBuffer> commandBuffers) {
  poll();

  if (commandBuffers.empty()) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    "submit: a submission needs at least one command buffer"};
  }
  if (commandBuffers.size() > kMaxCommandBuffersPerSubmission) {
    // Refused before anything is consumed, so the caller still owns every buffer and can submit
    // them as several smaller spans.
    return GpuError{
        GpuErrorType::InvalidDescriptor,
        std::format("submit: a submission carries at most {} command buffers, not {}; split it "
                    "across submissions",
                    kMaxCommandBuffersPerSubmission, commandBuffers.size())};
  }

  std::vector<ConsumedCommandBuffer> consumed;
  if (Status refusal = consumeSubmissionCommandBuffers(commandBuffers, consumed);
      refusal.hasError()) {
    return std::move(refusal).error();
  }

  // Re-validate every recorded resource identity across the whole span: a resource destroyed
  // between recording and submission fails closed here, before the backend sees the commands.
  std::vector<SubmissionUse> uses;
  std::vector<SubmittedCommandBuffer> submitted;
  submitted.reserve(consumed.size());
  for (const ConsumedCommandBuffer& buffer : consumed) {
    Result<std::vector<SubmissionUse>> bufferUses = validateSubmissionResources(buffer.commands);
    if (bufferUses.hasError()) {
      return std::move(bufferUses).error();
    }
    uses.insert(uses.end(), bufferUses.result().begin(), bufferUses.result().end());
    submitted.push_back(SubmittedCommandBuffer{buffer.slotIndex, buffer.commands});
  }

  if (Status mappings = checkSubmissionMappings(uses); mappings.hasError()) {
    return std::move(mappings).error();
  }
  std::vector<SourceWait> sourceWaits;
  if (Status sources = checkSubmissionTextureSources(uses, sourceWaits); sources.hasError()) {
    return std::move(sources).error();
  }

  // Advance the serial only after the backend accepts the submission: a failed submit must not
  // burn a serial, or completion waiters would treat the failed work as finished. Resources are
  // marked in-use only for accepted submissions for the same reason.
  const uint64_t serial = lastSubmittedSerial_ + 1;
  if (Status status = onSubmitAfterSources(serial, submitted, sourceWaits); status.hasError()) {
    return std::move(status).error();
  }
  lastSubmittedSerial_ = serial;
  markSubmissionUses(uses, serial);
  noteSubmittedTextureShares(uses, serial);
  if (observer_ != nullptr) {
    observer_->onSubmitted(submitted.size(), CountDraws(submitted));
  }
  return serial;
}

template <typename Record>
Result<const Record*> Device::checkSubmissionResource(const details::SlotTable<Record>& table,
                                                      const ResourceIdentity& identity,
                                                      ResourceKind kind,
                                                      std::string_view resourceName,
                                                      std::string_view context,
                                                      std::vector<SubmissionUse>& uses) const {
  const Record* record = table.find(identity.slotIndex, identity.generation);
  if (record == nullptr) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("submit: {} references destroyed {} (slot {})", context,
                                resourceName, identity.slotIndex)};
  }
  uses.push_back(SubmissionUse{kind, identity.slotIndex});
  return record;
}

Status Device::checkSubmissionTextureView(const ResourceIdentity& viewIdentity,
                                          std::string_view context,
                                          std::vector<SubmissionUse>& uses) const {
  auto viewRecord = checkSubmissionResource(textureViews_, viewIdentity, ResourceKind::TextureView,
                                            TextureViewTag::kName, context, uses);
  if (viewRecord.hasError()) {
    return std::move(viewRecord).error();
  }
  auto textureRecord =
      checkSubmissionResource(textures_, viewRecord.result()->textureIdentity,
                              ResourceKind::Texture, TextureTag::kName, context, uses);
  if (textureRecord.hasError()) {
    return std::move(textureRecord).error();
  }
  return OkStatus();
}

Status Device::checkSubmissionRenderPass(const RenderPassDescriptor& descriptor,
                                         std::vector<SubmissionUse>& uses) const {
  for (const RenderPassColorAttachment& attachment : descriptor.colorAttachments) {
    const ResourceIdentity viewIdentity{attachment.view.slotIndex(), attachment.view.generation()};
    if (Status attachmentStatus =
            checkSubmissionTextureView(viewIdentity, "render pass attachment", uses);
        attachmentStatus.hasError()) {
      return attachmentStatus;
    }
  }
  return OkStatus();
}

Status Device::checkSubmissionBindGroup(const ResourceIdentity& groupIdentity,
                                        std::vector<SubmissionUse>& uses) const {
  auto groupRecord = checkSubmissionResource(bindGroups_, groupIdentity, ResourceKind::BindGroup,
                                             BindGroupTag::kName, "recorded setBindGroup", uses);
  if (groupRecord.hasError()) {
    return std::move(groupRecord).error();
  }
  auto layoutRecord = checkSubmissionResource(
      bindGroupLayouts_, groupRecord.result()->layoutIdentity, ResourceKind::BindGroupLayout,
      BindGroupLayoutTag::kName,
      std::format("bind group \"{}\"", groupRecord.result()->descriptor.label.str()), uses);
  if (layoutRecord.hasError()) {
    return std::move(layoutRecord).error();
  }
  for (const BindGroupEntry& entry : groupRecord.result()->descriptor.entries) {
    const std::string context =
        std::format("bind group \"{}\" entry binding {}",
                    groupRecord.result()->descriptor.label.str(), entry.binding);
    if (const BufferBinding* bufferBinding = std::get_if<BufferBinding>(&entry.resource)) {
      const ResourceIdentity bufferIdentity{bufferBinding->buffer.slotIndex(),
                                            bufferBinding->buffer.generation()};
      auto bufferRecord = checkSubmissionResource(buffers_, bufferIdentity, ResourceKind::Buffer,
                                                  BufferTag::kName, context, uses);
      if (bufferRecord.hasError()) {
        return std::move(bufferRecord).error();
      }
    } else if (const TextureViewBinding* viewBinding =
                   std::get_if<TextureViewBinding>(&entry.resource)) {
      const ResourceIdentity viewIdentity{viewBinding->view.slotIndex(),
                                          viewBinding->view.generation()};
      if (Status entryStatus = checkSubmissionTextureView(viewIdentity, context, uses);
          entryStatus.hasError()) {
        return entryStatus;
      }
    } else if (const SamplerBinding* samplerBinding =
                   std::get_if<SamplerBinding>(&entry.resource)) {
      const ResourceIdentity samplerIdentity{samplerBinding->sampler.slotIndex(),
                                             samplerBinding->sampler.generation()};
      auto samplerRecord = checkSubmissionResource(
          samplers_, samplerIdentity, ResourceKind::Sampler, SamplerTag::kName, context, uses);
      if (samplerRecord.hasError()) {
        return std::move(samplerRecord).error();
      }
    }
  }
  return OkStatus();
}

Status Device::checkSubmissionCopyToBuffer(const CopyTextureToBufferCommand& command,
                                           std::vector<SubmissionUse>& uses) const {
  auto textureRecord =
      checkSubmissionResource(textures_, command.textureId, ResourceKind::Texture,
                              TextureTag::kName, "recorded copyTextureToBuffer", uses);
  if (textureRecord.hasError()) {
    return std::move(textureRecord).error();
  }
  auto bufferRecord =
      checkSubmissionResource(buffers_, command.bufferId, ResourceKind::Buffer, BufferTag::kName,
                              "recorded copyTextureToBuffer", uses);
  if (bufferRecord.hasError()) {
    return std::move(bufferRecord).error();
  }
  return OkStatus();
}

Status Device::checkSubmissionCopyToTexture(const CopyTextureToTextureCommand& command,
                                            std::vector<SubmissionUse>& uses) const {
  auto sourceRecord =
      checkSubmissionResource(textures_, command.textureSrcId, ResourceKind::Texture,
                              TextureTag::kName, "recorded copyTextureToTexture", uses);
  if (sourceRecord.hasError()) {
    return std::move(sourceRecord).error();
  }
  auto destinationRecord =
      checkSubmissionResource(textures_, command.textureDstId, ResourceKind::Texture,
                              TextureTag::kName, "recorded copyTextureToTexture", uses);
  if (destinationRecord.hasError()) {
    return std::move(destinationRecord).error();
  }
  return OkStatus();
}

Status Device::checkSubmissionCommand(const Command& command,
                                      std::vector<SubmissionUse>& uses) const {
  return std::visit(
      [&](const auto& typedCommand) -> Status {
        using CommandType = std::remove_cvref_t<decltype(typedCommand)>;

        if constexpr (std::is_same_v<CommandType, BeginRenderPassCommand>) {
          return checkSubmissionRenderPass(typedCommand.descriptor, uses);
        } else if constexpr (std::is_same_v<CommandType, SetPipelineCommand>) {
          auto pipelineRecord = checkSubmissionResource(
              renderPipelines_, typedCommand.pipelineId, ResourceKind::RenderPipeline,
              RenderPipelineTag::kName, "recorded setPipeline", uses);
          if (pipelineRecord.hasError()) {
            return std::move(pipelineRecord).error();
          }
          return OkStatus();
        } else if constexpr (std::is_same_v<CommandType, SetComputePipelineCommand>) {
          auto pipelineRecord = checkSubmissionResource(
              computePipelines_, typedCommand.pipelineId, ResourceKind::ComputePipeline,
              ComputePipelineTag::kName, "recorded compute setPipeline", uses);
          if (pipelineRecord.hasError()) {
            return std::move(pipelineRecord).error();
          }
          return OkStatus();
        } else if constexpr (std::is_same_v<CommandType, SetBindGroupCommand>) {
          return checkSubmissionBindGroup(typedCommand.bindGroupId, uses);
        } else if constexpr (std::is_same_v<CommandType, SetVertexBufferCommand>) {
          auto bufferRecord =
              checkSubmissionResource(buffers_, typedCommand.bufferId, ResourceKind::Buffer,
                                      BufferTag::kName, "recorded setVertexBuffer", uses);
          if (bufferRecord.hasError()) {
            return std::move(bufferRecord).error();
          }
          return OkStatus();
        } else if constexpr (std::is_same_v<CommandType, SetIndexBufferCommand>) {
          auto bufferRecord =
              checkSubmissionResource(buffers_, typedCommand.bufferId, ResourceKind::Buffer,
                                      BufferTag::kName, "recorded setIndexBuffer", uses);
          if (bufferRecord.hasError()) {
            return std::move(bufferRecord).error();
          }
          return OkStatus();
        } else if constexpr (std::is_same_v<CommandType, CopyTextureToBufferCommand>) {
          return checkSubmissionCopyToBuffer(typedCommand, uses);
        } else if constexpr (std::is_same_v<CommandType, CopyTextureToTextureCommand>) {
          return checkSubmissionCopyToTexture(typedCommand, uses);
        } else {
          return OkStatus();
        }
      },
      command);
}

Result<std::vector<Device::SubmissionUse>> Device::validateSubmissionResources(
    std::span<const Command> commands) const {
  std::vector<SubmissionUse> uses;

  for (const Command& command : commands) {
    Status status = checkSubmissionCommand(command, uses);
    if (status.hasError()) {
      return std::move(status).error();
    }
  }
  return uses;
}

void Device::markResourceUsed(ResourceKind kind, uint32_t slotIndex, uint64_t submissionSerial) {
  switch (kind) {
    case ResourceKind::Buffer: buffers_.markUsed(slotIndex, submissionSerial); return;
    case ResourceKind::Texture: textures_.markUsed(slotIndex, submissionSerial); return;
    case ResourceKind::TextureView: textureViews_.markUsed(slotIndex, submissionSerial); return;
    case ResourceKind::Sampler: samplers_.markUsed(slotIndex, submissionSerial); return;
    case ResourceKind::BindGroupLayout:
      bindGroupLayouts_.markUsed(slotIndex, submissionSerial);
      return;
    case ResourceKind::BindGroup: bindGroups_.markUsed(slotIndex, submissionSerial); return;
    case ResourceKind::PipelineLayout:
      pipelineLayouts_.markUsed(slotIndex, submissionSerial);
      return;
    case ResourceKind::ShaderModule: shaderModules_.markUsed(slotIndex, submissionSerial); return;
    case ResourceKind::RenderPipeline:
      renderPipelines_.markUsed(slotIndex, submissionSerial);
      return;
    case ResourceKind::ComputePipeline:
      computePipelines_.markUsed(slotIndex, submissionSerial);
      return;
  }
}

void Device::markSubmissionUses(std::span<const SubmissionUse> uses, uint64_t submissionSerial) {
  for (const SubmissionUse& use : uses) {
    markResourceUsed(use.kind, use.slotIndex, submissionSerial);
  }
}

uint64_t Device::bufferLastUseSerial(uint32_t slotIndex) const {
  return buffers_.lastUseOf(slotIndex);
}

uint64_t Device::textureLastUseSerial(uint32_t slotIndex) const {
  return textures_.lastUseOf(slotIndex);
}

Status Device::validateBufferHandleForBackend(const Buffer& buffer) const {
  auto record = resolve(buffers_, buffer, BufferTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  return OkStatus();
}

Status Device::validateBufferMappingHandleForBackend(const BufferMapping& mapping) const {
  auto record = resolve(bufferMappings_, mapping, BufferMappingTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  return OkStatus();
}

Status Device::validateTextureHandleForBackend(const Texture& texture) const {
  auto record = resolve(textures_, texture, TextureTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  return OkStatus();
}

Status Device::validateTextureViewHandleForBackend(const TextureView& textureView) const {
  auto record = resolve(textureViews_, textureView, TextureViewTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  auto viewedTexture = resolveViewedTexture(*record.result());
  if (viewedTexture.hasError()) {
    return std::move(viewedTexture).error();
  }
  return OkStatus();
}

CommandBuffer Device::registerCommandBuffer(std::vector<Command>&& commands) {
  return allocateHandle<CommandBufferTag>(commandBuffers_,
                                          CommandBufferRecord{std::move(commands)});
}

// Cross-device texture registration.
//
// The producer half runs on the producer's thread and the consumer half on the consumer's; the
// only state they share is the TextureShare the export created, which synchronizes itself.

namespace {

/// Declares the producer of a registered texture lost because its backend reported an execution
/// failure. No deadline expired, so the loss carries no wait site; only the producer's condition is
/// written, because the consumer's own waits decide its condition.
/// @param share Share of the registered texture.
void DeclareProducerFailure(const details::TextureShare& share) {
  if (DeclareDeviceLost(share.producerLostState())) {
    LogDeclaredDeviceLoss(
        "the device producing a registered texture reported an execution failure");
  }
}

/// Usage a registration keeps: a consumer reads what the producer wrote and never writes it.
constexpr TextureUsage kRegistrationUsage = TextureUsage::Sampled | TextureUsage::CopySrc;

/// Why a backend without cross-device naming refuses a registration.
constexpr std::string_view kRegistrationUnsupported =
    "registerTexture: this backend cannot name a texture of another runtime device";

/// Grows \p slots to cover \p slotIndex and returns that element.
/// @param slots Slot-indexed side table. @param slotIndex Slot to reach.
template <typename T>
T& SideTableSlot(std::vector<T>& slots, uint32_t slotIndex) {
  if (slotIndex >= slots.size()) {
    slots.resize(static_cast<size_t>(slotIndex) + 1);
  }
  return slots[slotIndex];
}

}  // namespace

BackendDeviceIdentity Device::backendDeviceIdentity() const {
  return {};
}

Result<BackendTextureExport> Device::onExportTexture(uint32_t /*slotIndex*/) {
  return Err(GpuErrorType::Unsupported,
             "exportTexture: this backend reaches each native device through one runtime "
             "device, so no other runtime device can name its textures");
}

Status Device::onRegisterTexture(uint32_t /*slotIndex*/,
                                 const ExportedTextureBacking& /*backing*/) {
  return Err(GpuErrorType::Unsupported, std::string(kRegistrationUnsupported));
}

bool Device::onTextureWritePending(uint32_t /*slotIndex*/) const {
  return false;
}

details::TextureShare* Device::textureShareOf(uint32_t slotIndex) const {
  return slotIndex < textureShares_.size() ? textureShares_[slotIndex].get() : nullptr;
}

const Device::TextureRegistration* Device::textureRegistrationOf(uint32_t slotIndex) const {
  if (slotIndex >= textureRegistrations_.size() || !textureRegistrations_[slotIndex]) {
    return nullptr;
  }
  return &*textureRegistrations_[slotIndex];
}

Status Device::checkTextureExportable(const Texture& texture,
                                      const TextureDescriptor& descriptor) const {
  if (isLost()) {
    return Err(GpuErrorType::DeviceLost,
               std::format("exportTexture: texture \"{}\" belongs to a lost device, whose "
                           "contents cannot be trusted",
                           descriptor.label.str()));
  }
  if (textureRegistrationOf(texture.slotIndex()) != nullptr) {
    return Err(GpuErrorType::InvalidState,
               std::format("exportTexture: texture \"{}\" is a registration of another device's "
                           "texture; export it from the device that allocated it",
                           descriptor.label.str()));
  }
  return OkStatus();
}

Status Device::checkSurfaceFrameExport(
    const Texture& texture, const TextureDescriptor& descriptor,
    const Result<std::shared_ptr<details::TextureShare>>& created) const {
  if (!namesAcquiredSurfaceFrame(texture) ||
      (created.hasResult() && created.result()->ordering() == SourceOrdering::SharedQueue)) {
    return OkStatus();
  }
  return Err(GpuErrorType::InvalidState,
             std::format("exportTexture: texture \"{}\" is the frame a surface has out, and a "
                         "reader on another queue could still be reading it after the surface "
                         "takes it back at present",
                         descriptor.label.str()));
}

Result<std::shared_ptr<details::TextureShare>> Device::createTextureShare(
    uint32_t slotIndex, const TextureDescriptor& descriptor) {
  Result<BackendTextureExport> exported = onExportTexture(slotIndex);
  if (exported.hasError()) {
    return std::move(exported).error();
  }
  BackendTextureExport backend = std::move(exported).result();
  const BackendDeviceIdentity identity = backendDeviceIdentity();
  if (!identity.isValid() || backend.backing == nullptr ||
      (backend.ordering != SourceOrdering::SharedQueue && backend.completion == nullptr)) {
    return Err(GpuErrorType::InvalidState,
               "exportTexture: the backend reported an incomplete export");
  }
  // Submissions this device accepted before the export are recorded only here, not in the share.
  backend.contentSerial = std::max(backend.contentSerial, textures_.lastUseOf(slotIndex));
  return std::make_shared<details::TextureShare>(descriptor, deviceId_, identity, lostState_,
                                                 std::move(backend), sharedTextureTailBytes_);
}

Result<TextureExport> Device::exportTexture(const Texture& texture) {
  auto record = resolve(textures_, texture, TextureTag::kName);
  if (record.hasError()) {
    return std::move(record).error();
  }
  const TextureDescriptor& descriptor = record.result()->descriptor;
  if (Status exportable = checkTextureExportable(texture, descriptor); exportable.hasError()) {
    return std::move(exportable).error();
  }
  std::shared_ptr<details::TextureShare>& share =
      SideTableSlot(textureShares_, texture.slotIndex());
  if (share == nullptr) {
    Result<std::shared_ptr<details::TextureShare>> created =
        createTextureShare(texture.slotIndex(), descriptor);
    if (created.hasError()) {
      return std::move(created).error();
    }
    if (Status frame = checkSurfaceFrameExport(texture, descriptor, created); frame.hasError()) {
      return std::move(frame).error();
    }
    share = std::move(created).result();
    if (share->writePending()) {
      pendingSharedTextureWrites_.push_back(texture.slotIndex());
    }
  }
  return TextureExport(std::make_shared<const details::TextureShareLease>(share));
}

void Device::releaseTextureBackingOrDefer(uint32_t slotIndex) {
  details::TextureShare* share = textureShareOf(slotIndex);
  if (share != nullptr && share->heldElsewhere()) {
    // Another device may still be reading the allocation, so it is released when the last
    // holder lets go rather than under that reader.
    share->requestBackingRelease();
    return;
  }
  onDestroyTextureBacking(slotIndex);
}

Status Device::checkRegistrationIdentity(const details::TextureShare& share) const {
  const BackendDeviceIdentity identity = backendDeviceIdentity();
  if (!identity.isValid()) {
    return Err(GpuErrorType::Unsupported, std::string(kRegistrationUnsupported));
  }
  if (identity.family != share.identity().family) {
    return Err(GpuErrorType::DeviceMismatch,
               std::format("registerTexture: texture \"{}\" belongs to a device of another "
                           "backend",
                           share.descriptor().label.str()));
  }
  if (identity.nativeDevice != share.identity().nativeDevice) {
    return Err(GpuErrorType::DeviceMismatch,
               std::format("registerTexture: texture \"{}\" belongs to a different native device",
                           share.descriptor().label.str()));
  }
  return OkStatus();
}

Status Device::checkRegistrationSource(const details::TextureShare& share) const {
  if (share.producerDeviceId() == deviceId_) {
    return Err(GpuErrorType::InvalidState,
               std::format("registerTexture: texture \"{}\" is already a texture of this device; "
                           "use its own handle",
                           share.descriptor().label.str()));
  }
  if (isLost() || share.producerLost()) {
    return Err(GpuErrorType::DeviceLost,
               std::format("registerTexture: texture \"{}\" cannot be registered once either "
                           "device is lost",
                           share.descriptor().label.str()));
  }
  if (share.producerReleased()) {
    return Err(GpuErrorType::InvalidHandle,
               std::format("registerTexture: the producing device no longer names texture \"{}\"",
                           share.descriptor().label.str()));
  }
  if (Status identity = checkRegistrationIdentity(share); identity.hasError()) {
    return identity;
  }
  if (share.writePending()) {
    return Err(GpuErrorType::InvalidState,
               std::format("registerTexture: a write to texture \"{}\" is queued for the "
                           "producer's next submission; register it once that is submitted",
                           share.descriptor().label.str()));
  }
  return OkStatus();
}

Result<Texture> Device::registerTexture(const TextureExport& source) {
  if (!source.isValid()) {
    return Err(GpuErrorType::InvalidHandle, "registerTexture: the export names no texture");
  }
  const std::shared_ptr<details::TextureShare>& share = source.lease_->share();
  if (Status admissible = checkRegistrationSource(*share); admissible.hasError()) {
    return std::move(admissible).error();
  }
  TextureDescriptor descriptor = share->descriptor();
  descriptor.usage = descriptor.usage & kRegistrationUsage;
  if (descriptor.usage == TextureUsage::None) {
    return Err(GpuErrorType::UsageMismatch,
               std::format("registerTexture: texture \"{}\" can be neither sampled nor copied "
                           "from, so a registration could do nothing with it",
                           share->descriptor().label.str()));
  }
  const uint64_t orderAfterSerial = share->contentSerial();

  Texture handle = allocateHandle<TextureTag>(textures_, TextureRecord{std::move(descriptor)});
  if (Status status = onRegisterTexture(handle.slotIndex(), share->backing()); status.hasError()) {
    textures_.release(handle.slotIndex());
    return std::move(status).error();
  }
  SideTableSlot(textureRegistrations_, handle.slotIndex()) = TextureRegistration{
      std::make_shared<const details::TextureShareLease>(share), orderAfterSerial};
  return handle;
}

std::optional<bool> Device::textureSourceState(const TextureRegistration& entry) const {
  const details::TextureShare& share = *entry.lease->share();
  if (isLost() || share.producerLost()) {
    return false;
  }
  const SubmissionCompletion& completion = *share.completion();
  // Completion is read before the failure flag: a backend publishes a failure before the serial
  // of the work that failed, so a serial seen complete here cannot hide that failure.
  const uint64_t completedSerial = completion.completedSerial();
  if (completion.failed()) {
    DeclareProducerFailure(share);
    return false;
  }
  if (completedSerial >= entry.orderAfterSerial) {
    return true;
  }
  if (ordersOnDevice(share) && completion.committedSerial() >= entry.orderAfterSerial) {
    return true;
  }
  return std::nullopt;
}

bool Device::ordersOnDevice(const details::TextureShare& share) const {
  // A device-side wait ends when the producer's backend signals it, which it also does for work
  // that failed and for a loss of the producer's root. Only a consumer that shares that loss
  // condition learns of either, so any other consumer waits for the outcome on the host.
  return share.ordering() == SourceOrdering::WaitOnDevice &&
         &share.producerLostState() == lostState_.get();
}

bool Device::waitForTextureSource(const Texture& registration, double timeoutSeconds) {
  if (validateTextureHandleForBackend(registration).hasError()) {
    return false;
  }
  const TextureRegistration* entry = textureRegistrationOf(registration.slotIndex());
  if (entry == nullptr || entry->lease->share()->ordering() == SourceOrdering::SharedQueue) {
    return true;
  }
  const double clampedSeconds =
      timeoutSeconds > 0.0 ? std::min(timeoutSeconds, kMaxWaitSeconds) : 0.0;
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(clampedSeconds));
  constexpr std::chrono::milliseconds kRecheckInterval{1};
  for (;;) {
    if (const std::optional<bool> settled = textureSourceState(*entry)) {
      return *settled;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kRecheckInterval);
  }
}

uint64_t Device::sharedTextureTailBytes() const {
  return sharedTextureTailBytes_->load(std::memory_order_relaxed);
}

namespace {

/// Records that a submission waits on the device for \p backing's producer work through
/// \p serial, merging it into a wait the submission already carries for the same texture.
/// @param waits Waits of the submission. @param backing Export the registration was made from.
/// @param serial Producer serial the registration follows.
void AddSourceWait(std::vector<SourceWait>& waits, const ExportedTextureBacking& backing,
                   uint64_t serial) {
  const auto sameBacking = [&backing](const SourceWait& wait) { return wait.backing == &backing; };
  if (auto recorded = std::ranges::find_if(waits, sameBacking); recorded != waits.end()) {
    recorded->serial = std::max(recorded->serial, serial);
    return;
  }
  waits.push_back(SourceWait{&backing, serial});
}

/// Why a submission naming a registration whose producer work is unfinished is refused.
/// @param share Share of the registered texture. @param slotIndex Slot of the registration.
/// @param onDevice Whether the device orders work naming the registration.
GpuError SourceNotReadyError(const details::TextureShare& share, uint32_t slotIndex,
                             bool onDevice) {
  if (onDevice) {
    return Err(GpuErrorType::InvalidState,
               std::format("submit: registered texture \"{}\" (slot {}) follows producer work "
                           "its producer has not submitted yet; wait for it with "
                           "waitForTextureSource before submitting work that reads it",
                           share.descriptor().label.str(), slotIndex));
  }
  return Err(GpuErrorType::InvalidState,
             std::format("submit: registered texture \"{}\" (slot {}) is still being "
                         "written by its producer; wait for it with waitForTextureSource "
                         "before submitting work that reads it",
                         share.descriptor().label.str(), slotIndex));
}

}  // namespace

Status Device::checkTextureSourceReady(const TextureRegistration& entry, uint32_t slotIndex,
                                       std::vector<SourceWait>& waits) const {
  const details::TextureShare& share = *entry.lease->share();
  const SubmissionCompletion* completion = share.completion();
  // Completion first, for the reason given in textureSourceState.
  const uint64_t completedSerial = completion != nullptr ? completion->completedSerial() : 0;
  const bool failed = completion != nullptr && completion->failed();
  if (failed) {
    DeclareProducerFailure(share);
  }
  if (isLost() || share.producerLost() || failed) {
    return Err(GpuErrorType::DeviceLost,
               std::format("submit: registered texture \"{}\" (slot {}) comes from a device "
                           "that is lost or failed; its contents cannot be trusted",
                           share.descriptor().label.str(), slotIndex));
  }
  if (share.ordering() == SourceOrdering::SharedQueue ||
      completedSerial >= entry.orderAfterSerial) {
    return OkStatus();
  }
  // Only work the producer has handed to its queue is waited for on the device. A device-side
  // wait on work still being recorded could wait for a host thread that waits for this one.
  const bool onDevice = ordersOnDevice(share);
  if (onDevice && completion->committedSerial() >= entry.orderAfterSerial) {
    AddSourceWait(waits, share.backing(), entry.orderAfterSerial);
    return OkStatus();
  }
  return SourceNotReadyError(share, slotIndex, onDevice);
}

Status Device::checkSubmissionTextureSources(std::span<const SubmissionUse> uses,
                                             std::vector<SourceWait>& waits) const {
  for (const SubmissionUse& use : uses) {
    if (use.kind != ResourceKind::Texture) {
      continue;
    }
    const TextureRegistration* entry = textureRegistrationOf(use.slotIndex);
    if (entry == nullptr) {
      continue;
    }
    if (Status ready = checkTextureSourceReady(*entry, use.slotIndex, waits); ready.hasError()) {
      return ready;
    }
  }
  return OkStatus();
}

void Device::noteSubmittedTextureShares(std::span<const SubmissionUse> uses,
                                        uint64_t submissionSerial) {
  for (const SubmissionUse& use : uses) {
    if (use.kind != ResourceKind::Texture) {
      continue;
    }
    if (details::TextureShare* share = textureShareOf(use.slotIndex)) {
      share->noteProducerUse(submissionSerial);
    }
  }
  for (const uint32_t slotIndex : pendingSharedTextureWrites_) {
    if (details::TextureShare* share = textureShareOf(slotIndex)) {
      share->noteWritesCarried(submissionSerial);
    }
  }
  pendingSharedTextureWrites_.clear();
}

void Device::noteTextureWriteForShares(uint32_t slotIndex) {
  details::TextureShare* share = textureShareOf(slotIndex);
  // A texture already waiting for the next submission is already listed for it.
  if (share == nullptr || share->writePending() || !onTextureWritePending(slotIndex)) {
    return;
  }
  share->noteWritePending();
  pendingSharedTextureWrites_.push_back(slotIndex);
}

void Device::releaseTextureShare(uint32_t slotIndex) {
  if (slotIndex >= textureShares_.size() || textureShares_[slotIndex] == nullptr) {
    return;
  }
  textureShares_[slotIndex]->releaseProducer();
  textureShares_[slotIndex].reset();
}

void Device::releaseTextureRegistration(uint32_t slotIndex) {
  if (slotIndex < textureRegistrations_.size()) {
    textureRegistrations_[slotIndex].reset();
  }
}

}  // namespace donner::gpu
