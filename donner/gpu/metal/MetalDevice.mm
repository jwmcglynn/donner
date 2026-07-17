/// @file
/// Metal backend implementation for \c donner::gpu::metal::MetalDevice (Objective-C++).
///
/// Compiled with ARC (Bazel `objc_library` compiles `srcs` with ARC), so Metal objects held in
/// C++ containers use implicit `__strong` semantics: clearing a slot to nil releases the object.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/metal/MetalDevice.h"
#include "donner/gpu/shader/MslBindingMap.h"

namespace donner::gpu::metal {

namespace {

/// Ensures \p table covers \p slotIndex and stores \p value there. Slots are value-initialized
/// (nil for ObjC ids, empty for optionals) until written.
template <typename T>
void SetSlot(std::vector<T>& table, uint32_t slotIndex, T value) {
  if (table.size() <= slotIndex) {
    table.resize(slotIndex + 1);
  }
  table[slotIndex] = std::move(value);
}

/// Returns the value at \p slotIndex, or a value-initialized T (nil / empty) if out of range.
template <typename T>
T GetSlot(const std::vector<T>& table, uint32_t slotIndex) {
  return slotIndex < table.size() ? table[slotIndex] : T{};
}

/// Returns a pointer to the record stored at \p slotIndex, or nullptr if the slot is empty.
template <typename Record>
const Record* FindRecord(const std::vector<std::optional<Record>>& table, uint32_t slotIndex) {
  if (slotIndex >= table.size() || !table[slotIndex].has_value()) {
    return nullptr;
  }
  return &table[slotIndex].value();
}

/// Converts UTF-8 text to NSString; returns nil on invalid UTF-8.
NSString* ToNSString(std::string_view text) {
  return [[NSString alloc] initWithBytes:text.data()
                                  length:text.size()
                                encoding:NSUTF8StringEncoding];
}

/// Extracts a readable message from an NSError, falling back to \p fallback when nil.
std::string DescribeNSError(NSError* error, std::string_view fallback) {
  if (error != nil && error.localizedDescription != nil) {
    return std::string([error.localizedDescription UTF8String]);
  }
  return std::string(fallback);
}

MTLPixelFormat ToMtlPixelFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
    case TextureFormat::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
    case TextureFormat::R8Unorm: return MTLPixelFormatR8Unorm;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated TextureFormat out of range");
  return MTLPixelFormatRGBA8Unorm;
}

MTLSamplerMinMagFilter ToMtlFilter(FilterMode mode) {
  switch (mode) {
    case FilterMode::Nearest: return MTLSamplerMinMagFilterNearest;
    case FilterMode::Linear: return MTLSamplerMinMagFilterLinear;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated FilterMode out of range");
  return MTLSamplerMinMagFilterNearest;
}

MTLSamplerAddressMode ToMtlAddressMode(AddressMode mode) {
  switch (mode) {
    case AddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
    case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated AddressMode out of range");
  return MTLSamplerAddressModeClampToEdge;
}

MTLVertexFormat ToMtlVertexFormat(VertexFormat format) {
  switch (format) {
    case VertexFormat::Float32x2: return MTLVertexFormatFloat2;
    case VertexFormat::Float32x4: return MTLVertexFormatFloat4;
    case VertexFormat::Uint32: return MTLVertexFormatUInt;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexFormat out of range");
  return MTLVertexFormatFloat2;
}

MTLVertexStepFunction ToMtlStepFunction(VertexStepMode mode) {
  switch (mode) {
    case VertexStepMode::Vertex: return MTLVertexStepFunctionPerVertex;
    case VertexStepMode::Instance: return MTLVertexStepFunctionPerInstance;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated VertexStepMode out of range");
  return MTLVertexStepFunctionPerVertex;
}

MTLBlendFactor ToMtlBlendFactor(BlendFactor factor) {
  switch (factor) {
    case BlendFactor::Zero: return MTLBlendFactorZero;
    case BlendFactor::One: return MTLBlendFactorOne;
    case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendFactor out of range");
  return MTLBlendFactorZero;
}

MTLBlendOperation ToMtlBlendOperation(BlendOperation operation) {
  switch (operation) {
    case BlendOperation::Add: return MTLBlendOperationAdd;
    case BlendOperation::Max: return MTLBlendOperationMax;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated BlendOperation out of range");
  return MTLBlendOperationAdd;
}

MTLColorWriteMask ToMtlColorWriteMask(ColorWriteMask mask) {
  MTLColorWriteMask result = MTLColorWriteMaskNone;
  if (HasAllFlags(mask, ColorWriteMask::Red)) {
    result |= MTLColorWriteMaskRed;
  }
  if (HasAllFlags(mask, ColorWriteMask::Green)) {
    result |= MTLColorWriteMaskGreen;
  }
  if (HasAllFlags(mask, ColorWriteMask::Blue)) {
    result |= MTLColorWriteMaskBlue;
  }
  if (HasAllFlags(mask, ColorWriteMask::Alpha)) {
    result |= MTLColorWriteMaskAlpha;
  }
  return result;
}

MTLLoadAction ToMtlLoadAction(LoadOp op) {
  switch (op) {
    case LoadOp::Clear: return MTLLoadActionClear;
    case LoadOp::Load: return MTLLoadActionLoad;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated LoadOp out of range");
  return MTLLoadActionClear;
}

MTLStoreAction ToMtlStoreAction(StoreOp op) {
  switch (op) {
    case StoreOp::Store: return MTLStoreActionStore;
    case StoreOp::Discard: return MTLStoreActionDontCare;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated StoreOp out of range");
  return MTLStoreActionStore;
}

MTLPrimitiveType ToMtlPrimitiveType(PrimitiveTopology topology) {
  switch (topology) {
    case PrimitiveTopology::TriangleList: return MTLPrimitiveTypeTriangle;
    case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
  }
  UTILS_RELEASE_ASSERT_MSG(false, "validated PrimitiveTopology out of range");
  return MTLPrimitiveTypeTriangle;
}

/// State shared with Metal command-buffer completion handlers, which run on a Metal-internal
/// thread. Held by shared_ptr so a handler that outlives the device touches valid memory.
struct CompletionState {
  std::atomic<uint64_t> completedSerial{0};  //!< Highest completed submission serial.
  std::atomic<bool> hadError{false};         //!< True once any command buffer reported an error.
  std::mutex mutex;                          //!< Guards errorMessage.
  std::string errorMessage;                  //!< Message of the first captured execution error.
};

}  // namespace

/// Objective-C++ state of a MetalDevice: the Metal device and queue plus per-resource slot
/// tables mirroring the validated slot indices handed to the `on*` hooks.
struct MetalDevice::Impl {
  id<MTLDevice> device = nil;              //!< The Metal device; set by Create.
  id<MTLCommandQueue> commandQueue = nil;  //!< Lazily created on first submit.

  /// A bind group plus the layout slot it was created against (for per-binding visibility).
  struct BindGroupRecord {
    BindGroupDescriptor descriptor;  //!< Validated creation descriptor (entries).
    uint32_t layoutSlot = 0;         //!< Slot of the bind group layout.
  };

  /// A compiled render pipeline plus the topology every draw through it uses.
  struct RenderPipelineRecord {
    id<MTLRenderPipelineState> state = nil;                        //!< Compiled pipeline state.
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;  //!< Pipeline topology.
  };

  std::vector<id<MTLBuffer>> buffers;                         //!< Buffer slots.
  std::vector<id<MTLTexture>> textures;                       //!< Texture slots.
  std::vector<std::optional<uint32_t>> textureViewToTexture;  //!< View slot -> texture slot.
  std::vector<id<MTLSamplerState>> samplers;                  //!< Sampler slots.
  std::vector<std::optional<BindGroupLayoutDescriptor>> bindGroupLayouts;  //!< Layout slots.
  std::vector<std::optional<BindGroupRecord>> bindGroups;                  //!< Bind group slots.
  std::vector<std::optional<PipelineLayoutDescriptor>> pipelineLayouts;    //!< Pipeline layouts.
  std::vector<id<MTLLibrary>> shaderLibraries;                             //!< Shader module slots.
  std::vector<std::optional<RenderPipelineRecord>> renderPipelines;  //!< Render pipeline slots.

  std::shared_ptr<CompletionState> completionState =
      std::make_shared<CompletionState>();  //!< Shared with completion handlers.
};

std::unique_ptr<MetalDevice> MetalDevice::Create() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    return nullptr;
  }

  std::unique_ptr<MetalDevice> result(new MetalDevice());
  result->impl_->device = device;
  return result;
}

MetalDevice::MetalDevice() : impl_(std::make_unique<Impl>()) {}

MetalDevice::~MetalDevice() = default;

uint64_t MetalDevice::completedSerial() const {
  return impl_->completionState->completedSerial.load(std::memory_order_acquire);
}

bool MetalDevice::waitForSerial(uint64_t serial, double timeoutSeconds) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(timeoutSeconds));
  const CompletionState& state = *impl_->completionState;
  for (;;) {
    if (state.hadError.load(std::memory_order_acquire)) {
      return false;
    }
    if (state.completedSerial.load(std::memory_order_acquire) >= serial) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

Result<std::vector<uint8_t>> MetalDevice::readBackBuffer(const Buffer& buffer) {
  if (!buffer.isValid()) {
    return GpuError{GpuErrorType::InvalidHandle, "buffer handle is null"};
  }
  if (buffer.deviceId() != deviceId()) {
    return GpuError{GpuErrorType::DeviceMismatch,
                    std::format("buffer handle belongs to device {} but was used with device {}",
                                buffer.deviceId(), deviceId())};
  }
  id<MTLBuffer> metalBuffer = GetSlot(impl_->buffers, buffer.slotIndex());
  if (metalBuffer == nil) {
    return GpuError{GpuErrorType::InvalidHandle,
                    std::format("buffer handle (slot {}) does not name a live Metal buffer",
                                buffer.slotIndex())};
  }

  const uint8_t* contents = static_cast<const uint8_t*>(metalBuffer.contents);
  return std::vector<uint8_t>(contents, contents + metalBuffer.length);
}

std::string MetalDevice::lastErrorForTest() const {
  CompletionState& state = *impl_->completionState;
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.errorMessage;
}

Status MetalDevice::onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) {
  id<MTLBuffer> buffer = [impl_->device newBufferWithLength:descriptor.byteSize
                                                    options:MTLResourceStorageModeShared];
  if (buffer == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("Metal buffer allocation of {} bytes failed for '{}'",
                                descriptor.byteSize, std::string_view(descriptor.label))};
  }

  SetSlot(impl_->buffers, slotIndex, buffer);
  return OkStatus();
}

Status MetalDevice::onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) {
  MTLTextureDescriptor* textureDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:ToMtlPixelFormat(descriptor.format)
                                                         width:descriptor.size.width
                                                        height:descriptor.size.height
                                                     mipmapped:NO];

  MTLTextureUsage usage = 0;
  if (HasAllFlags(descriptor.usage, TextureUsage::RenderAttachment)) {
    usage |= MTLTextureUsageRenderTarget;
  }
  if (HasAllFlags(descriptor.usage, TextureUsage::Sampled)) {
    usage |= MTLTextureUsageShaderRead;
  }
  textureDescriptor.usage = usage;
  // Shared storage: this backend targets Apple Silicon's unified memory, so render targets stay
  // directly host-visible and blit readback needs no staging or synchronize step.
  textureDescriptor.storageMode = MTLStorageModeShared;

  id<MTLTexture> texture = [impl_->device newTextureWithDescriptor:textureDescriptor];
  if (texture == nil) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("Metal texture allocation ({}x{}) failed for '{}'", descriptor.size.width,
                    descriptor.size.height, std::string_view(descriptor.label))};
  }

  SetSlot(impl_->textures, slotIndex, texture);
  return OkStatus();
}

Status MetalDevice::onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                                        const TextureViewDescriptor& descriptor) {
  // Views cover the whole texture, so the backend records only the view -> texture mapping and
  // binds the underlying texture wherever the view is consumed.
  (void)descriptor;
  SetSlot(impl_->textureViewToTexture, slotIndex, std::optional<uint32_t>(textureSlotIndex));
  return OkStatus();
}

Status MetalDevice::onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) {
  MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
  samplerDescriptor.minFilter = ToMtlFilter(descriptor.minFilter);
  samplerDescriptor.magFilter = ToMtlFilter(descriptor.magFilter);
  samplerDescriptor.sAddressMode = ToMtlAddressMode(descriptor.addressModeU);
  samplerDescriptor.tAddressMode = ToMtlAddressMode(descriptor.addressModeV);

  id<MTLSamplerState> sampler = [impl_->device newSamplerStateWithDescriptor:samplerDescriptor];
  if (sampler == nil) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("Metal sampler creation failed for '{}'", std::string_view(descriptor.label))};
  }

  SetSlot(impl_->samplers, slotIndex, sampler);
  return OkStatus();
}

Status MetalDevice::onCreateBindGroupLayout(uint32_t slotIndex,
                                            const BindGroupLayoutDescriptor& descriptor) {
  SetSlot(impl_->bindGroupLayouts, slotIndex, std::optional<BindGroupLayoutDescriptor>(descriptor));
  return OkStatus();
}

Status MetalDevice::onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) {
  SetSlot(impl_->bindGroups, slotIndex,
          std::optional<Impl::BindGroupRecord>(
              Impl::BindGroupRecord{descriptor, descriptor.layout.slotIndex()}));
  return OkStatus();
}

Status MetalDevice::onCreatePipelineLayout(uint32_t slotIndex,
                                           const PipelineLayoutDescriptor& descriptor) {
  // Metal has no pipeline layout object; the descriptor is recorded for completeness. Binding
  // indices map through MslBindingMap.h at draw-encoding time.
  SetSlot(impl_->pipelineLayouts, slotIndex, std::optional<PipelineLayoutDescriptor>(descriptor));
  return OkStatus();
}

Status MetalDevice::onCreateShaderModule(uint32_t slotIndex,
                                         const ShaderModuleDescriptor& descriptor) {
  if (descriptor.sourceKind != ShaderSourceKind::Msl) {
    return GpuError{GpuErrorType::Unsupported, "the Metal backend compiles MSL only"};
  }

  NSString* source = ToNSString(std::string_view(descriptor.sourceText));
  if (source == nil) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("shader source for '{}' is not valid UTF-8",
                                std::string_view(descriptor.label))};
  }

  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
  NSError* error = nil;
  id<MTLLibrary> library = [impl_->device newLibraryWithSource:source options:options error:&error];
  if (library == nil) {
    return GpuError{
        GpuErrorType::InvalidDescriptor,
        std::format("MSL compilation failed for '{}': {}", std::string_view(descriptor.label),
                    DescribeNSError(error, "no compiler diagnostics"))};
  }

  SetSlot(impl_->shaderLibraries, slotIndex, library);
  return OkStatus();
}

Status MetalDevice::onCreateRenderPipeline(uint32_t slotIndex,
                                           const RenderPipelineDescriptor& descriptor) {
  id<MTLLibrary> vertexLibrary =
      GetSlot(impl_->shaderLibraries, descriptor.vertex.module.slotIndex());
  id<MTLLibrary> fragmentLibrary =
      GetSlot(impl_->shaderLibraries, descriptor.fragment.module.slotIndex());
  if (vertexLibrary == nil || fragmentLibrary == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    "render pipeline references a shader module with no compiled Metal library"};
  }

  NSString* vertexEntryPoint = ToNSString(std::string_view(descriptor.vertex.entryPoint));
  NSString* fragmentEntryPoint = ToNSString(std::string_view(descriptor.fragment.entryPoint));
  id<MTLFunction> vertexFunction =
      vertexEntryPoint != nil ? [vertexLibrary newFunctionWithName:vertexEntryPoint] : nil;
  id<MTLFunction> fragmentFunction =
      fragmentEntryPoint != nil ? [fragmentLibrary newFunctionWithName:fragmentEntryPoint] : nil;
  if (vertexFunction == nil) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("vertex entry point '{}' not found in shader module",
                                std::string_view(descriptor.vertex.entryPoint))};
  }
  if (fragmentFunction == nil) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("fragment entry point '{}' not found in shader module",
                                std::string_view(descriptor.fragment.entryPoint))};
  }

  MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
  pipelineDescriptor.vertexFunction = vertexFunction;
  pipelineDescriptor.fragmentFunction = fragmentFunction;

  if (!descriptor.vertex.buffers.empty()) {
    if (descriptor.vertex.buffers.size() != 1) {
      return GpuError{GpuErrorType::Unsupported,
                      "the Metal backend supports a single vertex buffer layout (slot 0) in this "
                      "slice"};
    }

    const VertexBufferLayout& layout = descriptor.vertex.buffers[0];
    MTLVertexDescriptor* vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    for (const VertexAttribute& attribute : layout.attributes) {
      MTLVertexAttributeDescriptor* attributeDescriptor =
          vertexDescriptor.attributes[attribute.shaderLocation];
      attributeDescriptor.format = ToMtlVertexFormat(attribute.format);
      attributeDescriptor.offset = attribute.offsetBytes;
      attributeDescriptor.bufferIndex = shader::kMslVertexBufferIndex;
    }

    MTLVertexBufferLayoutDescriptor* layoutDescriptor =
        vertexDescriptor.layouts[shader::kMslVertexBufferIndex];
    layoutDescriptor.stride = layout.strideBytes;
    layoutDescriptor.stepFunction = ToMtlStepFunction(layout.stepMode);
    pipelineDescriptor.vertexDescriptor = vertexDescriptor;
  }

  for (size_t i = 0; i < descriptor.fragment.targets.size(); ++i) {
    const ColorTargetState& target = descriptor.fragment.targets[i];
    MTLRenderPipelineColorAttachmentDescriptor* attachment = pipelineDescriptor.colorAttachments[i];
    attachment.pixelFormat = ToMtlPixelFormat(target.format);
    attachment.writeMask = ToMtlColorWriteMask(target.writeMask);
    if (target.blend.has_value()) {
      attachment.blendingEnabled = YES;
      attachment.sourceRGBBlendFactor = ToMtlBlendFactor(target.blend->color.srcFactor);
      attachment.destinationRGBBlendFactor = ToMtlBlendFactor(target.blend->color.dstFactor);
      attachment.rgbBlendOperation = ToMtlBlendOperation(target.blend->color.operation);
      attachment.sourceAlphaBlendFactor = ToMtlBlendFactor(target.blend->alpha.srcFactor);
      attachment.destinationAlphaBlendFactor = ToMtlBlendFactor(target.blend->alpha.dstFactor);
      attachment.alphaBlendOperation = ToMtlBlendOperation(target.blend->alpha.operation);
    }
  }

  NSError* error = nil;
  id<MTLRenderPipelineState> pipelineState =
      [impl_->device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
  if (pipelineState == nil) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("Metal render pipeline creation failed for '{}': {}",
                                std::string_view(descriptor.label),
                                DescribeNSError(error, "no pipeline diagnostics"))};
  }

  SetSlot(impl_->renderPipelines, slotIndex,
          std::optional<Impl::RenderPipelineRecord>(
              Impl::RenderPipelineRecord{pipelineState, descriptor.topology}));
  return OkStatus();
}

void MetalDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  // Clearing a slot to nil / nullopt releases the ObjC object under ARC. Unknown resource names
  // (e.g. types added by later packets) are ignored; the base class owns their bookkeeping.
  if (resourceName == "buffer") {
    SetSlot(impl_->buffers, slotIndex, id<MTLBuffer>(nil));
  } else if (resourceName == "texture") {
    SetSlot(impl_->textures, slotIndex, id<MTLTexture>(nil));
  } else if (resourceName == "textureView") {
    SetSlot(impl_->textureViewToTexture, slotIndex, std::optional<uint32_t>());
  } else if (resourceName == "sampler") {
    SetSlot(impl_->samplers, slotIndex, id<MTLSamplerState>(nil));
  } else if (resourceName == "bindGroupLayout") {
    SetSlot(impl_->bindGroupLayouts, slotIndex, std::optional<BindGroupLayoutDescriptor>());
  } else if (resourceName == "bindGroup") {
    SetSlot(impl_->bindGroups, slotIndex, std::optional<Impl::BindGroupRecord>());
  } else if (resourceName == "pipelineLayout") {
    SetSlot(impl_->pipelineLayouts, slotIndex, std::optional<PipelineLayoutDescriptor>());
  } else if (resourceName == "shaderModule") {
    SetSlot(impl_->shaderLibraries, slotIndex, id<MTLLibrary>(nil));
  } else if (resourceName == "renderPipeline") {
    SetSlot(impl_->renderPipelines, slotIndex, std::optional<Impl::RenderPipelineRecord>());
  }
}

Status MetalDevice::onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                                  std::span<const uint8_t> data) {
  id<MTLBuffer> buffer = GetSlot(impl_->buffers, slotIndex);
  if (buffer == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("buffer slot {} has no Metal buffer", slotIndex)};
  }

  if (!data.empty()) {
    std::memcpy(static_cast<uint8_t*>(buffer.contents) + offsetBytes, data.data(), data.size());
  }
  return OkStatus();
}

Status MetalDevice::onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                                   const TexelCopyBufferLayout& dataLayout,
                                   const Extent2d& writeSize) {
  id<MTLTexture> texture = GetSlot(impl_->textures, slotIndex);
  if (texture == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("texture slot {} has no Metal texture", slotIndex)};
  }

  [texture replaceRegion:MTLRegionMake2D(0, 0, writeSize.width, writeSize.height)
             mipmapLevel:0
               withBytes:data.data() + dataLayout.offsetBytes
             bytesPerRow:dataLayout.bytesPerRow];
  return OkStatus();
}

Status MetalDevice::onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                             std::span<const Command> commands) {
  (void)commandBufferSlotIndex;

  if (impl_->commandQueue == nil) {
    impl_->commandQueue = [impl_->device newCommandQueue];
    if (impl_->commandQueue == nil) {
      return GpuError{GpuErrorType::InvalidState, "Metal command queue creation failed"};
    }
  }

  id<MTLCommandBuffer> commandBuffer = [impl_->commandQueue commandBuffer];
  if (commandBuffer == nil) {
    return GpuError{GpuErrorType::InvalidState, "Metal command buffer creation failed"};
  }

  id<MTLRenderCommandEncoder> renderEncoder = nil;
  PrimitiveTopology currentTopology = PrimitiveTopology::TriangleList;

  // On any encoding failure, close an open encoder before returning so the un-committed command
  // buffer tears down cleanly, then fail closed.
  const auto failEncoding = [&renderEncoder](GpuError error) -> Status {
    if (renderEncoder != nil) {
      [renderEncoder endEncoding];
      renderEncoder = nil;
    }
    return std::move(error);
  };

  for (const Command& command : commands) {
    if (const auto* beginPass = std::get_if<BeginRenderPassCommand>(&command)) {
      MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
      const auto& attachments = beginPass->descriptor.colorAttachments;
      for (size_t i = 0; i < attachments.size(); ++i) {
        const RenderPassColorAttachment& attachment = attachments[i];
        const std::optional<uint32_t> textureSlot =
            GetSlot(impl_->textureViewToTexture, attachment.view.slotIndex());
        id<MTLTexture> texture =
            textureSlot.has_value() ? GetSlot(impl_->textures, *textureSlot) : nil;
        if (texture == nil) {
          return failEncoding(GpuError{
              GpuErrorType::InvalidState,
              std::format("render pass attachment {} does not resolve to a Metal texture", i)});
        }

        MTLRenderPassColorAttachmentDescriptor* colorAttachment =
            passDescriptor.colorAttachments[i];
        colorAttachment.texture = texture;
        colorAttachment.loadAction = ToMtlLoadAction(attachment.loadOp);
        colorAttachment.storeAction = ToMtlStoreAction(attachment.storeOp);
        colorAttachment.clearColor =
            MTLClearColorMake(attachment.clearColor[0], attachment.clearColor[1],
                              attachment.clearColor[2], attachment.clearColor[3]);
      }

      renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:passDescriptor];
      if (renderEncoder == nil) {
        return GpuError{GpuErrorType::InvalidState, "Metal render command encoder creation failed"};
      }
    } else if (const auto* setPipeline = std::get_if<SetPipelineCommand>(&command)) {
      const Impl::RenderPipelineRecord* pipeline =
          FindRecord(impl_->renderPipelines, setPipeline->pipelineSlot);
      if (renderEncoder == nil || pipeline == nullptr || pipeline->state == nil) {
        return failEncoding(GpuError{GpuErrorType::InvalidState,
                                     std::format("setPipeline: pipeline slot {} is not encodable",
                                                 setPipeline->pipelineSlot)});
      }
      [renderEncoder setRenderPipelineState:pipeline->state];
      currentTopology = pipeline->topology;
    } else if (const auto* setBindGroup = std::get_if<SetBindGroupCommand>(&command)) {
      if (setBindGroup->index != 0) {
        return failEncoding(
            GpuError{GpuErrorType::Unsupported,
                     "the Metal backend maps bind group 0 only in this slice (MslBindingMap.h)"});
      }
      const Impl::BindGroupRecord* bindGroup =
          FindRecord(impl_->bindGroups, setBindGroup->bindGroupSlot);
      const BindGroupLayoutDescriptor* layout =
          bindGroup != nullptr ? FindRecord(impl_->bindGroupLayouts, bindGroup->layoutSlot)
                               : nullptr;
      if (renderEncoder == nil || bindGroup == nullptr || layout == nullptr) {
        return failEncoding(
            GpuError{GpuErrorType::InvalidState,
                     std::format("setBindGroup: bind group slot {} is not encodable",
                                 setBindGroup->bindGroupSlot)});
      }

      Status bindStatus = OkStatus();
      for (const BindGroupEntry& entry : bindGroup->descriptor.entries) {
        ShaderStage visibility = ShaderStage::None;
        for (const BindGroupLayoutEntry& layoutEntry : layout->entries) {
          if (layoutEntry.binding == entry.binding) {
            visibility = layoutEntry.visibility;
            break;
          }
        }

        if (const auto* bufferBinding = std::get_if<BufferBinding>(&entry.resource)) {
          id<MTLBuffer> buffer = GetSlot(impl_->buffers, bufferBinding->buffer.slotIndex());
          if (buffer == nil) {
            bindStatus =
                GpuError{GpuErrorType::InvalidState,
                         std::format("setBindGroup: binding {} does not resolve to a Metal buffer",
                                     entry.binding)};
            break;
          }
          if (HasAllFlags(visibility, ShaderStage::Vertex)) {
            [renderEncoder setVertexBuffer:buffer
                                    offset:bufferBinding->offsetBytes
                                   atIndex:shader::MslBufferIndex(entry.binding)];
          }
          if (HasAllFlags(visibility, ShaderStage::Fragment)) {
            [renderEncoder setFragmentBuffer:buffer
                                      offset:bufferBinding->offsetBytes
                                     atIndex:shader::MslBufferIndex(entry.binding)];
          }
        } else if (const auto* viewBinding = std::get_if<TextureViewBinding>(&entry.resource)) {
          const std::optional<uint32_t> textureSlot =
              GetSlot(impl_->textureViewToTexture, viewBinding->view.slotIndex());
          id<MTLTexture> texture =
              textureSlot.has_value() ? GetSlot(impl_->textures, *textureSlot) : nil;
          if (texture == nil) {
            bindStatus =
                GpuError{GpuErrorType::InvalidState,
                         std::format("setBindGroup: binding {} does not resolve to a Metal texture",
                                     entry.binding)};
            break;
          }
          if (HasAllFlags(visibility, ShaderStage::Vertex)) {
            [renderEncoder setVertexTexture:texture atIndex:shader::MslTextureIndex(entry.binding)];
          }
          if (HasAllFlags(visibility, ShaderStage::Fragment)) {
            [renderEncoder setFragmentTexture:texture
                                      atIndex:shader::MslTextureIndex(entry.binding)];
          }
        } else if (const auto* samplerBinding = std::get_if<SamplerBinding>(&entry.resource)) {
          id<MTLSamplerState> sampler =
              GetSlot(impl_->samplers, samplerBinding->sampler.slotIndex());
          if (sampler == nil) {
            bindStatus =
                GpuError{GpuErrorType::InvalidState,
                         std::format("setBindGroup: binding {} does not resolve to a Metal sampler",
                                     entry.binding)};
            break;
          }
          if (HasAllFlags(visibility, ShaderStage::Vertex)) {
            [renderEncoder setVertexSamplerState:sampler
                                         atIndex:shader::MslSamplerIndex(entry.binding)];
          }
          if (HasAllFlags(visibility, ShaderStage::Fragment)) {
            [renderEncoder setFragmentSamplerState:sampler
                                           atIndex:shader::MslSamplerIndex(entry.binding)];
          }
        }
      }
      if (bindStatus.hasError()) {
        return failEncoding(std::move(bindStatus).error());
      }
    } else if (const auto* setVertexBuffer = std::get_if<SetVertexBufferCommand>(&command)) {
      if (setVertexBuffer->slot != 0) {
        return failEncoding(
            GpuError{GpuErrorType::Unsupported,
                     "the Metal backend supports vertex buffer slot 0 only in this slice"});
      }
      id<MTLBuffer> buffer = GetSlot(impl_->buffers, setVertexBuffer->bufferSlot);
      if (renderEncoder == nil || buffer == nil) {
        return failEncoding(GpuError{GpuErrorType::InvalidState,
                                     std::format("setVertexBuffer: buffer slot {} is not encodable",
                                                 setVertexBuffer->bufferSlot)});
      }
      [renderEncoder setVertexBuffer:buffer
                              offset:setVertexBuffer->offsetBytes
                             atIndex:shader::kMslVertexBufferIndex];
    } else if (const auto* setScissor = std::get_if<SetScissorRectCommand>(&command)) {
      if (renderEncoder == nil) {
        return failEncoding(
            GpuError{GpuErrorType::InvalidState, "setScissorRect outside a render pass"});
      }
      [renderEncoder setScissorRect:MTLScissorRect{setScissor->x, setScissor->y, setScissor->width,
                                                   setScissor->height}];
    } else if (const auto* setViewport = std::get_if<SetViewportCommand>(&command)) {
      if (renderEncoder == nil) {
        return failEncoding(
            GpuError{GpuErrorType::InvalidState, "setViewport outside a render pass"});
      }
      [renderEncoder setViewport:MTLViewport{setViewport->x, setViewport->y, setViewport->width,
                                             setViewport->height, setViewport->minDepth,
                                             setViewport->maxDepth}];
    } else if (const auto* draw = std::get_if<DrawCommand>(&command)) {
      if (renderEncoder == nil) {
        return failEncoding(GpuError{GpuErrorType::InvalidState, "draw outside a render pass"});
      }
      [renderEncoder drawPrimitives:ToMtlPrimitiveType(currentTopology)
                        vertexStart:draw->firstVertex
                        vertexCount:draw->vertexCount
                      instanceCount:draw->instanceCount
                       baseInstance:draw->firstInstance];
    } else if (std::get_if<EndRenderPassCommand>(&command) != nullptr) {
      if (renderEncoder == nil) {
        return GpuError{GpuErrorType::InvalidState, "endRenderPass without an active render pass"};
      }
      [renderEncoder endEncoding];
      renderEncoder = nil;
    } else if (const auto* copy = std::get_if<CopyTextureToBufferCommand>(&command)) {
      if (renderEncoder != nil) {
        return failEncoding(
            GpuError{GpuErrorType::InvalidState, "copyTextureToBuffer inside a render pass"});
      }
      id<MTLTexture> texture = GetSlot(impl_->textures, copy->textureSlot);
      id<MTLBuffer> buffer = GetSlot(impl_->buffers, copy->bufferSlot);
      if (texture == nil || buffer == nil) {
        return GpuError{GpuErrorType::InvalidState,
                        "copyTextureToBuffer: source texture or destination buffer is missing"};
      }
      id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];
      if (blitEncoder == nil) {
        return GpuError{GpuErrorType::InvalidState, "Metal blit command encoder creation failed"};
      }
      [blitEncoder copyFromTexture:texture
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(copy->copySize.width, copy->copySize.height, 1)
                          toBuffer:buffer
                 destinationOffset:copy->layout.offsetBytes
            destinationBytesPerRow:copy->layout.bytesPerRow
          destinationBytesPerImage:static_cast<uint64_t>(copy->layout.bytesPerRow) *
                                   copy->layout.rowsPerImage];
      [blitEncoder endEncoding];
    }
  }

  if (renderEncoder != nil) {
    // The encoder state machine guarantees passes are ended before finish; fail closed anyway.
    [renderEncoder endEncoding];
    return GpuError{GpuErrorType::InvalidState, "submitted command stream left a render pass open"};
  }

  std::shared_ptr<CompletionState> completionState = impl_->completionState;
  [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
    if (completedBuffer.error != nil) {
      completionState->hadError.store(true, std::memory_order_release);
      std::lock_guard<std::mutex> lock(completionState->mutex);
      if (completionState->errorMessage.empty()) {
        completionState->errorMessage =
            DescribeNSError(completedBuffer.error, "Metal command buffer execution failed");
      }
    }

    // Monotonic max: handlers may complete out of order across command buffers.
    uint64_t previous = completionState->completedSerial.load(std::memory_order_relaxed);
    while (previous < submissionSerial &&
           !completionState->completedSerial.compare_exchange_weak(
               previous, submissionSerial, std::memory_order_release, std::memory_order_relaxed)) {}
  }];
  [commandBuffer commit];

  return OkStatus();
}

}  // namespace donner::gpu::metal
