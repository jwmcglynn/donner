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
    case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
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
    /// Snapshot of the layout the group was created against, taken at creation time. Layouts
    /// are immutable value descriptors, so the copy stays correct even if the layout object is
    /// destroyed and its slot recycled; encode never looks the layout up by slot.
    BindGroupLayoutDescriptor layout;
  };

  /// A compiled render pipeline plus the encoder state every draw through it uses (cull mode
  /// is encoder state in Metal, so it is applied when the pipeline binds).
  struct RenderPipelineRecord {
    id<MTLRenderPipelineState> state = nil;                        //!< Compiled pipeline state.
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;  //!< Pipeline topology.
    CullMode cullMode = CullMode::None;                            //!< Pipeline cull mode.
  };

  std::vector<id<MTLBuffer>> buffers;                         //!< Buffer slots.
  std::vector<id<MTLTexture>> textures;                       //!< Texture slots.
  std::vector<std::optional<uint32_t>> textureViewToTexture;  //!< View slot -> texture slot.
  std::vector<id<MTLSamplerState>> samplers;                  //!< Sampler slots.
  std::vector<std::optional<BindGroupLayoutDescriptor>> bindGroupLayouts;  //!< Layout slots.
  std::vector<std::optional<BindGroupRecord>> bindGroups;                  //!< Bind group slots.
  std::vector<std::optional<PipelineLayoutDescriptor>> pipelineLayouts;    //!< Pipeline layouts.
  std::vector<id<MTLLibrary>> shaderLibraries;                             //!< Shader module slots.
  /// A compiled compute pipeline plus the threadgroup shape every dispatch through it uses;
  /// Metal takes that shape at dispatch time rather than from the pipeline state object.
  struct ComputePipelineRecord {
    id<MTLComputePipelineState> state = nil;  //!< Compiled pipeline state.
    WorkgroupSize workgroupSize;              //!< Declared threads per threadgroup.
  };

  std::vector<std::optional<RenderPipelineRecord>> renderPipelines;    //!< Render pipeline slots.
  std::vector<std::optional<ComputePipelineRecord>> computePipelines;  //!< Compute pipeline slots.

  std::shared_ptr<CompletionState> completionState =
      std::make_shared<CompletionState>();  //!< Shared with completion handlers.

  /// Mutable state threaded through the encoding of one command stream.
  struct EncodingState {
    id<MTLCommandBuffer> commandBuffer = nil;           //!< Command buffer being encoded.
    id<MTLRenderCommandEncoder> renderEncoder = nil;    //!< Active render encoder, or nil.
    id<MTLComputeCommandEncoder> computeEncoder = nil;  //!< Active compute encoder, or nil.
    /// Topology of the bound pipeline; Metal takes it per draw call rather than from the
    /// pipeline state object.
    PrimitiveTopology currentTopology = PrimitiveTopology::TriangleList;
    /// Threadgroup shape of the bound compute pipeline, applied at dispatch.
    WorkgroupSize currentWorkgroupSize;
  };

  /// Opens a render encoder for a recorded pass and configures its color attachments.
  /// @param state Encoding state.
  /// @param beginPass Recorded command.
  Status beginEncodedRenderPass(EncodingState& state, const BeginRenderPassCommand& beginPass);

  /// Binds a recorded pipeline plus the encoder state it carries.
  /// @param state Encoding state.
  /// @param setPipeline Recorded command.
  Status encodeSetPipeline(EncodingState& state, const SetPipelineCommand& setPipeline);

  /// Binds one buffer entry to the stages its layout entry declares.
  /// @param state Encoding state.
  /// @param entry Bind group entry.
  /// @param bufferBinding Buffer binding carried by the entry.
  /// @param visibility Stages the layout entry declares.
  Status encodeBufferBinding(EncodingState& state, const BindGroupEntry& entry,
                             const BufferBinding& bufferBinding, ShaderStage visibility);

  /// Binds one texture view entry to the stages its layout entry declares.
  /// @param state Encoding state.
  /// @param entry Bind group entry.
  /// @param viewBinding Texture view binding carried by the entry.
  /// @param visibility Stages the layout entry declares.
  Status encodeTextureBinding(EncodingState& state, const BindGroupEntry& entry,
                              const TextureViewBinding& viewBinding, ShaderStage visibility);

  /// Binds one sampler entry to the stages its layout entry declares.
  /// @param state Encoding state.
  /// @param entry Bind group entry.
  /// @param samplerBinding Sampler binding carried by the entry.
  /// @param visibility Stages the layout entry declares.
  Status encodeSamplerBinding(EncodingState& state, const BindGroupEntry& entry,
                              const SamplerBinding& samplerBinding, ShaderStage visibility);

  /// Binds one bind group entry, looking its stage visibility up in the layout snapshot.
  /// @param state Encoding state.
  /// @param layout Layout snapshot the group was created against.
  /// @param entry Bind group entry.
  Status encodeBindGroupEntry(EncodingState& state, const BindGroupLayoutDescriptor& layout,
                              const BindGroupEntry& entry);

  /// Binds every entry of a recorded bind group.
  /// @param state Encoding state.
  /// @param setBindGroup Recorded command.
  Status encodeSetBindGroup(EncodingState& state, const SetBindGroupCommand& setBindGroup);

  /// Binds a recorded vertex buffer.
  /// @param state Encoding state.
  /// @param setVertexBuffer Recorded command.
  Status encodeSetVertexBuffer(EncodingState& state, const SetVertexBufferCommand& setVertexBuffer);

  /// Sets an explicit scissor rectangle.
  /// @param state Encoding state.
  /// @param setScissor Recorded command.
  Status encodeSetScissorRect(EncodingState& state, const SetScissorRectCommand& setScissor);

  /// Sets an explicit viewport.
  /// @param state Encoding state.
  /// @param setViewport Recorded command.
  Status encodeSetViewport(EncodingState& state, const SetViewportCommand& setViewport);

  /// Issues a draw with the bound pipeline's topology.
  /// @param state Encoding state.
  /// @param draw Recorded command.
  Status encodeDraw(EncodingState& state, const DrawCommand& draw);

  /// Closes the active render encoder.
  /// @param state Encoding state.
  Status encodeEndRenderPass(EncodingState& state);

  /// Records a texture-to-buffer copy on a blit encoder.
  /// @param state Encoding state.
  /// @param copy Recorded command.
  Status encodeCopyTextureToBuffer(EncodingState& state, const CopyTextureToBufferCommand& copy);
  Status encodeCopyTextureToTexture(EncodingState& state, const CopyTextureToTextureCommand& copy);

  /// Encodes a render-pass command, or returns empty when \p command is not one.
  /// @param state Encoding state. @param command Recorded command.
  std::optional<Status> encodeRenderCommand(EncodingState& state, const Command& command);

  /// Opens a compute encoder for a recorded pass.
  /// @param state Encoding state.
  Status beginEncodedComputePass(EncodingState& state);

  /// Binds a recorded compute pipeline and records its threadgroup shape.
  /// @param state Encoding state. @param setPipeline Recorded command.
  Status encodeSetComputePipeline(EncodingState& state,
                                  const SetComputePipelineCommand& setPipeline);

  /// Issues a dispatch with the bound pipeline's threadgroup shape.
  /// @param state Encoding state. @param dispatch Recorded command.
  Status encodeDispatchWorkgroups(EncodingState& state, const DispatchWorkgroupsCommand& dispatch);

  /// Closes the active compute encoder.
  /// @param state Encoding state.
  Status encodeEndComputePass(EncodingState& state);

  /// Encodes a compute-pass command, or returns empty when \p command is not one.
  /// @param state Encoding state. @param command Recorded command.
  std::optional<Status> encodeComputeCommand(EncodingState& state, const Command& command);

  /// Encodes a copy command, or returns empty when \p command is not one.
  /// @param state Encoding state. @param command Recorded command.
  std::optional<Status> encodeCopyCommand(EncodingState& state, const Command& command);

  /// Encodes one recorded command.
  /// @param state Encoding state.
  /// @param command Recorded command.
  Status encodeCommand(EncodingState& state, const Command& command);

  /// Attaches the completion handler that latches execution errors and advances the completed
  /// serial monotonically.
  /// @param state Encoding state.
  /// @param submissionSerial Serial assigned to this submission.
  void attachCompletionHandler(EncodingState& state, uint64_t submissionSerial);

  /// Whether resources are built for unified memory; decides every storage mode below.
  bool unifiedMemory = true;
  uint64_t hostWritePublishCount = 0;    //!< Host ranges published to the device copy.
  uint64_t deviceWritePublishCount = 0;  //!< Submissions that published back to the host copy.

  /// Storage mode for a resource the host reads or writes.
  ///
  /// Shared is right only where the two processors address one copy. Where they do not, Managed
  /// keeps a copy on each side, and each side's changes have to be published to the other
  /// explicitly - which is what the didModifyRange and synchronizeResource steps below do.
  MTLStorageMode hostVisibleStorageMode() const {
    return unifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
  }

  /// Whether host-visible resources need explicit publication in both directions.
  bool needsExplicitHostCoherency() const { return !unifiedMemory; }

  /// Publishes every live host-visible resource's device-side changes back to the host copy, for
  /// a memory model that keeps the two apart. A no-op where the two are one copy.
  /// @param state Encoding state.
  Status encodeHostCoherencySync(EncodingState& state);
};

std::unique_ptr<MetalDevice> MetalDevice::Create(MemoryModel memoryModel) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    return nullptr;
  }

  std::unique_ptr<MetalDevice> result(new MetalDevice());
  result->impl_->device = device;
  // Ask the device rather than assuming. On a unified-memory device the CPU and GPU address one
  // copy of a shared resource and nothing has to be moved between them; on a device without it,
  // a shared resource is not the same bytes on both sides, and reading GPU output through the
  // CPU pointer without synchronizing returns whatever the host copy last held - which is zeros
  // for a buffer nothing ever wrote from the host.
  result->impl_->unifiedMemory =
      memoryModel == MemoryModel::Detected ? (device.hasUnifiedMemory != NO) : false;
  return result;
}

bool MetalDevice::usesUnifiedMemoryForTest() const {
  return impl_->unifiedMemory;
}

uint64_t MetalDevice::hostWritePublishCountForTest() const {
  return impl_->hostWritePublishCount;
}

uint64_t MetalDevice::deviceWritePublishCountForTest() const {
  return impl_->deviceWritePublishCount;
}

MetalDevice::MetalDevice() : impl_(std::make_unique<Impl>()) {}

MetalDevice::~MetalDevice() {
  // Wait for in-flight submissions so deferred destructions drain before Impl teardown releases
  // the remaining Metal objects. On timeout (a hung submission) teardown proceeds anyway: Metal
  // itself retains every resource referenced by a committed command buffer until it completes,
  // so releasing our references cannot free memory the GPU is still using.
  if (lastSubmittedSerial() > completedSerial()) {
    waitForSerial(lastSubmittedSerial(), /*timeoutSeconds=*/5.0);
  }
  poll();
}

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
  // Full handle validation (null, device identity, AND generation) through the base class, so a
  // stale handle whose slot was reused cannot read the replacement buffer.
  if (Status status = validateBufferHandleForBackend(buffer); status.hasError()) {
    return std::move(status).error();
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
  const MTLResourceOptions bufferOptions =
      impl_->unifiedMemory ? MTLResourceStorageModeShared : MTLResourceStorageModeManaged;
  id<MTLBuffer> buffer = [impl_->device newBufferWithLength:descriptor.byteSize
                                                    options:bufferOptions];
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
  if (HasAllFlags(descriptor.usage, TextureUsage::StorageBinding)) {
    usage |= MTLTextureUsageShaderWrite;
  }
  textureDescriptor.usage = usage;
  // Host-visible either way: render targets and storage textures are read back through blits
  // into host-visible buffers, and on a device without unified memory that means a managed
  // texture whose GPU-side changes are published before the host reads them.
  textureDescriptor.storageMode = impl_->hostVisibleStorageMode();

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
  // The base class validated the layout reference before this hook, so the slot lookup cannot
  // miss; the descriptor is snapshotted into the record (see Impl::BindGroupRecord::layout).
  const BindGroupLayoutDescriptor* layout =
      FindRecord(impl_->bindGroupLayouts, descriptor.layout.slotIndex());
  if (layout == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("bind group layout slot {} has no Metal-side descriptor",
                                descriptor.layout.slotIndex())};
  }
  SetSlot(impl_->bindGroups, slotIndex,
          std::optional<Impl::BindGroupRecord>(Impl::BindGroupRecord{descriptor, *layout}));
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
              Impl::RenderPipelineRecord{pipelineState, descriptor.topology, descriptor.cullMode}));
  return OkStatus();
}

Status MetalDevice::onCreateComputePipeline(uint32_t slotIndex,
                                            const ComputePipelineDescriptor& descriptor) {
  id<MTLLibrary> library = GetSlot(impl_->shaderLibraries, descriptor.compute.module.slotIndex());
  if (library == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    "compute pipeline references a shader module with no compiled Metal library"};
  }

  NSString* entryPoint = ToNSString(std::string_view(descriptor.compute.entryPoint));
  id<MTLFunction> function = entryPoint != nil ? [library newFunctionWithName:entryPoint] : nil;
  if (function == nil) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("compute entry point '{}' not found in shader module",
                                std::string_view(descriptor.compute.entryPoint))};
  }

  NSError* error = nil;
  id<MTLComputePipelineState> pipelineState =
      [impl_->device newComputePipelineStateWithFunction:function error:&error];
  if (pipelineState == nil) {
    return GpuError{GpuErrorType::InvalidDescriptor,
                    std::format("Metal compute pipeline creation failed for '{}': {}",
                                std::string_view(descriptor.label),
                                DescribeNSError(error, "no pipeline diagnostics"))};
  }
  // The declared threadgroup must fit what the compiled kernel can actually launch; a larger
  // shape would be rejected asynchronously at dispatch instead of here.
  const uint64_t declaredInvocations = uint64_t{descriptor.workgroupSize.x} *
                                       descriptor.workgroupSize.y * descriptor.workgroupSize.z;
  if (declaredInvocations > pipelineState.maxTotalThreadsPerThreadgroup) {
    return GpuError{
        GpuErrorType::LimitExceeded,
        std::format("compute pipeline '{}' declares {} threads per threadgroup but the compiled "
                    "kernel supports at most {}",
                    std::string_view(descriptor.label), declaredInvocations,
                    pipelineState.maxTotalThreadsPerThreadgroup)};
  }

  SetSlot(impl_->computePipelines, slotIndex,
          std::optional<Impl::ComputePipelineRecord>(
              Impl::ComputePipelineRecord{pipelineState, descriptor.workgroupSize}));
  return OkStatus();
}

void MetalDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  // Clearing a slot to nil / nullopt releases the ObjC object under ARC. Unknown resource names
  // are ignored; the base class owns their bookkeeping.
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
  } else if (resourceName == "computePipeline") {
    SetSlot(impl_->computePipelines, slotIndex, std::optional<Impl::ComputePipelineRecord>());
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
    if (impl_->needsExplicitHostCoherency()) {
      // The write landed in the host's copy; the GPU reads its own until the range is published.
      [buffer didModifyRange:NSMakeRange(static_cast<NSUInteger>(offsetBytes),
                                         static_cast<NSUInteger>(data.size()))];
      ++impl_->hostWritePublishCount;
    }
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

Status MetalDevice::Impl::beginEncodedRenderPass(EncodingState& state,
                                                 const BeginRenderPassCommand& beginPass) {
  MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  const auto& attachments = beginPass.descriptor.colorAttachments;
  for (size_t i = 0; i < attachments.size(); ++i) {
    const RenderPassColorAttachment& attachment = attachments[i];
    const std::optional<uint32_t> textureSlot =
        GetSlot(textureViewToTexture, attachment.view.slotIndex());
    id<MTLTexture> texture = textureSlot.has_value() ? GetSlot(textures, *textureSlot) : nil;
    if (texture == nil) {
      return GpuError{
          GpuErrorType::InvalidState,
          std::format("render pass attachment {} does not resolve to a Metal texture", i)};
    }

    MTLRenderPassColorAttachmentDescriptor* colorAttachment = passDescriptor.colorAttachments[i];
    colorAttachment.texture = texture;
    colorAttachment.loadAction = ToMtlLoadAction(attachment.loadOp);
    colorAttachment.storeAction = ToMtlStoreAction(attachment.storeOp);
    colorAttachment.clearColor =
        MTLClearColorMake(attachment.clearColor[0], attachment.clearColor[1],
                          attachment.clearColor[2], attachment.clearColor[3]);
  }

  state.renderEncoder = [state.commandBuffer renderCommandEncoderWithDescriptor:passDescriptor];
  if (state.renderEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "Metal render command encoder creation failed"};
  }
  return OkStatus();
}

Status MetalDevice::Impl::encodeSetPipeline(EncodingState& state,
                                            const SetPipelineCommand& setPipeline) {
  const RenderPipelineRecord* pipeline =
      FindRecord(renderPipelines, setPipeline.pipelineId.slotIndex);
  if (state.renderEncoder == nil || pipeline == nullptr || pipeline->state == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  [state.renderEncoder setRenderPipelineState:pipeline->state];
  // Cull mode is encoder state in Metal; apply the pipeline's recorded mode at bind time.
  // Behavioral coverage requires winding-controlled geometry, which the solid-fill slice does
  // not exercise: it always uses CullMode::None.
  [state.renderEncoder
      setCullMode:(pipeline->cullMode == CullMode::Back ? MTLCullModeBack : MTLCullModeNone)];
  state.currentTopology = pipeline->topology;
  return OkStatus();
}

Status MetalDevice::Impl::encodeBufferBinding(EncodingState& state, const BindGroupEntry& entry,
                                              const BufferBinding& bufferBinding,
                                              ShaderStage visibility) {
  // Mirror of the MSL emitter's guard: buffer bindings must not reach the reserved
  // stage-in vertex buffer index (or exceed Metal's argument table).
  if (shader::MslBufferIndex(entry.binding) >= shader::kMslVertexBufferIndex) {
    return GpuError{GpuErrorType::Unsupported,
                    std::format("setBindGroup: binding {} maps to Metal buffer index {}, which "
                                "collides with or exceeds the reserved vertex buffer index {}",
                                entry.binding, shader::MslBufferIndex(entry.binding),
                                shader::kMslVertexBufferIndex)};
  }
  id<MTLBuffer> buffer = GetSlot(buffers, bufferBinding.buffer.slotIndex());
  if (buffer == nil) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("setBindGroup: binding {} does not resolve to a Metal buffer", entry.binding)};
  }
  if (state.renderEncoder != nil && HasAllFlags(visibility, ShaderStage::Vertex)) {
    [state.renderEncoder setVertexBuffer:buffer
                                  offset:bufferBinding.offsetBytes
                                 atIndex:shader::MslBufferIndex(entry.binding)];
  }
  if (state.renderEncoder != nil && HasAllFlags(visibility, ShaderStage::Fragment)) {
    [state.renderEncoder setFragmentBuffer:buffer
                                    offset:bufferBinding.offsetBytes
                                   atIndex:shader::MslBufferIndex(entry.binding)];
  }
  if (state.computeEncoder != nil && HasAllFlags(visibility, ShaderStage::Compute)) {
    [state.computeEncoder setBuffer:buffer
                             offset:bufferBinding.offsetBytes
                            atIndex:shader::MslBufferIndex(entry.binding)];
  }
  return OkStatus();
}

Status MetalDevice::Impl::encodeTextureBinding(EncodingState& state, const BindGroupEntry& entry,
                                               const TextureViewBinding& viewBinding,
                                               ShaderStage visibility) {
  const std::optional<uint32_t> textureSlot =
      GetSlot(textureViewToTexture, viewBinding.view.slotIndex());
  id<MTLTexture> texture = textureSlot.has_value() ? GetSlot(textures, *textureSlot) : nil;
  if (texture == nil) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("setBindGroup: binding {} does not resolve to a Metal texture", entry.binding)};
  }
  if (state.renderEncoder != nil && HasAllFlags(visibility, ShaderStage::Vertex)) {
    [state.renderEncoder setVertexTexture:texture atIndex:shader::MslTextureIndex(entry.binding)];
  }
  if (state.renderEncoder != nil && HasAllFlags(visibility, ShaderStage::Fragment)) {
    [state.renderEncoder setFragmentTexture:texture atIndex:shader::MslTextureIndex(entry.binding)];
  }
  if (state.computeEncoder != nil && HasAllFlags(visibility, ShaderStage::Compute)) {
    [state.computeEncoder setTexture:texture atIndex:shader::MslTextureIndex(entry.binding)];
  }
  return OkStatus();
}

Status MetalDevice::Impl::encodeSamplerBinding(EncodingState& state, const BindGroupEntry& entry,
                                               const SamplerBinding& samplerBinding,
                                               ShaderStage visibility) {
  id<MTLSamplerState> sampler = GetSlot(samplers, samplerBinding.sampler.slotIndex());
  if (sampler == nil) {
    return GpuError{
        GpuErrorType::InvalidState,
        std::format("setBindGroup: binding {} does not resolve to a Metal sampler", entry.binding)};
  }
  if (state.renderEncoder != nil && HasAllFlags(visibility, ShaderStage::Vertex)) {
    [state.renderEncoder setVertexSamplerState:sampler
                                       atIndex:shader::MslSamplerIndex(entry.binding)];
  }
  if (state.renderEncoder != nil && HasAllFlags(visibility, ShaderStage::Fragment)) {
    [state.renderEncoder setFragmentSamplerState:sampler
                                         atIndex:shader::MslSamplerIndex(entry.binding)];
  }
  if (state.computeEncoder != nil && HasAllFlags(visibility, ShaderStage::Compute)) {
    [state.computeEncoder setSamplerState:sampler atIndex:shader::MslSamplerIndex(entry.binding)];
  }
  return OkStatus();
}

Status MetalDevice::Impl::encodeBindGroupEntry(EncodingState& state,
                                               const BindGroupLayoutDescriptor& layout,
                                               const BindGroupEntry& entry) {
  ShaderStage visibility = ShaderStage::None;
  for (const BindGroupLayoutEntry& layoutEntry : layout.entries) {
    if (layoutEntry.binding == entry.binding) {
      visibility = layoutEntry.visibility;
      break;
    }
  }

  if (const auto* bufferBinding = std::get_if<BufferBinding>(&entry.resource)) {
    return encodeBufferBinding(state, entry, *bufferBinding, visibility);
  } else if (const auto* viewBinding = std::get_if<TextureViewBinding>(&entry.resource)) {
    return encodeTextureBinding(state, entry, *viewBinding, visibility);
  } else if (const auto* samplerBinding = std::get_if<SamplerBinding>(&entry.resource)) {
    return encodeSamplerBinding(state, entry, *samplerBinding, visibility);
  }
  return OkStatus();
}

Status MetalDevice::Impl::encodeSetBindGroup(EncodingState& state,
                                             const SetBindGroupCommand& setBindGroup) {
  if (setBindGroup.index != 0) {
    return GpuError{GpuErrorType::Unsupported,
                    "the Metal backend maps bind group 0 only in this slice (MslBindingMap.h)"};
  }
  const BindGroupRecord* bindGroup = FindRecord(bindGroups, setBindGroup.bindGroupId.slotIndex);
  if ((state.renderEncoder == nil && state.computeEncoder == nil) || bindGroup == nullptr) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setBindGroup: bind group slot {} is not encodable",
                                setBindGroup.bindGroupId.slotIndex)};
  }

  for (const BindGroupEntry& entry : bindGroup->descriptor.entries) {
    Status bindStatus = encodeBindGroupEntry(state, bindGroup->layout, entry);
    if (bindStatus.hasError()) {
      return bindStatus;
    }
  }
  return OkStatus();
}

Status MetalDevice::Impl::encodeSetVertexBuffer(EncodingState& state,
                                                const SetVertexBufferCommand& setVertexBuffer) {
  if (setVertexBuffer.slot != 0) {
    return GpuError{GpuErrorType::Unsupported,
                    "the Metal backend supports vertex buffer slot 0 only in this slice"};
  }
  id<MTLBuffer> buffer = GetSlot(buffers, setVertexBuffer.bufferId.slotIndex);
  if (state.renderEncoder == nil || buffer == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setVertexBuffer: buffer slot {} is not encodable",
                                setVertexBuffer.bufferId.slotIndex)};
  }
  [state.renderEncoder setVertexBuffer:buffer
                                offset:setVertexBuffer.offsetBytes
                               atIndex:shader::kMslVertexBufferIndex];
  return OkStatus();
}

Status MetalDevice::Impl::encodeSetScissorRect(EncodingState& state,
                                               const SetScissorRectCommand& setScissor) {
  if (state.renderEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "setScissorRect outside a render pass"};
  }
  [state.renderEncoder setScissorRect:MTLScissorRect{setScissor.x, setScissor.y, setScissor.width,
                                                     setScissor.height}];
  return OkStatus();
}

Status MetalDevice::Impl::encodeSetViewport(EncodingState& state,
                                            const SetViewportCommand& setViewport) {
  if (state.renderEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "setViewport outside a render pass"};
  }
  [state.renderEncoder
      setViewport:MTLViewport{setViewport.x, setViewport.y, setViewport.width, setViewport.height,
                              setViewport.minDepth, setViewport.maxDepth}];
  return OkStatus();
}

Status MetalDevice::Impl::encodeDraw(EncodingState& state, const DrawCommand& draw) {
  if (state.renderEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "draw outside a render pass"};
  }
  [state.renderEncoder drawPrimitives:ToMtlPrimitiveType(state.currentTopology)
                          vertexStart:draw.firstVertex
                          vertexCount:draw.vertexCount
                        instanceCount:draw.instanceCount
                         baseInstance:draw.firstInstance];
  return OkStatus();
}

Status MetalDevice::Impl::encodeEndRenderPass(EncodingState& state) {
  if (state.renderEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "endRenderPass without an active render pass"};
  }
  [state.renderEncoder endEncoding];
  state.renderEncoder = nil;
  return OkStatus();
}

Status MetalDevice::Impl::encodeCopyTextureToBuffer(EncodingState& state,
                                                    const CopyTextureToBufferCommand& copy) {
  if (state.renderEncoder != nil) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToBuffer inside a render pass"};
  }
  id<MTLTexture> texture = GetSlot(textures, copy.textureId.slotIndex);
  id<MTLBuffer> buffer = GetSlot(buffers, copy.bufferId.slotIndex);
  if (texture == nil || buffer == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    "copyTextureToBuffer: source texture or destination buffer is missing"};
  }
  id<MTLBlitCommandEncoder> blitEncoder = [state.commandBuffer blitCommandEncoder];
  if (blitEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "Metal blit command encoder creation failed"};
  }
  [blitEncoder copyFromTexture:texture
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(copy.copySize.width, copy.copySize.height, 1)
                      toBuffer:buffer
             destinationOffset:copy.layout.offsetBytes
        destinationBytesPerRow:copy.layout.bytesPerRow
      destinationBytesPerImage:static_cast<uint64_t>(copy.layout.bytesPerRow) *
                               copy.layout.rowsPerImage];
  [blitEncoder endEncoding];
  return OkStatus();
}

Status MetalDevice::Impl::encodeCopyTextureToTexture(EncodingState& state,
                                                     const CopyTextureToTextureCommand& copy) {
  if (state.renderEncoder != nil) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToTexture inside a render pass"};
  }
  if (state.computeEncoder != nil) {
    return GpuError{GpuErrorType::InvalidState, "copyTextureToTexture inside a compute pass"};
  }
  id<MTLTexture> source = GetSlot(textures, copy.textureSrcId.slotIndex);
  id<MTLTexture> destination = GetSlot(textures, copy.textureDstId.slotIndex);
  if (source == nil || destination == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    "copyTextureToTexture: source or destination texture is missing"};
  }
  id<MTLBlitCommandEncoder> blitEncoder = [state.commandBuffer blitCommandEncoder];
  if (blitEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "Metal blit command encoder creation failed"};
  }
  [blitEncoder
        copyFromTexture:source
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(copy.sourceOrigin.x, copy.sourceOrigin.y, 0)
             sourceSize:MTLSizeMake(copy.copySize.width, copy.copySize.height, 1)
              toTexture:destination
       destinationSlice:0
       destinationLevel:0
      destinationOrigin:MTLOriginMake(copy.destinationOrigin.x, copy.destinationOrigin.y, 0)];
  [blitEncoder endEncoding];
  return OkStatus();
}

std::optional<Status> MetalDevice::Impl::encodeRenderCommand(EncodingState& state,
                                                             const Command& command) {
  if (const auto* beginPass = std::get_if<BeginRenderPassCommand>(&command)) {
    return beginEncodedRenderPass(state, *beginPass);
  } else if (const auto* setPipeline = std::get_if<SetPipelineCommand>(&command)) {
    return encodeSetPipeline(state, *setPipeline);
  } else if (const auto* setBindGroup = std::get_if<SetBindGroupCommand>(&command)) {
    return encodeSetBindGroup(state, *setBindGroup);
  } else if (const auto* setVertexBuffer = std::get_if<SetVertexBufferCommand>(&command)) {
    return encodeSetVertexBuffer(state, *setVertexBuffer);
  } else if (const auto* setScissor = std::get_if<SetScissorRectCommand>(&command)) {
    return encodeSetScissorRect(state, *setScissor);
  } else if (const auto* setViewport = std::get_if<SetViewportCommand>(&command)) {
    return encodeSetViewport(state, *setViewport);
  } else if (const auto* draw = std::get_if<DrawCommand>(&command)) {
    return encodeDraw(state, *draw);
  } else if (std::get_if<EndRenderPassCommand>(&command) != nullptr) {
    return encodeEndRenderPass(state);
  }
  return std::nullopt;
}

Status MetalDevice::Impl::beginEncodedComputePass(EncodingState& state) {
  state.computeEncoder = [state.commandBuffer computeCommandEncoder];
  if (state.computeEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "Metal compute command encoder creation failed"};
  }
  return OkStatus();
}

Status MetalDevice::Impl::encodeSetComputePipeline(EncodingState& state,
                                                   const SetComputePipelineCommand& setPipeline) {
  const ComputePipelineRecord* pipeline =
      FindRecord(computePipelines, setPipeline.pipelineId.slotIndex);
  if (state.computeEncoder == nil || pipeline == nullptr || pipeline->state == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    std::format("setPipeline: compute pipeline slot {} is not encodable",
                                setPipeline.pipelineId.slotIndex)};
  }
  [state.computeEncoder setComputePipelineState:pipeline->state];
  state.currentWorkgroupSize = pipeline->workgroupSize;
  return OkStatus();
}

Status MetalDevice::Impl::encodeDispatchWorkgroups(EncodingState& state,
                                                   const DispatchWorkgroupsCommand& dispatch) {
  if (state.computeEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "dispatchWorkgroups outside a compute pass"};
  }
  const MTLSize threadgroups =
      MTLSizeMake(dispatch.workgroupCountX, dispatch.workgroupCountY, dispatch.workgroupCountZ);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(
      state.currentWorkgroupSize.x, state.currentWorkgroupSize.y, state.currentWorkgroupSize.z);
  [state.computeEncoder dispatchThreadgroups:threadgroups
                       threadsPerThreadgroup:threadsPerThreadgroup];
  return OkStatus();
}

Status MetalDevice::Impl::encodeEndComputePass(EncodingState& state) {
  if (state.computeEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState, "endComputePass without an active compute pass"};
  }
  [state.computeEncoder endEncoding];
  state.computeEncoder = nil;
  return OkStatus();
}

std::optional<Status> MetalDevice::Impl::encodeComputeCommand(EncodingState& state,
                                                              const Command& command) {
  if (std::get_if<BeginComputePassCommand>(&command) != nullptr) {
    return beginEncodedComputePass(state);
  } else if (const auto* setPipeline = std::get_if<SetComputePipelineCommand>(&command)) {
    return encodeSetComputePipeline(state, *setPipeline);
  } else if (const auto* dispatch = std::get_if<DispatchWorkgroupsCommand>(&command)) {
    return encodeDispatchWorkgroups(state, *dispatch);
  } else if (std::get_if<EndComputePassCommand>(&command) != nullptr) {
    return encodeEndComputePass(state);
  }
  return std::nullopt;
}

std::optional<Status> MetalDevice::Impl::encodeCopyCommand(EncodingState& state,
                                                           const Command& command) {
  if (const auto* copy = std::get_if<CopyTextureToBufferCommand>(&command)) {
    return encodeCopyTextureToBuffer(state, *copy);
  } else if (const auto* textureCopy = std::get_if<CopyTextureToTextureCommand>(&command)) {
    return encodeCopyTextureToTexture(state, *textureCopy);
  }
  return std::nullopt;
}

Status MetalDevice::Impl::encodeCommand(EncodingState& state, const Command& command) {
  if (std::optional<Status> status = encodeRenderCommand(state, command)) {
    return *status;
  }
  if (std::optional<Status> status = encodeComputeCommand(state, command)) {
    return *status;
  }
  if (std::optional<Status> status = encodeCopyCommand(state, command)) {
    return *status;
  }
  return OkStatus();
}

void MetalDevice::Impl::attachCompletionHandler(EncodingState& state, uint64_t submissionSerial) {
  std::shared_ptr<CompletionState> sharedState = completionState;
  [state.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
    if (completedBuffer.error != nil) {
      sharedState->hadError.store(true, std::memory_order_release);
      std::lock_guard<std::mutex> lock(sharedState->mutex);
      if (sharedState->errorMessage.empty()) {
        sharedState->errorMessage =
            DescribeNSError(completedBuffer.error, "Metal command buffer execution failed");
      }
    }

    // Monotonic max: handlers may complete out of order across command buffers.
    uint64_t previous = sharedState->completedSerial.load(std::memory_order_relaxed);
    while (previous < submissionSerial &&
           !sharedState->completedSerial.compare_exchange_weak(
               previous, submissionSerial, std::memory_order_release, std::memory_order_relaxed)) {}
  }];
}

Status MetalDevice::Impl::encodeHostCoherencySync(EncodingState& state) {
  if (!needsExplicitHostCoherency()) {
    return OkStatus();  // One copy addressed from both sides; nothing to publish.
  }

  id<MTLBlitCommandEncoder> blitEncoder = [state.commandBuffer blitCommandEncoder];
  if (blitEncoder == nil) {
    return GpuError{GpuErrorType::InvalidState,
                    "Metal blit command encoder creation failed for the host-coherency sync"};
  }
  for (id<MTLBuffer> buffer : buffers) {
    if (buffer != nil) {
      [blitEncoder synchronizeResource:buffer];
    }
  }
  for (id<MTLTexture> texture : textures) {
    if (texture != nil) {
      [blitEncoder synchronizeResource:texture];
    }
  }
  [blitEncoder endEncoding];
  ++deviceWritePublishCount;
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

  Impl::EncodingState state;
  state.commandBuffer = [impl_->commandQueue commandBuffer];
  if (state.commandBuffer == nil) {
    return GpuError{GpuErrorType::InvalidState, "Metal command buffer creation failed"};
  }

  // On any encoding failure, close an open encoder before returning so the un-committed command
  // buffer tears down cleanly, then fail closed.
  const auto failEncoding = [&state](Status error) -> Status {
    if (state.renderEncoder != nil) {
      [state.renderEncoder endEncoding];
      state.renderEncoder = nil;
    }
    if (state.computeEncoder != nil) {
      [state.computeEncoder endEncoding];
      state.computeEncoder = nil;
    }
    return error;
  };

  for (const Command& command : commands) {
    Status encodeStatus = impl_->encodeCommand(state, command);
    if (encodeStatus.hasError()) {
      return failEncoding(std::move(encodeStatus));
    }
  }

  if (state.renderEncoder != nil || state.computeEncoder != nil) {
    // The encoder state machine guarantees passes are ended before finish; fail closed anyway.
    return failEncoding(
        GpuError{GpuErrorType::InvalidState, "submitted command stream left a pass open"});
  }

  // Publish every host-visible resource's GPU-side changes before the buffer completes, so a
  // host read after the completion sees them. Only a device without unified memory needs this,
  // and there it has to happen inside the submission: once the command buffer has completed
  // there is no encoder left to do it with. It covers every live resource rather than tracking
  // which this stream touched - the cost falls only on the path that already needs the copy, and
  // a missed resource here is silently wrong data rather than a reported failure.
  if (Status status = impl_->encodeHostCoherencySync(state); status.hasError()) {
    return failEncoding(std::move(status));
  }

  impl_->attachCompletionHandler(state, submissionSerial);
  [state.commandBuffer commit];

  return OkStatus();
}

}  // namespace donner::gpu::metal
