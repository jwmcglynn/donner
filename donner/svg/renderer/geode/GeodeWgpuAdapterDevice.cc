#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

#include <cassert>
#include <chrono>
#include <format>
#include <string_view>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"

namespace donner::geode {

namespace {

using gpu::GpuError;
using gpu::GpuErrorType;
using gpu::OkStatus;

/// Ensures \p table covers \p slotIndex and stores \p value there. Slots are value-initialized
/// (null handles) until written.
template <typename T>
void SetSlot(std::vector<T>& table, uint32_t slotIndex, T value) {
  if (table.size() <= slotIndex) {
    table.resize(slotIndex + 1);
  }
  table[slotIndex] = std::move(value);
}

/// Returns a borrowed alias of the wgpu handle at \p slotIndex, or a null handle if the slot is
/// out of range or dead.
template <typename Handle>
Handle GetHandle(const std::vector<ScopedWgpuHandle<Handle>>& table, uint32_t slotIndex) {
  return slotIndex < table.size() ? table[slotIndex].get() : Handle{};
}

/// Overload set for exhaustive std::visit dispatch: adding a new `gpu::Command` alternative
/// without a matching handler is a compile error instead of a silently dropped command.
template <typename... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

wgpu::TextureFormat ToWgpuTextureFormat(gpu::TextureFormat format) {
  switch (format) {
    case gpu::TextureFormat::RGBA8Unorm: return wgpu::TextureFormat::RGBA8Unorm;
    case gpu::TextureFormat::BGRA8Unorm: return wgpu::TextureFormat::BGRA8Unorm;
    case gpu::TextureFormat::R8Unorm: return wgpu::TextureFormat::R8Unorm;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated TextureFormat out of range");
  return wgpu::TextureFormat::RGBA8Unorm;
}

wgpu::BufferUsage ToWgpuBufferUsage(gpu::BufferUsage usage) {
  WGPUBufferUsage result = wgpu::BufferUsage::None;
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Vertex)) {
    result |= wgpu::BufferUsage::Vertex;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Index)) {
    result |= wgpu::BufferUsage::Index;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Uniform)) {
    result |= wgpu::BufferUsage::Uniform;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::Storage)) {
    result |= wgpu::BufferUsage::Storage;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::CopySrc)) {
    result |= wgpu::BufferUsage::CopySrc;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::CopyDst)) {
    result |= wgpu::BufferUsage::CopyDst;
  }
  if (gpu::HasAllFlags(usage, gpu::BufferUsage::MapRead)) {
    result |= wgpu::BufferUsage::MapRead;
  }
  return result;
}

wgpu::TextureUsage ToWgpuTextureUsage(gpu::TextureUsage usage) {
  WGPUTextureUsage result = wgpu::TextureUsage::None;
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::RenderAttachment)) {
    result |= wgpu::TextureUsage::RenderAttachment;
  }
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::Sampled)) {
    result |= wgpu::TextureUsage::TextureBinding;
  }
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::CopySrc)) {
    result |= wgpu::TextureUsage::CopySrc;
  }
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::CopyDst)) {
    result |= wgpu::TextureUsage::CopyDst;
  }
  return result;
}

wgpu::ShaderStage ToWgpuShaderStage(gpu::ShaderStage visibility) {
  WGPUShaderStage result = wgpu::ShaderStage::None;
  if (gpu::HasAllFlags(visibility, gpu::ShaderStage::Vertex)) {
    result |= wgpu::ShaderStage::Vertex;
  }
  if (gpu::HasAllFlags(visibility, gpu::ShaderStage::Fragment)) {
    result |= wgpu::ShaderStage::Fragment;
  }
  if (gpu::HasAllFlags(visibility, gpu::ShaderStage::Compute)) {
    result |= wgpu::ShaderStage::Compute;
  }
  return result;
}

wgpu::ColorWriteMask ToWgpuColorWriteMask(gpu::ColorWriteMask mask) {
  WGPUColorWriteMask result = wgpu::ColorWriteMask::None;
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Red)) {
    result |= wgpu::ColorWriteMask::Red;
  }
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Green)) {
    result |= wgpu::ColorWriteMask::Green;
  }
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Blue)) {
    result |= wgpu::ColorWriteMask::Blue;
  }
  if (gpu::HasAllFlags(mask, gpu::ColorWriteMask::Alpha)) {
    result |= wgpu::ColorWriteMask::Alpha;
  }
  return result;
}

wgpu::FilterMode ToWgpuFilterMode(gpu::FilterMode mode) {
  switch (mode) {
    case gpu::FilterMode::Nearest: return wgpu::FilterMode::Nearest;
    case gpu::FilterMode::Linear: return wgpu::FilterMode::Linear;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated FilterMode out of range");
  return wgpu::FilterMode::Nearest;
}

wgpu::AddressMode ToWgpuAddressMode(gpu::AddressMode mode) {
  switch (mode) {
    case gpu::AddressMode::ClampToEdge: return wgpu::AddressMode::ClampToEdge;
    case gpu::AddressMode::Repeat: return wgpu::AddressMode::Repeat;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated AddressMode out of range");
  return wgpu::AddressMode::ClampToEdge;
}

wgpu::VertexFormat ToWgpuVertexFormat(gpu::VertexFormat format) {
  switch (format) {
    case gpu::VertexFormat::Float32x2: return wgpu::VertexFormat::Float32x2;
    case gpu::VertexFormat::Float32x4: return wgpu::VertexFormat::Float32x4;
    case gpu::VertexFormat::Uint32: return wgpu::VertexFormat::Uint32;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexFormat out of range");
  return wgpu::VertexFormat::Float32x2;
}

wgpu::VertexStepMode ToWgpuVertexStepMode(gpu::VertexStepMode mode) {
  switch (mode) {
    case gpu::VertexStepMode::Vertex: return wgpu::VertexStepMode::Vertex;
    case gpu::VertexStepMode::Instance: return wgpu::VertexStepMode::Instance;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexStepMode out of range");
  return wgpu::VertexStepMode::Vertex;
}

wgpu::PrimitiveTopology ToWgpuPrimitiveTopology(gpu::PrimitiveTopology topology) {
  switch (topology) {
    case gpu::PrimitiveTopology::TriangleList: return wgpu::PrimitiveTopology::TriangleList;
    case gpu::PrimitiveTopology::TriangleStrip: return wgpu::PrimitiveTopology::TriangleStrip;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated PrimitiveTopology out of range");
  return wgpu::PrimitiveTopology::TriangleList;
}

wgpu::CullMode ToWgpuCullMode(gpu::CullMode mode) {
  switch (mode) {
    case gpu::CullMode::None: return wgpu::CullMode::None;
    case gpu::CullMode::Back: return wgpu::CullMode::Back;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated CullMode out of range");
  return wgpu::CullMode::None;
}

wgpu::BlendFactor ToWgpuBlendFactor(gpu::BlendFactor factor) {
  switch (factor) {
    case gpu::BlendFactor::Zero: return wgpu::BlendFactor::Zero;
    case gpu::BlendFactor::One: return wgpu::BlendFactor::One;
    case gpu::BlendFactor::SrcAlpha: return wgpu::BlendFactor::SrcAlpha;
    case gpu::BlendFactor::OneMinusSrcAlpha: return wgpu::BlendFactor::OneMinusSrcAlpha;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendFactor out of range");
  return wgpu::BlendFactor::Zero;
}

wgpu::BlendOperation ToWgpuBlendOperation(gpu::BlendOperation operation) {
  switch (operation) {
    case gpu::BlendOperation::Add: return wgpu::BlendOperation::Add;
    case gpu::BlendOperation::Max: return wgpu::BlendOperation::Max;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendOperation out of range");
  return wgpu::BlendOperation::Add;
}

wgpu::LoadOp ToWgpuLoadOp(gpu::LoadOp op) {
  switch (op) {
    case gpu::LoadOp::Clear: return wgpu::LoadOp::Clear;
    case gpu::LoadOp::Load: return wgpu::LoadOp::Load;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated LoadOp out of range");
  return wgpu::LoadOp::Clear;
}

wgpu::StoreOp ToWgpuStoreOp(gpu::StoreOp op) {
  switch (op) {
    case gpu::StoreOp::Store: return wgpu::StoreOp::Store;
    case gpu::StoreOp::Discard: return wgpu::StoreOp::Discard;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated StoreOp out of range");
  return wgpu::StoreOp::Store;
}

/// Fills the resource-kind union of a wgpu bind group layout entry for \p type.
void ApplyBindingType(wgpu::BindGroupLayoutEntry& entry, gpu::BindingType type) {
  switch (type) {
    case gpu::BindingType::UniformBuffer:
      entry.buffer.type = wgpu::BufferBindingType::Uniform;
      entry.buffer.minBindingSize = 0;
      return;
    case gpu::BindingType::ReadOnlyStorageBuffer:
      entry.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
      entry.buffer.minBindingSize = 0;
      return;
    case gpu::BindingType::SampledTexture2dFloat:
      entry.texture.sampleType = wgpu::TextureSampleType::Float;
      entry.texture.viewDimension = wgpu::TextureViewDimension::_2D;
      entry.texture.multisampled = false;
      return;
    case gpu::BindingType::FilteringSampler:
      entry.sampler.type = wgpu::SamplerBindingType::Filtering;
      return;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BindingType out of range");
}

}  // namespace

GeodeWgpuAdapterDevice::GeodeWgpuAdapterDevice(GeodeDevice& geodeDevice)
    : geodeDevice_(geodeDevice) {}

GeodeWgpuAdapterDevice::~GeodeWgpuAdapterDevice() {
  // Wait for in-flight submissions so deferred destructions drain before the slot vectors
  // release the remaining wgpu objects. On timeout teardown proceeds anyway: wgpu retains every
  // resource referenced by a submitted command buffer until it completes.
  if (lastSubmittedSerial() > completedSerial()) {
    waitForSerial(lastSubmittedSerial(), /*timeoutSeconds=*/5.0);
  }
  poll();
}

uint64_t GeodeWgpuAdapterDevice::completedSerial() const {
  return completionState_->completedSerial.load(std::memory_order_acquire);
}

bool GeodeWgpuAdapterDevice::waitForSerial(uint64_t serial, double timeoutSeconds) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(timeoutSeconds));
  // Bounded like GeodeDevice's WaitForSubmittedWork: poll(true) blocks until pending work
  // progresses (yielding through Asyncify on Emscripten), so iterations are cheap when idle.
  for (int pollIter = 0; pollIter < 20000; ++pollIter) {
    if (completedSerial() >= serial) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    geodeDevice_.device().poll(true, nullptr);
  }
  return completedSerial() >= serial;
}

gpu::Result<gpu::Texture> GeodeWgpuAdapterDevice::importExternalTexture(wgpu::Texture texture,
                                                                        const gpu::Extent2d& size,
                                                                        gpu::TextureFormat format,
                                                                        gpu::TextureUsage usage) {
  if (!texture) {
    return GpuError{GpuErrorType::InvalidHandle, "importExternalTexture: wgpu texture is null"};
  }

  pendingImport_ = texture;
  gpu::Result<gpu::Texture> result =
      createTexture(gpu::TextureDescriptor{"externalTexture", size, format, usage});
  pendingImport_ = wgpu::Texture();  // Cleared on the failure paths too.
  return result;
}

wgpu::Texture GeodeWgpuAdapterDevice::wgpuTextureOf(const gpu::Texture& texture) const {
  // Full base-class validation (null, device identity, AND generation), so a stale or forged
  // handle cannot bridge the slot's new occupant to raw wgpu.
  if (validateTextureHandleForBackend(texture).hasError() ||
      texture.slotIndex() >= slotTextures_.size()) {
    return wgpu::Texture();
  }
  return slotTextures_[texture.slotIndex()].texture;
}

wgpu::TextureView GeodeWgpuAdapterDevice::wgpuTextureViewOf(
    const gpu::TextureView& textureView) const {
  // Full base-class validation including viewed-texture re-resolution, so a view whose Donner
  // texture was destroyed (or slot-recycled) fails closed here exactly like it does on every
  // normal Device path instead of bridging a stale view to raw wgpu.
  if (validateTextureViewHandleForBackend(textureView).hasError()) {
    return wgpu::TextureView();
  }
  return GetHandle(slotTextureViews_, textureView.slotIndex());
}

wgpu::RenderPipeline GeodeWgpuAdapterDevice::wgpuRenderPipelineOf(
    const gpu::RenderPipeline& pipeline) const {
  if (!pipeline.isValid() || pipeline.deviceId() != deviceId()) {
    return wgpu::RenderPipeline();
  }
  return GetHandle(slotRenderPipelines_, pipeline.slotIndex());
}

wgpu::BindGroupLayout GeodeWgpuAdapterDevice::wgpuBindGroupLayoutOf(
    const gpu::BindGroupLayout& layout) const {
  if (!layout.isValid() || layout.deviceId() != deviceId()) {
    return wgpu::BindGroupLayout();
  }
  return GetHandle(slotBindGroupLayouts_, layout.slotIndex());
}

wgpu::Sampler GeodeWgpuAdapterDevice::wgpuSamplerOf(const gpu::Sampler& sampler) const {
  if (!sampler.isValid() || sampler.deviceId() != deviceId()) {
    return wgpu::Sampler();
  }
  return GetHandle(slotSamplers_, sampler.slotIndex());
}

gpu::TextureFormat GpuTextureFormatFromWgpu(wgpu::TextureFormat format) {
  switch (static_cast<WGPUTextureFormat>(format)) {
    case WGPUTextureFormat_RGBA8Unorm: return gpu::TextureFormat::RGBA8Unorm;
    case WGPUTextureFormat_BGRA8Unorm: return gpu::TextureFormat::BGRA8Unorm;
    case WGPUTextureFormat_R8Unorm: return gpu::TextureFormat::R8Unorm;
    default: break;
  }
  UTILS_RELEASE_ASSERT_MSG(false,
                           "wgpu texture format is outside the donner::gpu supported set "
                           "(RGBA8Unorm / BGRA8Unorm / R8Unorm)");
  return gpu::TextureFormat::RGBA8Unorm;
}

gpu::Status GeodeWgpuAdapterDevice::onCreateBuffer(uint32_t slotIndex,
                                                   const gpu::BufferDescriptor& descriptor) {
  wgpu::BufferDescriptor bufferDescriptor = {};
  bufferDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  bufferDescriptor.size = descriptor.byteSize;
  bufferDescriptor.usage = ToWgpuBufferUsage(descriptor.usage);

  wgpu::Buffer buffer = geodeDevice_.device().createBuffer(bufferDescriptor);
  if (!buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu buffer allocation of {} bytes failed for '{}'",
                                descriptor.byteSize, std::string_view(descriptor.label))};
  }
  geodeDevice_.countBuffer();

  SetSlot(slotBuffers_, slotIndex, ScopedWgpuHandle<wgpu::Buffer>(buffer));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateTexture(uint32_t slotIndex,
                                                    const gpu::TextureDescriptor& descriptor) {
  if (pendingImport_) {
    // importExternalTexture path: register the borrowed texture; no ownership is taken.
    SetSlot(slotTextures_, slotIndex,
            TextureSlot{ScopedWgpuHandle<wgpu::Texture>(), pendingImport_});
    return OkStatus();
  }

  wgpu::TextureDescriptor textureDescriptor = {};
  textureDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  textureDescriptor.size = {descriptor.size.width, descriptor.size.height, 1u};
  textureDescriptor.format = ToWgpuTextureFormat(descriptor.format);
  textureDescriptor.usage = ToWgpuTextureUsage(descriptor.usage);
  textureDescriptor.mipLevelCount = 1;
  textureDescriptor.sampleCount = 1;
  textureDescriptor.dimension = wgpu::TextureDimension::_2D;

  wgpu::Texture texture = geodeDevice_.device().createTexture(textureDescriptor);
  if (!texture) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("wgpu texture allocation ({}x{}) failed for '{}'", descriptor.size.width,
                    descriptor.size.height, std::string_view(descriptor.label))};
  }
  geodeDevice_.countTexture();

  SetSlot(slotTextures_, slotIndex, TextureSlot{ScopedWgpuHandle<wgpu::Texture>(texture), texture});
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateTextureView(
    uint32_t slotIndex, uint32_t textureSlotIndex, const gpu::TextureViewDescriptor& descriptor) {
  (void)descriptor;  // Views cover the whole texture; wgpu's default view matches.
  wgpu::Texture texture = textureSlotIndex < slotTextures_.size()
                              ? slotTextures_[textureSlotIndex].texture
                              : wgpu::Texture();
  if (!texture) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("texture slot {} has no wgpu texture to view", textureSlotIndex)};
  }

  wgpu::TextureView view = texture.createView();
  if (!view) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("wgpu texture view creation failed for texture slot {}", textureSlotIndex)};
  }

  SetSlot(slotTextureViews_, slotIndex, ScopedWgpuHandle<wgpu::TextureView>(view));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateSampler(uint32_t slotIndex,
                                                    const gpu::SamplerDescriptor& descriptor) {
  // `{wgpu::Default}` fills lodMaxClamp and, critically, `maxAnisotropy = 1`, which wgpu-native
  // validates as non-zero (see GeodeImagePipeline's sampler creation).
  wgpu::SamplerDescriptor samplerDescriptor{wgpu::Default};
  samplerDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  samplerDescriptor.magFilter = ToWgpuFilterMode(descriptor.magFilter);
  samplerDescriptor.minFilter = ToWgpuFilterMode(descriptor.minFilter);
  samplerDescriptor.addressModeU = ToWgpuAddressMode(descriptor.addressModeU);
  samplerDescriptor.addressModeV = ToWgpuAddressMode(descriptor.addressModeV);
  samplerDescriptor.maxAnisotropy = 1;

  wgpu::Sampler sampler = geodeDevice_.device().createSampler(samplerDescriptor);
  if (!sampler) {
    return GpuError{GpuErrorType::InvalidState, std::format("wgpu sampler creation failed for '{}'",
                                                            std::string_view(descriptor.label))};
  }

  SetSlot(slotSamplers_, slotIndex, ScopedWgpuHandle<wgpu::Sampler>(sampler));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateBindGroupLayout(
    uint32_t slotIndex, const gpu::BindGroupLayoutDescriptor& descriptor) {
  std::vector<wgpu::BindGroupLayoutEntry> entries(descriptor.entries.size());
  for (size_t i = 0; i < descriptor.entries.size(); ++i) {
    const gpu::BindGroupLayoutEntry& entry = descriptor.entries[i];
    entries[i].binding = entry.binding;
    entries[i].visibility = ToWgpuShaderStage(entry.visibility);
    ApplyBindingType(entries[i], entry.type);
  }

  wgpu::BindGroupLayoutDescriptor layoutDescriptor = {};
  layoutDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  layoutDescriptor.entryCount = entries.size();
  layoutDescriptor.entries = entries.data();

  wgpu::BindGroupLayout layout = geodeDevice_.device().createBindGroupLayout(layoutDescriptor);
  if (!layout) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu bind group layout creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotBindGroupLayouts_, slotIndex, ScopedWgpuHandle<wgpu::BindGroupLayout>(layout));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateBindGroup(uint32_t slotIndex,
                                                      const gpu::BindGroupDescriptor& descriptor) {
  // Creation-time layout lookup only (the base class just validated the reference); the created
  // wgpu bind group retains its layout internally, so encoding never resolves layouts by slot.
  wgpu::BindGroupLayout layout = GetHandle(slotBindGroupLayouts_, descriptor.layout.slotIndex());
  if (!layout) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("bind group layout slot {} has no wgpu layout", descriptor.layout.slotIndex())};
  }

  std::vector<wgpu::BindGroupEntry> entries(descriptor.entries.size());
  for (size_t i = 0; i < descriptor.entries.size(); ++i) {
    const gpu::BindGroupEntry& entry = descriptor.entries[i];
    entries[i].binding = entry.binding;
    if (const gpu::BufferBinding* bufferBinding =
            std::get_if<gpu::BufferBinding>(&entry.resource)) {
      wgpu::Buffer buffer = GetHandle(slotBuffers_, bufferBinding->buffer.slotIndex());
      if (!buffer) {
        return GpuError{GpuErrorType::InvalidState,
                        std::format("bind group entry binding {} does not resolve to a wgpu "
                                    "buffer",
                                    entry.binding)};
      }
      entries[i].buffer = buffer;
      entries[i].offset = bufferBinding->offsetBytes;
      entries[i].size = bufferBinding->sizeBytes;
    } else if (const gpu::TextureViewBinding* viewBinding =
                   std::get_if<gpu::TextureViewBinding>(&entry.resource)) {
      wgpu::TextureView view = GetHandle(slotTextureViews_, viewBinding->view.slotIndex());
      if (!view) {
        return GpuError{GpuErrorType::InvalidState,
                        std::format("bind group entry binding {} does not resolve to a wgpu "
                                    "texture view",
                                    entry.binding)};
      }
      entries[i].textureView = view;
    } else if (const gpu::SamplerBinding* samplerBinding =
                   std::get_if<gpu::SamplerBinding>(&entry.resource)) {
      wgpu::Sampler sampler = GetHandle(slotSamplers_, samplerBinding->sampler.slotIndex());
      if (!sampler) {
        return GpuError{GpuErrorType::InvalidState,
                        std::format("bind group entry binding {} does not resolve to a wgpu "
                                    "sampler",
                                    entry.binding)};
      }
      entries[i].sampler = sampler;
    }
  }

  wgpu::BindGroupDescriptor groupDescriptor = {};
  groupDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  groupDescriptor.layout = layout;
  groupDescriptor.entryCount = entries.size();
  groupDescriptor.entries = entries.data();

  wgpu::BindGroup group = geodeDevice_.device().createBindGroup(groupDescriptor);
  if (!group) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu bind group creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }
  geodeDevice_.countBindGroup();

  SetSlot(slotBindGroups_, slotIndex, ScopedWgpuHandle<wgpu::BindGroup>(group));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreatePipelineLayout(
    uint32_t slotIndex, const gpu::PipelineLayoutDescriptor& descriptor) {
  std::vector<WGPUBindGroupLayout> layouts(descriptor.bindGroupLayouts.size());
  for (size_t i = 0; i < descriptor.bindGroupLayouts.size(); ++i) {
    wgpu::BindGroupLayout layout =
        GetHandle(slotBindGroupLayouts_, descriptor.bindGroupLayouts[i].slotIndex());
    if (!layout) {
      return GpuError{GpuErrorType::InvalidState,
                      std::format("pipeline layout references bind group layout slot {} with no "
                                  "wgpu layout",
                                  descriptor.bindGroupLayouts[i].slotIndex())};
    }
    layouts[i] = layout;
  }

  wgpu::PipelineLayoutDescriptor layoutDescriptor = {};
  layoutDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  layoutDescriptor.bindGroupLayoutCount = layouts.size();
  layoutDescriptor.bindGroupLayouts = layouts.data();

  wgpu::PipelineLayout layout = geodeDevice_.device().createPipelineLayout(layoutDescriptor);
  if (!layout) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("wgpu pipeline layout creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotPipelineLayouts_, slotIndex, ScopedWgpuHandle<wgpu::PipelineLayout>(layout));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateShaderModule(
    uint32_t slotIndex, const gpu::ShaderModuleDescriptor& descriptor) {
  if (descriptor.sourceKind != gpu::ShaderSourceKind::Wgsl) {
    return GpuError{GpuErrorType::Unsupported, "the wgpu adapter compiles WGSL only"};
  }

  // Same WGSL chaining as GeodeShaders.cc's createShaderFromWgsl: the source text rides a
  // ShaderSourceWGSL chained struct whose sType `setDefault()` fills in.
  const std::string_view source(descriptor.sourceText);
  wgpu::ShaderSourceWGSL wgslSource{wgpu::Default};
  wgslSource.code.data = source.data();
  wgslSource.code.length = source.size();

  wgpu::ShaderModuleDescriptor moduleDescriptor{wgpu::Default};
  moduleDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  moduleDescriptor.nextInChain = &wgslSource.chain;

  wgpu::ShaderModule module = geodeDevice_.device().createShaderModule(moduleDescriptor);
  if (!module) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("wgpu shader module creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotShaderModules_, slotIndex, ScopedWgpuHandle<wgpu::ShaderModule>(module));
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onCreateRenderPipeline(
    uint32_t slotIndex, const gpu::RenderPipelineDescriptor& descriptor) {
  wgpu::PipelineLayout layout = GetHandle(slotPipelineLayouts_, descriptor.layout.slotIndex());
  wgpu::ShaderModule vertexModule =
      GetHandle(slotShaderModules_, descriptor.vertex.module.slotIndex());
  wgpu::ShaderModule fragmentModule =
      GetHandle(slotShaderModules_, descriptor.fragment.module.slotIndex());
  if (!layout || !vertexModule || !fragmentModule) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("render pipeline '{}' references a layout or shader module with "
                                "no wgpu object",
                                std::string_view(descriptor.label))};
  }

  // Per-buffer attribute arrays must stay alive until createRenderPipeline returns.
  std::vector<std::vector<wgpu::VertexAttribute>> attributeStorage(
      descriptor.vertex.buffers.size());
  std::vector<wgpu::VertexBufferLayout> vertexBuffers(descriptor.vertex.buffers.size());
  for (size_t i = 0; i < descriptor.vertex.buffers.size(); ++i) {
    const gpu::VertexBufferLayout& bufferLayout = descriptor.vertex.buffers[i];
    attributeStorage[i].resize(bufferLayout.attributes.size());
    for (size_t j = 0; j < bufferLayout.attributes.size(); ++j) {
      const gpu::VertexAttribute& attribute = bufferLayout.attributes[j];
      attributeStorage[i][j].format = ToWgpuVertexFormat(attribute.format);
      attributeStorage[i][j].offset = attribute.offsetBytes;
      attributeStorage[i][j].shaderLocation = attribute.shaderLocation;
    }

    vertexBuffers[i].arrayStride = bufferLayout.strideBytes;
    vertexBuffers[i].stepMode = ToWgpuVertexStepMode(bufferLayout.stepMode);
    vertexBuffers[i].attributeCount = attributeStorage[i].size();
    vertexBuffers[i].attributes = attributeStorage[i].data();
  }

  std::vector<wgpu::BlendState> blendStorage(descriptor.fragment.targets.size());
  std::vector<wgpu::ColorTargetState> targets(descriptor.fragment.targets.size());
  for (size_t i = 0; i < descriptor.fragment.targets.size(); ++i) {
    const gpu::ColorTargetState& target = descriptor.fragment.targets[i];
    targets[i].format = ToWgpuTextureFormat(target.format);
    targets[i].writeMask = ToWgpuColorWriteMask(target.writeMask);
    if (target.blend.has_value()) {
      blendStorage[i].color.srcFactor = ToWgpuBlendFactor(target.blend->color.srcFactor);
      blendStorage[i].color.dstFactor = ToWgpuBlendFactor(target.blend->color.dstFactor);
      blendStorage[i].color.operation = ToWgpuBlendOperation(target.blend->color.operation);
      blendStorage[i].alpha.srcFactor = ToWgpuBlendFactor(target.blend->alpha.srcFactor);
      blendStorage[i].alpha.dstFactor = ToWgpuBlendFactor(target.blend->alpha.dstFactor);
      blendStorage[i].alpha.operation = ToWgpuBlendOperation(target.blend->alpha.operation);
      targets[i].blend = &blendStorage[i];
    }
  }

  const std::string_view vertexEntryPoint(descriptor.vertex.entryPoint);
  const std::string_view fragmentEntryPoint(descriptor.fragment.entryPoint);

  wgpu::FragmentState fragmentState = {};
  fragmentState.module = fragmentModule;
  fragmentState.entryPoint = wgpuLabel(fragmentEntryPoint);
  fragmentState.targetCount = targets.size();
  fragmentState.targets = targets.data();

  wgpu::RenderPipelineDescriptor pipelineDescriptor = {};
  pipelineDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  pipelineDescriptor.layout = layout;
  pipelineDescriptor.vertex.module = vertexModule;
  pipelineDescriptor.vertex.entryPoint = wgpuLabel(vertexEntryPoint);
  pipelineDescriptor.vertex.bufferCount = vertexBuffers.size();
  pipelineDescriptor.vertex.buffers = vertexBuffers.empty() ? nullptr : vertexBuffers.data();
  pipelineDescriptor.primitive.topology = ToWgpuPrimitiveTopology(descriptor.topology);
  pipelineDescriptor.primitive.cullMode = ToWgpuCullMode(descriptor.cullMode);
  pipelineDescriptor.fragment = &fragmentState;
  pipelineDescriptor.multisample.count = 1;
  pipelineDescriptor.multisample.mask = 0xFFFFFFFF;

  wgpu::RenderPipeline pipeline = geodeDevice_.device().createRenderPipeline(pipelineDescriptor);
  if (!pipeline) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("wgpu render pipeline creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotRenderPipelines_, slotIndex, ScopedWgpuHandle<wgpu::RenderPipeline>(pipeline));
  return OkStatus();
}

void GeodeWgpuAdapterDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  // Clearing a slot releases the owned wgpu reference (imported external textures carry no
  // owned reference, so their backing object is untouched).
  if (resourceName == "buffer") {
    SetSlot(slotBuffers_, slotIndex, ScopedWgpuHandle<wgpu::Buffer>());
  } else if (resourceName == "texture") {
    SetSlot(slotTextures_, slotIndex, TextureSlot{});
  } else if (resourceName == "textureView") {
    SetSlot(slotTextureViews_, slotIndex, ScopedWgpuHandle<wgpu::TextureView>());
  } else if (resourceName == "sampler") {
    SetSlot(slotSamplers_, slotIndex, ScopedWgpuHandle<wgpu::Sampler>());
  } else if (resourceName == "bindGroupLayout") {
    SetSlot(slotBindGroupLayouts_, slotIndex, ScopedWgpuHandle<wgpu::BindGroupLayout>());
  } else if (resourceName == "bindGroup") {
    SetSlot(slotBindGroups_, slotIndex, ScopedWgpuHandle<wgpu::BindGroup>());
  } else if (resourceName == "pipelineLayout") {
    SetSlot(slotPipelineLayouts_, slotIndex, ScopedWgpuHandle<wgpu::PipelineLayout>());
  } else if (resourceName == "shaderModule") {
    SetSlot(slotShaderModules_, slotIndex, ScopedWgpuHandle<wgpu::ShaderModule>());
  } else if (resourceName == "renderPipeline") {
    SetSlot(slotRenderPipelines_, slotIndex, ScopedWgpuHandle<wgpu::RenderPipeline>());
  } else {
    // A resource kind this adapter does not track would leak its wgpu object silently. Loud in
    // debug so a new kind added to the runtime is wired up here; release-safe no-op (the base
    // class owns the bookkeeping either way).
    assert(false && "GeodeWgpuAdapterDevice::onDestroyResource: unknown resource kind");
  }
}

gpu::Status GeodeWgpuAdapterDevice::onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                                                  std::span<const uint8_t> data) {
  wgpu::Buffer buffer = GetHandle(slotBuffers_, slotIndex);
  if (!buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("buffer slot {} has no wgpu buffer", slotIndex)};
  }
  // WebGPU's writeBuffer requires 4-byte-aligned offset and size ("GPUQueue.writeBuffer"
  // validation); fail closed here instead of surfacing an asynchronous device error.
  if (offsetBytes % 4 != 0 || data.size() % 4 != 0) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("writeBuffer: offsetBytes {} / byteCount {} must be 4-byte "
                                "aligned for the wgpu adapter",
                                offsetBytes, data.size())};
  }

  geodeDevice_.queue().writeBuffer(buffer, offsetBytes, data.data(), data.size());
  geodeDevice_.countBufferWrite(data.size());
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onWriteTexture(uint32_t slotIndex,
                                                   std::span<const uint8_t> data,
                                                   const gpu::TexelCopyBufferLayout& dataLayout,
                                                   const gpu::Extent2d& writeSize) {
  wgpu::Texture texture =
      slotIndex < slotTextures_.size() ? slotTextures_[slotIndex].texture : wgpu::Texture();
  if (!texture) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("texture slot {} has no wgpu texture", slotIndex)};
  }

  wgpu::TexelCopyTextureInfo destination = {};
  destination.texture = texture;
  wgpu::TexelCopyBufferLayout layout = {};
  layout.offset = dataLayout.offsetBytes;
  layout.bytesPerRow = dataLayout.bytesPerRow;
  layout.rowsPerImage = dataLayout.rowsPerImage;
  const wgpu::Extent3D extent = {writeSize.width, writeSize.height, 1u};
  geodeDevice_.queue().writeTexture(destination, data.data(), data.size(), layout, extent);
  geodeDevice_.countTextureWrite(data.size());
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::onSubmit(uint64_t submissionSerial,
                                             uint32_t commandBufferSlotIndex,
                                             std::span<const gpu::Command> commands) {
  (void)commandBufferSlotIndex;

  ScopedWgpuHandle<wgpu::CommandEncoder> encoder(geodeDevice_.device().createCommandEncoder());
  if (!encoder) {
    return GpuError{GpuErrorType::InvalidState, "wgpu command encoder creation failed"};
  }

  ScopedWgpuHandle<wgpu::RenderPassEncoder> pass;

  // On any encoding failure, close an open pass before returning so the un-finished command
  // encoder tears down cleanly, then fail closed.
  const auto failEncoding = [&pass](GpuError error) -> gpu::Status {
    if (pass) {
      pass.get().end();
      pass.reset();
    }
    return std::move(error);
  };

  // Exhaustive dispatch: every `gpu::Command` alternative has a handler, so adding a new
  // command to the variant without wiring it here is a compile error instead of a validated,
  // submitted, "completed", never-executed no-op.
  for (const gpu::Command& command : commands) {
    gpu::Status commandStatus = std::visit(
        Overloaded{
            [&](const gpu::BeginRenderPassCommand& beginPass) -> gpu::Status {
              const auto& attachments = beginPass.descriptor.colorAttachments;
              std::vector<wgpu::RenderPassColorAttachment> colorAttachments(attachments.size());
              for (size_t i = 0; i < attachments.size(); ++i) {
                const gpu::RenderPassColorAttachment& attachment = attachments[i];
                wgpu::TextureView view = GetHandle(slotTextureViews_, attachment.view.slotIndex());
                if (!view) {
                  return failEncoding(GpuError{
                      GpuErrorType::InvalidState,
                      std::format("render pass attachment {} does not resolve to a wgpu texture "
                                  "view",
                                  i)});
                }
                colorAttachments[i].view = view;
                colorAttachments[i].loadOp = ToWgpuLoadOp(attachment.loadOp);
                colorAttachments[i].storeOp = ToWgpuStoreOp(attachment.storeOp);
                colorAttachments[i].clearValue = {
                    attachment.clearColor[0], attachment.clearColor[1], attachment.clearColor[2],
                    attachment.clearColor[3]};
                // Dawn (browser WebGPU) rejects depthSlice=0 on non-3D views; wgpu-native is
                // lenient. Set the UNDEFINED sentinel for cross-backend compatibility (see
                // GeoEncoder).
                colorAttachments[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
              }

              wgpu::RenderPassDescriptor passDescriptor = {};
              passDescriptor.label = wgpuLabel(std::string_view(beginPass.descriptor.label));
              passDescriptor.colorAttachmentCount = colorAttachments.size();
              passDescriptor.colorAttachments = colorAttachments.data();
              pass.reset(encoder.get().beginRenderPass(passDescriptor));
              if (!pass) {
                return GpuError{GpuErrorType::InvalidState, "wgpu render pass creation failed"};
              }
              return OkStatus();
            },
            [&](const gpu::SetPipelineCommand& setPipeline) -> gpu::Status {
              wgpu::RenderPipeline pipeline =
                  GetHandle(slotRenderPipelines_, setPipeline.pipelineId.slotIndex);
              if (!pass || !pipeline) {
                return failEncoding(
                    GpuError{GpuErrorType::InvalidState,
                             std::format("setPipeline: pipeline slot {} is not encodable",
                                         setPipeline.pipelineId.slotIndex)});
              }
              pass.get().setPipeline(pipeline);
              return OkStatus();
            },
            [&](const gpu::SetBindGroupCommand& setBindGroup) -> gpu::Status {
              wgpu::BindGroup group =
                  GetHandle(slotBindGroups_, setBindGroup.bindGroupId.slotIndex);
              if (!pass || !group) {
                return failEncoding(
                    GpuError{GpuErrorType::InvalidState,
                             std::format("setBindGroup: bind group slot {} is not encodable",
                                         setBindGroup.bindGroupId.slotIndex)});
              }
              pass.get().setBindGroup(setBindGroup.index, group, 0, nullptr);
              return OkStatus();
            },
            [&](const gpu::SetVertexBufferCommand& setVertexBuffer) -> gpu::Status {
              wgpu::Buffer buffer = GetHandle(slotBuffers_, setVertexBuffer.bufferId.slotIndex);
              if (!pass || !buffer) {
                return failEncoding(
                    GpuError{GpuErrorType::InvalidState,
                             std::format("setVertexBuffer: buffer slot {} is not encodable",
                                         setVertexBuffer.bufferId.slotIndex)});
              }
              pass.get().setVertexBuffer(setVertexBuffer.slot, buffer, setVertexBuffer.offsetBytes,
                                         WGPU_WHOLE_SIZE);
              return OkStatus();
            },
            [&](const gpu::SetScissorRectCommand& setScissor) -> gpu::Status {
              if (!pass) {
                return failEncoding(
                    GpuError{GpuErrorType::InvalidState, "setScissorRect outside a render pass"});
              }
              pass.get().setScissorRect(setScissor.x, setScissor.y, setScissor.width,
                                        setScissor.height);
              return OkStatus();
            },
            [&](const gpu::SetViewportCommand& setViewport) -> gpu::Status {
              if (!pass) {
                return failEncoding(
                    GpuError{GpuErrorType::InvalidState, "setViewport outside a render pass"});
              }
              pass.get().setViewport(setViewport.x, setViewport.y, setViewport.width,
                                     setViewport.height, setViewport.minDepth,
                                     setViewport.maxDepth);
              return OkStatus();
            },
            [&](const gpu::DrawCommand& draw) -> gpu::Status {
              if (!pass) {
                return failEncoding(
                    GpuError{GpuErrorType::InvalidState, "draw outside a render pass"});
              }
              pass.get().draw(draw.vertexCount, draw.instanceCount, draw.firstVertex,
                              draw.firstInstance);
              geodeDevice_.countDraw();
              return OkStatus();
            },
            [&](const gpu::EndRenderPassCommand&) -> gpu::Status {
              if (!pass) {
                return GpuError{GpuErrorType::InvalidState,
                                "endRenderPass without an active render pass"};
              }
              pass.get().end();
              pass.reset();
              return OkStatus();
            },
            [&](const gpu::CopyTextureToBufferCommand& copy) -> gpu::Status {
              if (pass) {
                return failEncoding(GpuError{GpuErrorType::InvalidState,
                                             "copyTextureToBuffer inside a render pass"});
              }
              wgpu::Texture texture = copy.textureId.slotIndex < slotTextures_.size()
                                          ? slotTextures_[copy.textureId.slotIndex].texture
                                          : wgpu::Texture();
              wgpu::Buffer buffer = GetHandle(slotBuffers_, copy.bufferId.slotIndex);
              if (!texture || !buffer) {
                return GpuError{
                    GpuErrorType::InvalidState,
                    "copyTextureToBuffer: source texture or destination buffer is missing"};
              }
              wgpu::TexelCopyTextureInfo source = {};
              source.texture = texture;
              wgpu::TexelCopyBufferInfo destination = {};
              destination.buffer = buffer;
              destination.layout.offset = copy.layout.offsetBytes;
              destination.layout.bytesPerRow = copy.layout.bytesPerRow;
              destination.layout.rowsPerImage = copy.layout.rowsPerImage;
              const wgpu::Extent3D extent = {copy.copySize.width, copy.copySize.height, 1u};
              encoder.get().copyTextureToBuffer(source, destination, extent);
              return OkStatus();
            },
            [&](const gpu::CopyTextureToTextureCommand& textureCopy) -> gpu::Status {
              if (pass) {
                return failEncoding(GpuError{GpuErrorType::InvalidState,
                                             "copyTextureToTexture inside a render pass"});
              }
              wgpu::Texture sourceTexture =
                  textureCopy.textureSrcId.slotIndex < slotTextures_.size()
                      ? slotTextures_[textureCopy.textureSrcId.slotIndex].texture
                      : wgpu::Texture();
              wgpu::Texture destinationTexture =
                  textureCopy.textureDstId.slotIndex < slotTextures_.size()
                      ? slotTextures_[textureCopy.textureDstId.slotIndex].texture
                      : wgpu::Texture();
              if (!sourceTexture || !destinationTexture) {
                return GpuError{GpuErrorType::InvalidState,
                                "copyTextureToTexture: source or destination texture is missing"};
              }
              wgpu::TexelCopyTextureInfo source = {};
              source.texture = sourceTexture;
              wgpu::TexelCopyTextureInfo destination = {};
              destination.texture = destinationTexture;
              const wgpu::Extent3D extent = {textureCopy.copySize.width,
                                             textureCopy.copySize.height, 1u};
              encoder.get().copyTextureToTexture(source, destination, extent);
              return OkStatus();
            },
        },
        command);
    if (commandStatus.hasError()) {
      return commandStatus;
    }
  }

  if (pass) {
    // The encoder state machine guarantees passes are ended before finish; fail closed anyway.
    pass.get().end();
    pass.reset();
    return GpuError{GpuErrorType::InvalidState, "submitted command stream left a render pass open"};
  }

  ScopedWgpuHandle<wgpu::CommandBuffer> commandBuffer(encoder.get().finish());
  if (!commandBuffer) {
    return GpuError{GpuErrorType::InvalidState, "wgpu command buffer finish failed"};
  }
  geodeDevice_.queue().submit(1, &commandBuffer.get());
  geodeDevice_.countSubmit();

  // Advance completedSerial when the queue drains. Callback-mode handling (wgpu-native vs
  // emdawnwebgpu) is centralized in notifyWhenSubmittedWorkDone; waitForSerial's poll loop
  // drives delivery, mirroring GeodeDevice's WaitForSubmittedWork.
  struct WorkDoneState {
    std::shared_ptr<CompletionState> completion;  //!< Shared completion counter.
    uint64_t serial = 0;                          //!< Serial this callback completes.

    /// Monotonic max: callbacks may complete out of order across submissions.
    void onWorkDone() {
      uint64_t previous = completion->completedSerial.load(std::memory_order_relaxed);
      while (previous < serial &&
             !completion->completedSerial.compare_exchange_weak(
                 previous, serial, std::memory_order_release, std::memory_order_relaxed)) {}
    }
  };
  auto state = std::make_shared<WorkDoneState>();
  state->completion = completionState_;
  state->serial = submissionSerial;
  notifyWhenSubmittedWorkDone(geodeDevice_.queue(), state);

  return OkStatus();
}

}  // namespace donner::geode
