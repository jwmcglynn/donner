#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/svg/renderer/geode/GeodeCallbackState.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuWait.h"

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
  if (gpu::HasAllFlags(usage, gpu::TextureUsage::StorageBinding)) {
    result |= wgpu::TextureUsage::StorageBinding;
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
    case gpu::BlendFactor::OneMinusDstAlpha: return wgpu::BlendFactor::OneMinusDstAlpha;
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

/// Fills the resource-kind union of a wgpu bind group layout entry from \p layoutEntry.
void ApplyBindingType(wgpu::BindGroupLayoutEntry& entry,
                      const gpu::BindGroupLayoutEntry& layoutEntry) {
  switch (layoutEntry.type) {
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
    case gpu::BindingType::WriteOnlyStorageTexture2d:
      entry.storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
      entry.storageTexture.format = ToWgpuTextureFormat(layoutEntry.storageTextureFormat);
      entry.storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;
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

gpu::Status GeodeWgpuAdapterDevice::destroyTextureBacking(gpu::Texture&& texture) {
  // Validate before touching the slot so a stale or foreign handle cannot destroy whatever
  // occupies that slot now.
  if (gpu::Status status = validateTextureHandleForBackend(texture); status.hasError()) {
    return status;
  }
  if (texture.slotIndex() < slotTextures_.size()) {
    slotTextures_[texture.slotIndex()].ownedTexture.destroyBackingAndReset();
  }
  return destroyTexture(std::move(texture));
}

gpu::Status GeodeWgpuAdapterDevice::destroyBufferBacking(gpu::Buffer&& buffer) {
  // Validate before touching the slot so a stale or foreign handle cannot destroy whatever
  // occupies that slot now.
  if (gpu::Status status = validateBufferHandleForBackend(buffer); status.hasError()) {
    return status;
  }
  if (buffer.slotIndex() < slotBuffers_.size()) {
    slotBuffers_[buffer.slotIndex()].destroyBackingAndReset();
  }
  return destroyBuffer(std::move(buffer));
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

gpu::TextureUsage GpuTextureUsageFromWgpu(wgpu::TextureUsage usage) {
  const WGPUTextureUsage raw = static_cast<WGPUTextureUsage>(usage);
  gpu::TextureUsage result = gpu::TextureUsage::None;
  if ((raw & WGPUTextureUsage_RenderAttachment) != 0) {
    result = result | gpu::TextureUsage::RenderAttachment;
  }
  if ((raw & WGPUTextureUsage_TextureBinding) != 0) {
    result = result | gpu::TextureUsage::Sampled;
  }
  if ((raw & WGPUTextureUsage_CopySrc) != 0) {
    result = result | gpu::TextureUsage::CopySrc;
  }
  if ((raw & WGPUTextureUsage_CopyDst) != 0) {
    result = result | gpu::TextureUsage::CopyDst;
  }
  if ((raw & WGPUTextureUsage_StorageBinding) != 0) {
    result = result | gpu::TextureUsage::StorageBinding;
  }
  return result;
}

namespace {

/// Maps what the backend reported about a surface onto the runtime's status.
///
/// A suboptimal surface is reported as out of date on purpose: both mean the configuration no
/// longer matches the window, and both recover the same way.
///
/// @param status Backend status.
gpu::SurfaceStatus GpuSurfaceStatusFromWgpu(wgpu::SurfaceGetCurrentTextureStatus status) {
  switch (status) {
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal: return gpu::SurfaceStatus::Success;
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
    case wgpu::SurfaceGetCurrentTextureStatus::Outdated: return gpu::SurfaceStatus::Outdated;
    case wgpu::SurfaceGetCurrentTextureStatus::Timeout: return gpu::SurfaceStatus::Timeout;
    case wgpu::SurfaceGetCurrentTextureStatus::DeviceLost: return gpu::SurfaceStatus::DeviceLost;
    default: break;
  }
  // Everything else means the surface itself can no longer serve frames, which recovers only by
  // building a new one.
  return gpu::SurfaceStatus::Lost;
}

/// Maps the runtime's frame pacing onto the backend's. @param mode Runtime pacing.
wgpu::PresentMode WgpuPresentModeFrom(gpu::PresentMode mode) {
  switch (mode) {
    case gpu::PresentMode::Immediate: return wgpu::PresentMode::Immediate;
    case gpu::PresentMode::Mailbox: return wgpu::PresentMode::Mailbox;
    case gpu::PresentMode::Fifo: break;
  }
  return wgpu::PresentMode::Fifo;
}

/// Maps the backend's frame pacing onto the runtime's. @param mode Backend pacing.
gpu::PresentMode GpuPresentModeFrom(wgpu::PresentMode mode) {
  switch (static_cast<WGPUPresentMode>(mode)) {
    case WGPUPresentMode_Immediate: return gpu::PresentMode::Immediate;
    case WGPUPresentMode_Mailbox: return gpu::PresentMode::Mailbox;
    default: break;
  }
  return gpu::PresentMode::Fifo;
}

/// Maps the runtime's alpha compositing onto the backend's. @param mode Runtime mode.
wgpu::CompositeAlphaMode WgpuAlphaModeFrom(gpu::SurfaceAlphaMode mode) {
  switch (mode) {
    case gpu::SurfaceAlphaMode::Premultiplied: return wgpu::CompositeAlphaMode::Premultiplied;
    case gpu::SurfaceAlphaMode::Inherit: return wgpu::CompositeAlphaMode::Inherit;
    case gpu::SurfaceAlphaMode::Opaque: break;
  }
  return wgpu::CompositeAlphaMode::Opaque;
}

/// Maps the backend's alpha compositing onto the runtime's. @param mode Backend mode.
gpu::SurfaceAlphaMode GpuAlphaModeFrom(wgpu::CompositeAlphaMode mode) {
  switch (static_cast<WGPUCompositeAlphaMode>(mode)) {
    case WGPUCompositeAlphaMode_Premultiplied: return gpu::SurfaceAlphaMode::Premultiplied;
    case WGPUCompositeAlphaMode_Inherit: return gpu::SurfaceAlphaMode::Inherit;
    default: break;
  }
  return gpu::SurfaceAlphaMode::Opaque;
}

}  // namespace

gpu::Status GeodeWgpuAdapterDevice::onCreateSurface(uint32_t slotIndex,
                                                    const gpu::SurfaceDescriptor& descriptor) {
  if (descriptor.native.kind != gpu::NativeSurfaceKind::MetalLayer) {
    return GpuError{GpuErrorType::Unsupported,
                    "this adapter presents to a Metal layer only; the other platform surfaces "
                    "are still created by the embedder"};
  }

  wgpu::SurfaceSourceMetalLayer source(wgpu::Default);
  source.layer = descriptor.native.display;

  wgpu::SurfaceDescriptor surfaceDescriptor(wgpu::Default);
  surfaceDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  surfaceDescriptor.nextInChain = &source.chain;

  wgpu::Surface surface = geodeDevice_.instance().createSurface(surfaceDescriptor);
  if (!surface) {
    return GpuError{GpuErrorType::Unsupported, "the backend could not create a surface"};
  }

  SetSlot(slotSurfaces_, slotIndex,
          SurfaceSlot{ScopedWgpuHandle<wgpu::Surface>(surface), wgpu::Texture(), 0, false});
  return OkStatus();
}

gpu::Result<gpu::SurfaceCapabilities> GeodeWgpuAdapterDevice::onSurfaceCapabilities(
    uint32_t slotIndex) const {
  if (slotIndex >= slotSurfaces_.size() || !slotSurfaces_[slotIndex].surface) {
    return GpuError{GpuErrorType::InvalidState, "the surface is no longer live"};
  }

  wgpu::SurfaceCapabilities backendCapabilities = {};
  slotSurfaces_[slotIndex].surface.get().getCapabilities(geodeDevice_.adapter(),
                                                         &backendCapabilities);

  gpu::SurfaceCapabilities capabilities;
  for (size_t i = 0; i < backendCapabilities.formatCount; ++i) {
    // Formats outside the runtime's set are dropped rather than mapped to a stand-in: a caller
    // choosing among them must only ever see ones this runtime can actually render.
    const auto format = static_cast<WGPUTextureFormat>(backendCapabilities.formats[i]);
    if (format == WGPUTextureFormat_RGBA8Unorm || format == WGPUTextureFormat_BGRA8Unorm ||
        format == WGPUTextureFormat_R8Unorm) {
      capabilities.formats.push_back(GpuTextureFormatFromWgpu(backendCapabilities.formats[i]));
    }
  }
  capabilities.usages = GpuTextureUsageFromWgpu(wgpu::TextureUsage{backendCapabilities.usages});
  for (size_t i = 0; i < backendCapabilities.presentModeCount; ++i) {
    capabilities.presentModes.push_back(GpuPresentModeFrom(backendCapabilities.presentModes[i]));
  }
  for (size_t i = 0; i < backendCapabilities.alphaModeCount; ++i) {
    capabilities.alphaModes.push_back(GpuAlphaModeFrom(backendCapabilities.alphaModes[i]));
  }
  return capabilities;
}

gpu::Status GeodeWgpuAdapterDevice::onConfigureSurface(
    uint32_t slotIndex, const gpu::SurfaceConfiguration& configuration) {
  if (slotIndex >= slotSurfaces_.size() || !slotSurfaces_[slotIndex].surface) {
    return GpuError{GpuErrorType::InvalidState, "the surface is no longer live"};
  }

  wgpu::SurfaceConfiguration backendConfiguration(wgpu::Default);
  backendConfiguration.device = geodeDevice_.device();
  backendConfiguration.format = ToWgpuTextureFormat(configuration.format);
  backendConfiguration.usage = ToWgpuTextureUsage(configuration.usage);
  backendConfiguration.width = configuration.size.width;
  backendConfiguration.height = configuration.size.height;
  backendConfiguration.presentMode = WgpuPresentModeFrom(configuration.presentMode);
  backendConfiguration.alphaMode = WgpuAlphaModeFrom(configuration.alphaMode);
  slotSurfaces_[slotIndex].surface.get().configure(backendConfiguration);
  return OkStatus();
}

gpu::Result<gpu::SurfaceStatus> GeodeWgpuAdapterDevice::onAcquireCurrentTexture(
    uint32_t slotIndex, uint32_t textureSlotIndex) {
  if (slotIndex >= slotSurfaces_.size() || !slotSurfaces_[slotIndex].surface) {
    return GpuError{GpuErrorType::InvalidState, "the surface is no longer live"};
  }

  wgpu::SurfaceTexture surfaceTexture = {};
  slotSurfaces_[slotIndex].surface.get().getCurrentTexture(&surfaceTexture);
  const gpu::SurfaceStatus status = GpuSurfaceStatusFromWgpu(surfaceTexture.status);
  if (status == gpu::SurfaceStatus::Lost || status == gpu::SurfaceStatus::DeviceLost ||
      status == gpu::SurfaceStatus::Timeout || !surfaceTexture.texture) {
    return status;
  }

  SurfaceSlot& slot = slotSurfaces_[slotIndex];
  slot.acquired = wgpu::Texture(surfaceTexture.texture);
  slot.acquiredTextureSlot = textureSlotIndex;
  slot.hasAcquired = true;
  // Borrowed: the surface owns the frame's texture, so the runtime's slot names it without
  // taking a reference that would outlive the frame.
  SetSlot(slotTextures_, textureSlotIndex,
          TextureSlot{ScopedWgpuHandle<wgpu::Texture>(), slot.acquired});
  return status;
}

gpu::Result<gpu::SurfaceStatus> GeodeWgpuAdapterDevice::onPresentSurface(uint32_t slotIndex) {
  if (slotIndex >= slotSurfaces_.size() || !slotSurfaces_[slotIndex].surface) {
    return GpuError{GpuErrorType::InvalidState, "the surface is no longer live"};
  }

  SurfaceSlot& slot = slotSurfaces_[slotIndex];
  slot.surface.get().present();
  SetSlot(slotTextures_, slot.acquiredTextureSlot, TextureSlot{});
  slot.acquired = wgpu::Texture();
  slot.hasAcquired = false;
  return geodeDevice_.isDeviceLost() ? gpu::SurfaceStatus::DeviceLost : gpu::SurfaceStatus::Success;
}

void GeodeWgpuAdapterDevice::onAbandonCurrentTexture(uint32_t slotIndex) {
  if (slotIndex >= slotSurfaces_.size() || !slotSurfaces_[slotIndex].hasAcquired) {
    return;
  }
  SurfaceSlot& slot = slotSurfaces_[slotIndex];
  // Clear the runtime slot only while it still names this frame. A caller that disposed of the
  // frame's handle frees that slot, and a texture created before the next acquire can be given
  // the same index; wiping it then would take out a texture this frame never owned.
  if (slot.acquiredTextureSlot < slotTextures_.size() &&
      slotTextures_[slot.acquiredTextureSlot].texture == slot.acquired) {
    SetSlot(slotTextures_, slot.acquiredTextureSlot, TextureSlot{});
  }
  slot.acquired = wgpu::Texture();
  slot.hasAcquired = false;
}

gpu::Status GeodeWgpuAdapterDevice::onMapBufferAsync(uint32_t mappingSlotIndex,
                                                     uint32_t bufferSlotIndex, gpu::MapMode mode,
                                                     uint64_t offsetBytes, uint64_t byteCount) {
  if (mode != gpu::MapMode::Read) {
    return GpuError{GpuErrorType::Unsupported, "this adapter maps buffers for reading only"};
  }
  wgpu::Buffer buffer = GetHandle(slotBuffers_, bufferSlotIndex);
  if (!buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("buffer slot {} has no wgpu buffer", bufferSlotIndex)};
  }
  if (geodeDevice_.isDeviceLost()) {
    // A lost device never delivers the completion, so refuse the map rather than hand back a
    // handle whose wait can only ever run out its budget.
    return GpuError{GpuErrorType::InvalidState, "the device is lost, so buffers cannot be mapped"};
  }

  MappingSlot slot;
  slot.completion = new MappingSlot::Completion();
  // The completion keeps a reference of its own, so a map abandoned in flight still has a buffer
  // to unmap once it finishes.
  slot.completion->buffer = buffer;
  slot.completion->buffer.addRef();
  slot.buffer = buffer;
  slot.offsetBytes = offsetBytes;
  slot.byteCount = byteCount;

  wgpu::BufferMapCallbackInfo callbackInfo{wgpu::Default};
  callbackInfo.callback = [](WGPUMapAsyncStatus status, WGPUStringView /*message*/, void* userdata1,
                             void* /*userdata2*/) {
    auto* completion = static_cast<MappingSlot::Completion*>(userdata1);
    completion->ok.store(status == WGPUMapAsyncStatus_Success, std::memory_order_relaxed);
    completion->done.store(true, std::memory_order_release);
    // The mapping may already be gone, in which case nothing else can give the buffer back.
    completion->unmapIfAbandoned();
    completion->release();
  };
  callbackInfo.userdata1 = slot.completion;
  callbackInfo.userdata2 = nullptr;
  // Spontaneous delivery is what lets a wait slice observe the completion from inside the
  // backend call it makes, rather than only at an explicit processing point.
  callbackInfo.mode = wgpu::CallbackMode::AllowSpontaneous;
  slot.mapFuture = buffer.mapAsync(wgpu::MapMode::Read, offsetBytes, byteCount, callbackInfo);

  SetSlot(slotMappings_, mappingSlotIndex, std::move(slot));
  return OkStatus();
}

gpu::MapSliceState GeodeWgpuAdapterDevice::sliceStateOf(
    const MappingSlot::Completion& completion) const {
  // Total over the completion state on purpose. A wait that returns without the map having
  // completed - a timed wait that expired, a poll that found nothing - has learned nothing about
  // whether the map will succeed, and reading only the success flag there reports a map that is
  // merely not finished yet as a failed one. Every caller reads the state through here so that
  // mistake cannot be made at one site and not another.
  if (completion.done.load(std::memory_order_acquire)) {
    return completion.ok.load(std::memory_order_relaxed) ? gpu::MapSliceState::Ready
                                                         : gpu::MapSliceState::Failed;
  }
  return geodeDevice_.isDeviceLost() ? gpu::MapSliceState::DeviceLost : gpu::MapSliceState::Pending;
}

bool GeodeWgpuAdapterDevice::waitOnMapFutureSlice(uint32_t mappingSlotIndex,
                                                  std::chrono::microseconds slice) {
  if (simulateEventWaitForTest_) {
    // Stands in for a browser timed wait that expired without the map completing. The real arm
    // is compiled out everywhere the test suites run, so without this seam its contract - that a
    // slice which waited and learned nothing reports "not finished", never "failed" - would be
    // checked only by the browser lane.
    (void)mappingSlotIndex;
    (void)slice;
    return true;
  }
#ifdef __EMSCRIPTEN__
  // The browser instance is created asking for TimedWaitAny (see GeodeDevice::CreateHeadless)
  // exactly so this wait exists: on a worker thread the map completion is a browser-side event,
  // and a poll-and-yield loop only observes it when a yield happens to line up with the
  // browser's delivery - measured at 265 five-millisecond yields, 1.85 seconds, for a first
  // snapshot readback. Waiting on the future returns the moment the map resolves, while a
  // timeout still returns within the slice so the caller's cancellation stays responsive.
  //
  // The latch is process-wide because the failure it guards against is a property of this
  // thread's relationship to the instance, not of one mapping: once a status other than
  // Success or TimedOut says this future cannot be time-waited here, every later wait polls.
  static std::atomic<bool> instanceWaitUsable{true};
  MappingSlot& slot = slotMappings_[mappingSlotIndex];
  if (!geodeDevice_.instance() || !slot.mapFuture.id ||
      !instanceWaitUsable.load(std::memory_order_relaxed)) {
    return false;
  }

  wgpu::FutureWaitInfo waitInfo{};
  waitInfo.future = slot.mapFuture;
  // The browser's timed wait keeps its own cadence rather than the caller's slice. Every wait
  // here is an asyncify suspend and rewind of the whole call stack, so slicing at the native
  // poll cadence would spend fifty of those per millisecond to learn the same thing; five
  // milliseconds is what this path was tuned to, and a cancellation is still observed within it
  // because the caller re-checks between slices.
  constexpr std::chrono::microseconds kBrowserTimedWaitSlice{5000};
  (void)slice;
  const auto sliceNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(kBrowserTimedWaitSlice).count());
  const wgpu::WaitStatus waitStatus = geodeDevice_.instance().waitAny(1, &waitInfo, sliceNs);
  if (waitStatus != wgpu::WaitStatus::Success && waitStatus != wgpu::WaitStatus::TimedOut) {
    instanceWaitUsable.store(false, std::memory_order_relaxed);
    return false;
  }
  slot.usedTimedWaitAny = true;
  return true;
#else
  (void)mappingSlotIndex;
  (void)slice;
  return false;
#endif
}

gpu::MapSliceState GeodeWgpuAdapterDevice::onWaitMappingSlice(uint32_t mappingSlotIndex,
                                                              double sliceSeconds) {
  if (mappingSlotIndex >= slotMappings_.size() ||
      slotMappings_[mappingSlotIndex].completion == nullptr) {
    return gpu::MapSliceState::Failed;
  }
  MappingSlot::Completion& completion = *slotMappings_[mappingSlotIndex].completion;
  if (completion.done.load(std::memory_order_acquire) || geodeDevice_.isDeviceLost()) {
    return sliceStateOf(completion);
  }

  // Wait out the slice the caller allowed rather than returning the moment one poll finds
  // nothing: the waiter's budget is wall time, so a slice that returns immediately turns a map
  // that is merely not ready yet into a burst of fast calls against that budget.
  //
  // Microseconds, not milliseconds: the readback path slices below a millisecond, and rounding
  // a 100 us slice up to 1 ms would coarsen the cadence that path is tuned to.
  const auto slice = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::duration<double>(sliceSeconds));

  if (waitOnMapFutureSlice(mappingSlotIndex, slice)) {
    return sliceStateOf(completion);
  }

  (void)BoundedGpuWait(
      [&] {
        (void)geodeDevice_.pollSuspending(false);
        return completion.done.load(std::memory_order_acquire) || geodeDevice_.isDeviceLost();
      },
      std::max(slice, std::chrono::microseconds(1)));

  return sliceStateOf(completion);
}

bool GeodeWgpuAdapterDevice::mappingUsedTimedWaitAny(const gpu::BufferMapping& mapping) const {
  if (validateBufferMappingHandleForBackend(mapping).hasError() ||
      mapping.slotIndex() >= slotMappings_.size()) {
    return false;
  }
  return slotMappings_[mapping.slotIndex()].usedTimedWaitAny;
}

gpu::Result<std::span<const uint8_t>> GeodeWgpuAdapterDevice::onMappedBytes(
    uint32_t mappingSlotIndex) const {
  if (mappingSlotIndex >= slotMappings_.size() ||
      slotMappings_[mappingSlotIndex].completion == nullptr) {
    return GpuError{GpuErrorType::InvalidState, "the mapping is no longer live"};
  }
  const MappingSlot& slot = slotMappings_[mappingSlotIndex];
  if (!slot.completion->done.load(std::memory_order_acquire) ||
      !slot.completion->ok.load(std::memory_order_relaxed)) {
    return GpuError{GpuErrorType::InvalidState, "the mapping has not completed"};
  }
  const void* mapped = slot.buffer.getConstMappedRange(slot.offsetBytes, slot.byteCount);
  if (mapped == nullptr) {
    return GpuError{GpuErrorType::InvalidState, "the backend returned no mapped range"};
  }
  return std::span<const uint8_t>(static_cast<const uint8_t*>(mapped),
                                  static_cast<size_t>(slot.byteCount));
}

void GeodeWgpuAdapterDevice::onUnmapBuffer(uint32_t mappingSlotIndex) {
  if (mappingSlotIndex >= slotMappings_.size() ||
      slotMappings_[mappingSlotIndex].completion == nullptr) {
    return;
  }
  MappingSlot& slot = slotMappings_[mappingSlotIndex];
  // From here the completion is the only thing that can give the buffer back, whether the map
  // has already finished or is still in flight.
  slot.completion->abandoned.store(true, std::memory_order_release);
  slot.completion->unmapIfAbandoned();
  // The pending callback holds the other reference and releases it when it runs, so a map that
  // is still in flight when the mapping is dropped cannot leave the state behind either way.
  slot.completion->release();
  slot.completion = nullptr;
  slot.buffer = wgpu::Buffer();
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
    ApplyBindingType(entries[i], entry);
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

gpu::Status GeodeWgpuAdapterDevice::onCreateComputePipeline(
    uint32_t slotIndex, const gpu::ComputePipelineDescriptor& descriptor) {
  wgpu::PipelineLayout layout = GetHandle(slotPipelineLayouts_, descriptor.layout.slotIndex());
  wgpu::ShaderModule module = GetHandle(slotShaderModules_, descriptor.compute.module.slotIndex());
  if (!layout || !module) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("compute pipeline '{}' references a layout or shader module with "
                                "no wgpu object",
                                std::string_view(descriptor.label))};
  }

  const std::string_view entryPoint(descriptor.compute.entryPoint);
  wgpu::ComputePipelineDescriptor pipelineDescriptor = {};
  pipelineDescriptor.label = wgpuLabel(std::string_view(descriptor.label));
  pipelineDescriptor.layout = layout;
  pipelineDescriptor.compute.module = module;
  pipelineDescriptor.compute.entryPoint = wgpuLabel(entryPoint);

  wgpu::ComputePipeline pipeline = geodeDevice_.device().createComputePipeline(pipelineDescriptor);
  if (!pipeline) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("wgpu compute pipeline creation failed for '{}'",
                                std::string_view(descriptor.label))};
  }

  SetSlot(slotComputePipelines_, slotIndex, ScopedWgpuHandle<wgpu::ComputePipeline>(pipeline));
  return OkStatus();
}

/// Clears the slot of one non-pipeline resource kind, returning false when \p resourceName names
/// no kind this table set tracks.
/// @param resourceName Resource type name from the base class.
/// @param slotIndex Slot to clear.
bool GeodeWgpuAdapterDevice::clearResourceSlot(std::string_view resourceName, uint32_t slotIndex) {
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
  } else {
    return false;
  }
  return true;
}

/// Clears the slot of one pipeline-family resource kind, returning false when \p resourceName
/// names no kind this table set tracks.
/// @param resourceName Resource type name from the base class.
/// @param slotIndex Slot to clear.
bool GeodeWgpuAdapterDevice::clearPipelineSlot(std::string_view resourceName, uint32_t slotIndex) {
  if (resourceName == "pipelineLayout") {
    SetSlot(slotPipelineLayouts_, slotIndex, ScopedWgpuHandle<wgpu::PipelineLayout>());
  } else if (resourceName == "shaderModule") {
    SetSlot(slotShaderModules_, slotIndex, ScopedWgpuHandle<wgpu::ShaderModule>());
  } else if (resourceName == "renderPipeline") {
    SetSlot(slotRenderPipelines_, slotIndex, ScopedWgpuHandle<wgpu::RenderPipeline>());
  } else if (resourceName == "computePipeline") {
    SetSlot(slotComputePipelines_, slotIndex, ScopedWgpuHandle<wgpu::ComputePipeline>());
  } else {
    return false;
  }
  return true;
}

void GeodeWgpuAdapterDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  // Clearing a slot releases the owned wgpu reference (imported external textures carry no
  // owned reference, so their backing object is untouched).
  if (clearResourceSlot(resourceName, slotIndex) || clearPipelineSlot(resourceName, slotIndex)) {
    return;
  }
  // A resource kind this adapter does not track would leak its wgpu object silently. Loud in
  // debug so a new kind added to the runtime is wired up here; release-safe no-op (the base
  // class owns the bookkeeping either way).
  assert(false && "GeodeWgpuAdapterDevice::onDestroyResource: unknown resource kind");
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

gpu::Status GeodeWgpuAdapterDevice::encodeBeginRenderPass(
    EncodingState& state, const gpu::BeginRenderPassCommand& beginPass) {
  const auto& attachments = beginPass.descriptor.colorAttachments;
  std::vector<wgpu::RenderPassColorAttachment> colorAttachments(attachments.size());
  for (size_t i = 0; i < attachments.size(); ++i) {
    const gpu::RenderPassColorAttachment& attachment = attachments[i];
    wgpu::TextureView view = GetHandle(slotTextureViews_, attachment.view.slotIndex());
    if (!view) {
      return GpuError{
          GpuErrorType::InvalidState,
          std::format("render pass attachment {} does not resolve to a wgpu texture view", i)};
    }
    colorAttachments[i].view = view;
    colorAttachments[i].loadOp = ToWgpuLoadOp(attachment.loadOp);
    colorAttachments[i].storeOp = ToWgpuStoreOp(attachment.storeOp);
    colorAttachments[i].clearValue = {attachment.clearColor[0], attachment.clearColor[1],
                                      attachment.clearColor[2], attachment.clearColor[3]};
    // Dawn (browser WebGPU) rejects depthSlice=0 on non-3D views; wgpu-native is lenient. Set
    // the UNDEFINED sentinel for cross-backend compatibility (see GeoEncoder).
    colorAttachments[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  }

  wgpu::RenderPassDescriptor passDescriptor = {};
  passDescriptor.label = wgpuLabel(std::string_view(beginPass.descriptor.label));
  passDescriptor.colorAttachmentCount = colorAttachments.size();
  passDescriptor.colorAttachments = colorAttachments.data();
  state.pass.reset(state.encoder.beginRenderPass(passDescriptor));
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "wgpu render pass creation failed"};
  }
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetPipeline(EncodingState& state,
                                                      const gpu::SetPipelineCommand& setPipeline) {
  wgpu::RenderPipeline pipeline = GetHandle(slotRenderPipelines_, setPipeline.pipelineId.slotIndex);
  if (!state.pass || !pipeline) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  state.pass.get().setPipeline(pipeline);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetBindGroup(
    EncodingState& state, const gpu::SetBindGroupCommand& setBindGroup) {
  wgpu::BindGroup group = GetHandle(slotBindGroups_, setBindGroup.bindGroupId.slotIndex);
  if ((!state.pass && !state.computePass) || !group) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setBindGroup: bind group slot {} is not encodable",
                                setBindGroup.bindGroupId.slotIndex)};
  }
  if (state.pass) {
    state.pass.get().setBindGroup(setBindGroup.index, group, 0, nullptr);
  } else {
    state.computePass.get().setBindGroup(setBindGroup.index, group, 0, nullptr);
  }
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetVertexBuffer(
    EncodingState& state, const gpu::SetVertexBufferCommand& setVertexBuffer) {
  wgpu::Buffer buffer = GetHandle(slotBuffers_, setVertexBuffer.bufferId.slotIndex);
  if (!state.pass || !buffer) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setVertexBuffer: buffer slot {} is not encodable",
                                setVertexBuffer.bufferId.slotIndex)};
  }
  state.pass.get().setVertexBuffer(setVertexBuffer.slot, buffer, setVertexBuffer.offsetBytes,
                                   WGPU_WHOLE_SIZE);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetScissorRect(
    EncodingState& state, const gpu::SetScissorRectCommand& setScissor) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "setScissorRect outside a render pass"};
  }
  state.pass.get().setScissorRect(setScissor.x, setScissor.y, setScissor.width, setScissor.height);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetViewport(EncodingState& state,
                                                      const gpu::SetViewportCommand& setViewport) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "setViewport outside a render pass"};
  }
  state.pass.get().setViewport(setViewport.x, setViewport.y, setViewport.width, setViewport.height,
                               setViewport.minDepth, setViewport.maxDepth);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeDraw(EncodingState& state, const gpu::DrawCommand& draw) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "draw outside a render pass"};
  }
  state.pass.get().draw(draw.vertexCount, draw.instanceCount, draw.firstVertex, draw.firstInstance);
  geodeDevice_.countDraw();
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeEndRenderPass(EncodingState& state) {
  if (!state.pass) {
    return GpuError{GpuErrorType::InvalidState, "endRenderPass without an active render pass"};
  }
  state.pass.get().end();
  state.pass.reset();
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeBeginComputePass(
    EncodingState& state, const gpu::BeginComputePassCommand& beginPass) {
  wgpu::ComputePassDescriptor passDescriptor = {};
  passDescriptor.label = wgpuLabel(std::string_view(beginPass.descriptor.label));
  state.computePass.reset(state.encoder.beginComputePass(passDescriptor));
  if (!state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "wgpu compute pass creation failed"};
  }
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeSetComputePipeline(
    EncodingState& state, const gpu::SetComputePipelineCommand& setPipeline) {
  wgpu::ComputePipeline pipeline =
      GetHandle(slotComputePipelines_, setPipeline.pipelineId.slotIndex);
  if (!state.computePass || !pipeline) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: compute pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  state.computePass.get().setPipeline(pipeline);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeDispatchWorkgroups(
    EncodingState& state, const gpu::DispatchWorkgroupsCommand& dispatch) {
  if (!state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "dispatchWorkgroups outside a compute pass"};
  }
  state.computePass.get().dispatchWorkgroups(dispatch.workgroupCountX, dispatch.workgroupCountY,
                                             dispatch.workgroupCountZ);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeEndComputePass(EncodingState& state) {
  if (!state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "endComputePass without an active compute pass"};
  }
  state.computePass.get().end();
  state.computePass.reset();
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeCopyTextureToBuffer(
    EncodingState& state, const gpu::CopyTextureToBufferCommand& copy) {
  if (state.pass || state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToBuffer inside a pass"};
  }
  wgpu::Texture texture = copy.textureId.slotIndex < slotTextures_.size()
                              ? slotTextures_[copy.textureId.slotIndex].texture
                              : wgpu::Texture();
  wgpu::Buffer buffer = GetHandle(slotBuffers_, copy.bufferId.slotIndex);
  if (!texture || !buffer) {
    return GpuError{GpuErrorType::InvalidState,
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
  state.encoder.copyTextureToBuffer(source, destination, extent);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeCopyTextureToTexture(
    EncodingState& state, const gpu::CopyTextureToTextureCommand& textureCopy) {
  if (state.pass || state.computePass) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToTexture inside a pass"};
  }
  wgpu::Texture sourceTexture = textureCopy.textureSrcId.slotIndex < slotTextures_.size()
                                    ? slotTextures_[textureCopy.textureSrcId.slotIndex].texture
                                    : wgpu::Texture();
  wgpu::Texture destinationTexture = textureCopy.textureDstId.slotIndex < slotTextures_.size()
                                         ? slotTextures_[textureCopy.textureDstId.slotIndex].texture
                                         : wgpu::Texture();
  if (!sourceTexture || !destinationTexture) {
    return GpuError{GpuErrorType::InvalidState,
                    "copyTextureToTexture: source or destination texture is missing"};
  }
  wgpu::TexelCopyTextureInfo source = {};
  source.texture = sourceTexture;
  source.origin = {textureCopy.sourceOrigin.x, textureCopy.sourceOrigin.y, 0u};
  wgpu::TexelCopyTextureInfo destination = {};
  destination.texture = destinationTexture;
  destination.origin = {textureCopy.destinationOrigin.x, textureCopy.destinationOrigin.y, 0u};
  const wgpu::Extent3D extent = {textureCopy.copySize.width, textureCopy.copySize.height, 1u};
  state.encoder.copyTextureToTexture(source, destination, extent);
  return OkStatus();
}

gpu::Status GeodeWgpuAdapterDevice::encodeCommand(EncodingState& state,
                                                  const gpu::Command& command) {
  // Exhaustive dispatch: every `gpu::Command` alternative has a handler, so adding a new
  // command to the variant without wiring it here is a compile error instead of a validated,
  // submitted, "completed", never-executed no-op.
  return std::visit(
      Overloaded{
          [&](const gpu::BeginRenderPassCommand& beginPass) -> gpu::Status {
            return encodeBeginRenderPass(state, beginPass);
          },
          [&](const gpu::SetPipelineCommand& setPipeline) -> gpu::Status {
            return encodeSetPipeline(state, setPipeline);
          },
          [&](const gpu::SetBindGroupCommand& setBindGroup) -> gpu::Status {
            return encodeSetBindGroup(state, setBindGroup);
          },
          [&](const gpu::SetVertexBufferCommand& setVertexBuffer) -> gpu::Status {
            return encodeSetVertexBuffer(state, setVertexBuffer);
          },
          [&](const gpu::SetScissorRectCommand& setScissor) -> gpu::Status {
            return encodeSetScissorRect(state, setScissor);
          },
          [&](const gpu::SetViewportCommand& setViewport) -> gpu::Status {
            return encodeSetViewport(state, setViewport);
          },
          [&](const gpu::DrawCommand& draw) -> gpu::Status { return encodeDraw(state, draw); },
          [&](const gpu::EndRenderPassCommand&) -> gpu::Status {
            return encodeEndRenderPass(state);
          },
          [&](const gpu::CopyTextureToBufferCommand& copy) -> gpu::Status {
            return encodeCopyTextureToBuffer(state, copy);
          },
          [&](const gpu::CopyTextureToTextureCommand& textureCopy) -> gpu::Status {
            return encodeCopyTextureToTexture(state, textureCopy);
          },
          [&](const gpu::BeginComputePassCommand& beginPass) -> gpu::Status {
            return encodeBeginComputePass(state, beginPass);
          },
          [&](const gpu::SetComputePipelineCommand& setPipeline) -> gpu::Status {
            return encodeSetComputePipeline(state, setPipeline);
          },
          [&](const gpu::DispatchWorkgroupsCommand& dispatch) -> gpu::Status {
            return encodeDispatchWorkgroups(state, dispatch);
          },
          [&](const gpu::EndComputePassCommand&) -> gpu::Status {
            return encodeEndComputePass(state);
          },
      },
      command);
}

void GeodeWgpuAdapterDevice::setHostCommandEncoder(wgpu::CommandEncoder encoder) {
  hostCommandEncoder_ = std::move(encoder);
}

bool GeodeWgpuAdapterDevice::hostCommandEncoderIs(const wgpu::CommandEncoder& encoder) const {
  return static_cast<WGPUCommandEncoder>(hostCommandEncoder_) ==
         static_cast<WGPUCommandEncoder>(encoder);
}

void GeodeWgpuAdapterDevice::clearHostCommandEncoder() {
  hostCommandEncoder_ = wgpu::CommandEncoder();
}

bool GeodeWgpuAdapterDevice::hasHostCommandEncoder() const {
  return static_cast<bool>(hostCommandEncoder_);
}

void GeodeWgpuAdapterDevice::notifyHostSubmitted() {
  if (hostPendingSerial_ == 0) {
    return;
  }
  // Anything a standalone submit queued while this frame was pending is older than the frame's
  // own submit on the same queue, so it has certainly finished by the time this one does. Its
  // completion was capped below the frame's unqueued serials and reported less than its own
  // serial - possibly nothing at all - so the flush is what finally covers it.
  const uint64_t serial = std::max(hostPendingSerial_, standaloneWhilePendingSerial_);
  hostPendingSerial_ = 0;
  hostFirstPendingSerial_ = 0;
  standaloneWhilePendingSerial_ = 0;
  advanceCompletedSerialWhenQueueDrains(serial);
}

void GeodeWgpuAdapterDevice::advanceCompletedSerialWhenQueueDrains(uint64_t serial) {
  // A serial may be reported complete only once it has actually reached the queue, and while a
  // host encoder is installed the serials replayed into it are recorded but not submitted. A
  // standalone submit skips that encoder, so it takes a HIGHER serial than those and completes
  // first; reporting its own serial would declare the unqueued ones complete along with it.
  //
  // That is not a cosmetic lie. `completedSerial` is what the deferred-destroy sweep and every
  // wait read, so the open frame's still-recording resources would be retired underneath it and
  // their slots handed to something else, invalidating handles the frame is still using, and
  // waits on those serials - including the destructor's - would return early.
  //
  // So the advance is capped below the oldest serial still waiting on the host's submit. The cap
  // is computed here rather than in the callback because the callback may land after the frame
  // has flushed, and a merely conservative cap costs nothing: the flush advances the serial past
  // all of this on its own, and the standalone submit's own resources then retire at that
  // frame boundary like any others. The readback that motivates the standalone path waits on its
  // buffer's map signal rather than on a serial, so capping does not delay it.
  const uint64_t cappedSerial =
      hostFirstPendingSerial_ == 0 ? serial : std::min(serial, hostFirstPendingSerial_ - 1);
  if (cappedSerial == 0) {
    // Everything queued so far is older than nothing: there is no serial this completion may
    // report, so it reports none rather than an unqueued one.
    return;
  }

  // Callback-mode handling (wgpu-native vs emdawnwebgpu) is centralized in
  // notifyWhenSubmittedWorkDone; waitForSerial's poll loop drives delivery.
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
  auto workDoneState = std::make_shared<WorkDoneState>();
  workDoneState->completion = completionState_;
  workDoneState->serial = cappedSerial;
  notifyWhenSubmittedWorkDone(geodeDevice_.queue(), workDoneState);
}

void GeodeWgpuAdapterDevice::recordPendingHostSerial(uint64_t submissionSerial) {
  // The oldest of these is the cap every completion respects, so it is remembered separately
  // from the newest, which is what the eventual flush reports.
  if (hostFirstPendingSerial_ == 0) {
    hostFirstPendingSerial_ = submissionSerial;
  }
  hostPendingSerial_ = std::max(hostPendingSerial_, submissionSerial);
}

bool GeodeWgpuAdapterDevice::replaysIntoHostEncoder() const {
  return static_cast<bool>(hostCommandEncoder_) && !bypassHostEncoderForSubmit_;
}

gpu::Result<uint64_t> GeodeWgpuAdapterDevice::submitStandalone(gpu::CommandBuffer&& commands) {
  // Scoped rather than a separate encode path: the standalone submit differs from an ordinary
  // one only in ignoring the host encoder, so it runs the same encoding and the same completion
  // bookkeeping.
  const bool previous = std::exchange(bypassHostEncoderForSubmit_, true);
  gpu::Result<uint64_t> serial = submit(std::move(commands));
  bypassHostEncoderForSubmit_ = previous;

  // Remember it so the host's eventual flush reports it. Its own completion cannot: it is capped
  // below the frame's unqueued serials, which are lower than this one.
  if (serial.hasResult() && hostPendingSerial_ != 0) {
    standaloneWhilePendingSerial_ = std::max(standaloneWhilePendingSerial_, serial.result());
  }
  return serial;
}

gpu::Status GeodeWgpuAdapterDevice::onSubmit(uint64_t submissionSerial,
                                             uint32_t commandBufferSlotIndex,
                                             std::span<const gpu::Command> commands) {
  (void)commandBufferSlotIndex;

  // Replay into the host's encoder when one is installed, so a caller that also records spans
  // this runtime cannot express keeps one command buffer covering the whole frame in order.
  const bool replayingIntoHost = replaysIntoHostEncoder();

  EncodingState state;
  if (replayingIntoHost) {
    state.encoder = hostCommandEncoder_;
  } else {
    state.ownedEncoder.reset(geodeDevice_.device().createCommandEncoder());
    state.encoder = state.ownedEncoder.get();
  }
  if (!state.encoder) {
    return GpuError{GpuErrorType::InvalidState, "wgpu command encoder creation failed"};
  }

  // On any encoding failure, close an open pass before returning so the un-finished command
  // encoder tears down cleanly, then fail closed.
  const auto failEncoding = [&state](gpu::Status error) -> gpu::Status {
    if (state.pass) {
      state.pass.get().end();
      state.pass.reset();
    }
    if (state.computePass) {
      state.computePass.get().end();
      state.computePass.reset();
    }
    return error;
  };

  for (const gpu::Command& command : commands) {
    gpu::Status commandStatus = encodeCommand(state, command);
    if (commandStatus.hasError()) {
      return failEncoding(std::move(commandStatus));
    }
  }

  if (state.pass || state.computePass) {
    // The encoder state machine guarantees passes are ended before finish; fail closed anyway.
    return failEncoding(
        GpuError{GpuErrorType::InvalidState, "submitted command stream left a pass open"});
  }

  if (replayingIntoHost) {
    // The host owns finish + submit for its encoder. Hold the serial back until it reports that
    // submit: reporting completion before the work is even submitted would be a lie the deferred
    // destruction and wait paths both act on.
    recordPendingHostSerial(submissionSerial);
    return OkStatus();
  }

  ScopedWgpuHandle<wgpu::CommandBuffer> commandBuffer(state.encoder.finish());
  if (!commandBuffer) {
    return GpuError{GpuErrorType::InvalidState, "wgpu command buffer finish failed"};
  }
  geodeDevice_.queue().submit(1, &commandBuffer.get());
  geodeDevice_.countSubmit();

  advanceCompletedSerialWhenQueueDrains(submissionSerial);
  return OkStatus();
}

}  // namespace donner::geode
