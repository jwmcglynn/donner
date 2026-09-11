#include "donner/svg/renderer/geode/GeodeFilterEngine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/SmallVector.h"
#include "donner/base/Utils.h"
#include "donner/gpu/CommandEncoder.h"
#include "donner/gpu/shader/programs/BlendBindings.h"
#include "donner/gpu/shader/programs/ColorSpaceConvertBindings.h"
#include "donner/gpu/shader/programs/ComponentTransferBindings.h"
#include "donner/gpu/shader/programs/CompositeBindings.h"
#include "donner/gpu/shader/programs/DropShadowBindings.h"
#include "donner/gpu/shader/programs/FilterColorMatrixBindings.h"
#include "donner/gpu/shader/programs/FloodBindings.h"
#include "donner/gpu/shader/programs/GaussianBlurBindings.h"
#include "donner/gpu/shader/programs/MergeBindings.h"
#include "donner/gpu/shader/programs/MorphologyBindings.h"
#include "donner/gpu/shader/programs/OffsetBindings.h"
#include "donner/gpu/shader/programs/SubregionClipBindings.h"
#include "donner/gpu/shader/programs/TileBindings.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/renderer/PixelFormatUtils.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeGpuContext.h"
#include "donner/svg/renderer/geode/GeodeShaders.h"
#include "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#include "embed_resources/ColorSpaceConvertWgsl.h"
#include "embed_resources/FilterBlendWgsl.h"
#include "embed_resources/FilterColorMatrixWgsl.h"
#include "embed_resources/FilterComponentTransferWgsl.h"
#include "embed_resources/FilterCompositeWgsl.h"
#include "embed_resources/FilterDropShadowWgsl.h"
#include "embed_resources/FilterMergeWgsl.h"
#include "embed_resources/FilterMorphologyWgsl.h"
#include "embed_resources/FilterResolveWgsl.h"
#include "embed_resources/FilterTileWgsl.h"
#include "embed_resources/FloodWgsl.h"
#include "embed_resources/GaussianBlurWgsl.h"
#include "embed_resources/OffsetWgsl.h"
#include "embed_resources/SubregionClipWgsl.h"

namespace donner::geode {

/// Reusable fixed-size parameter blocks. New blocks never depend on a previous arena's size,
/// so admission bounds remain valid when other filters consume slots before execution.
struct FilterResourceCache {
  std::mutex mutex;

  struct UniformSlot {
    wgpu::Buffer buffer;
    uint64_t offset = 0;
  };

  struct RuntimeParameterSlot {
    const gpu::Buffer* buffer = nullptr;
    uint64_t offset = 0;
  };

  struct UniformBlock {
    ScopedWgpuHandle<wgpu::Buffer> buffer;
    uint64_t size = 0;
  };
  struct RuntimeBlock {
    gpu::Buffer buffer;
    uint64_t size = 0;
  };

  UniformSlot acquireUniformSlot(GeodeDevice& device, size_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t aligned = align(size);
    if (uniformIndex < uniforms.size() && aligned > uniforms[uniformIndex].size - uniformCursor) {
      ++uniformIndex;
      uniformCursor = 0;
    }
    if (uniformIndex == uniforms.size()) {
      const uint64_t bytes = std::max(aligned, svg::components::kGpuFilterParameterBlockBytes);
      wgpu::BufferDescriptor desc = {};
      desc.label = wgpuLabel("FilterUniformScratch");
      desc.size = bytes;
      desc.usage =
          wgpu::BufferUsage::Uniform | wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
      ScopedWgpuHandle<wgpu::Buffer> buffer(device.device().createBuffer(desc));
      if (!buffer) {
        return {};
      }
      device.countBuffer();
      uniforms.push_back({std::move(buffer), bytes});
      retainedBytes += bytes;
    }
    UniformSlot result{uniforms[uniformIndex].buffer.get(), uniformCursor};
    uniformCursor += aligned;
    return result;
  }

  RuntimeParameterSlot acquireRuntimeParameterSlot(GeodeDevice& device, size_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    const uint64_t aligned = align(size);
    if (runtimeIndex < runtime.size() && aligned > runtime[runtimeIndex].size - runtimeCursor) {
      ++runtimeIndex;
      runtimeCursor = 0;
    }
    if (runtimeIndex == runtime.size()) {
      const uint64_t bytes = std::max(aligned, svg::components::kGpuFilterParameterBlockBytes);
      gpu::Result<gpu::Buffer> buffer = device.adapterDevice().createBuffer(gpu::BufferDescriptor{
          "FilterRuntimeParameterScratch", bytes,
          gpu::BufferUsage::Uniform | gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst});
      if (!buffer.hasResult()) {
        return {};
      }
      runtime.push_back({std::move(buffer).result(), bytes});
      retainedBytes += bytes;
    }
    RuntimeParameterSlot result{&runtime[runtimeIndex].buffer, runtimeCursor};
    runtimeCursor += aligned;
    return result;
  }

  void beginFrame() {
    std::lock_guard<std::mutex> lock(mutex);
    uniformIndex = runtimeIndex = 0;
    uniformCursor = runtimeCursor = 0;
  }

  uint64_t retainedBytes = 0;

private:
  static uint64_t align(size_t size) {
    // Every parameter payload is bounded below one block by the graph and primitive limits.
    UTILS_RELEASE_ASSERT(size <= svg::components::kGpuFilterParameterBlockBytes / 2);
    return (static_cast<uint64_t>(size) + kUniformOffsetAlignment - 1u) &
           ~(kUniformOffsetAlignment - 1u);
  }

  std::deque<UniformBlock> uniforms;
  std::deque<RuntimeBlock> runtime;
  size_t uniformIndex = 0;
  size_t runtimeIndex = 0;
  uint64_t uniformCursor = 0;
  uint64_t runtimeCursor = 0;
};

struct FilterResourceArena {
  FilterResourceArena(GeodeDevice& device, FilterTextureAllocator& textureAllocator,
                      FilterResourceCache& resourceCache,
                      ScopedWgpuHandle<wgpu::CommandEncoder>& commandEncoder,
                      size_t& passesInCommandBuffer)
      : device_(device),
        textureAllocator_(textureAllocator),
        resourceCache_(resourceCache),
        encoderSlot_(&commandEncoder),
        passesInCommandBuffer_(passesInCommandBuffer) {}
  ~FilterResourceArena() {
    for (ScopedWgpuHandle<wgpu::Buffer>& buffer : backendBuffers_) {
      device_.deferDestroy(buffer.take());
    }
    for (auto& owned : textures_) {
      textureAllocator_.releaseFilterTextureAtFrameEnd(std::move(owned.texture), owned.desc);
    }
  }

  FilterResourceArena(const FilterResourceArena&) = delete;
  FilterResourceArena& operator=(const FilterResourceArena&) = delete;

  /// Imports carry sRGB. A produced value records the space its primitive evaluated in.
  bool isLinearRgb(const wgpu::Texture& texture) const {
    return std::any_of(colorValues_.begin(), colorValues_.end(),
                       [&](const auto& value) { return value.linear == texture; });
  }

  void markLinearRgb(const wgpu::Texture& texture, bool linear) {
    if (linear && !isLinearRgb(texture)) {
      colorValues_.push_back({{}, texture});
    }
  }

  /// Reuses the requested representation when an earlier edge already converted this value.
  wgpu::Texture convertedTexture(const wgpu::Texture& input, bool linear) const {
    if (isLinearRgb(input) == linear) {
      return input;
    }
    for (const auto& value : colorValues_) {
      if (value.srgb == input || value.linear == input) {
        return linear ? value.linear : value.srgb;
      }
    }
    return {};
  }

  void rememberConversion(const wgpu::Texture& input, const wgpu::Texture& output, bool linear) {
    for (auto& value : colorValues_) {
      if (value.srgb == input || value.linear == input) {
        (linear ? value.linear : value.srgb) = output;
        return;
      }
    }
    colorValues_.push_back(linear ? ColorValue{input, output} : ColorValue{output, input});
  }

  /// Makes dead intermediates available after a whole node has finished recording.
  /// Both color representations of every retained logical value stay live.
  void retainValues(std::span<const wgpu::Texture> values) {
    const auto live = [&](const wgpu::Texture& texture) {
      return texture && std::find(values.begin(), values.end(), texture) != values.end();
    };
    for (OwnedTexture& owned : textures_) {
      const wgpu::Texture texture = device_.adapterDevice().wgpuTextureOf(owned.texture);
      const bool representationIsLive =
          std::any_of(colorValues_.begin(), colorValues_.end(), [&](const ColorValue& value) {
            return (value.srgb == texture || value.linear == texture) &&
                   (live(value.srgb) || live(value.linear));
          });
      // Queue uploads can precede earlier reads still recorded in the host encoder.
      owned.available = !gpu::HasAllFlags(owned.desc.usage, gpu::TextureUsage::CopyDst) &&
                        !live(texture) && !representationIsLive;
    }
  }

  /// Takes a filter intermediate from the renderer's pool, owned by this arena until the frame
  /// ends. Null when the pool refuses the allocation.
  ///
  /// The returned reference names storage this arena keeps for its whole lifetime, so a caller
  /// may hold it across further allocations.
  /// @param desc Descriptor the intermediate is allocated with.
  const gpu::Texture* createRuntimeTexture(const gpu::TextureDescriptor& desc) {
    for (OwnedTexture& owned : textures_) {
      if (owned.available && owned.desc.size == desc.size && owned.desc.format == desc.format &&
          owned.desc.usage == desc.usage && owned.desc.sampleCount == desc.sampleCount) {
        const wgpu::Texture texture = device_.adapterDevice().wgpuTextureOf(owned.texture);
        // The physical allocation now carries a new logical value.
        std::erase_if(colorValues_, [&](const ColorValue& value) {
          return value.srgb == texture || value.linear == texture;
        });
        owned.available = false;
        return &owned.texture;
      }
    }
    gpu::Texture texture = textureAllocator_.acquireFilterTexture(desc);
    if (!texture.isValid()) {
      return nullptr;
    }

    textureBytes +=
        uint64_t{desc.size.width} * desc.size.height * gpu::TextureFormatBytesPerTexel(desc.format);
    textures_.push_back({std::move(texture), desc});
    const gpu::Texture* result = &textures_.back().texture;
    runtimeNameByBackendTexture_.emplace_back(
        static_cast<WGPUTexture>(device_.adapterDevice().wgpuTextureOf(*result)), result);
    return result;
  }

  /// Takes a filter intermediate from the renderer's pool, owned by this arena until the frame
  /// ends.
  ///
  /// The arena's currency with the renderer is the runtime handle, which is what the pool
  /// allocates and what the frame-end release takes back. The backend texture is returned to the
  /// engine's own recording code, which still speaks wgpu directly; it borrows, and this arena
  /// owns.
  /// @param desc Descriptor the intermediate is allocated with.
  wgpu::Texture createTexture(const gpu::TextureDescriptor& desc) {
    const gpu::Texture* texture = createRuntimeTexture(desc);
    if (texture == nullptr) {
      return {};
    }
    return device_.adapterDevice().wgpuTextureOf(*texture);
  }

  /// Names \p texture for the runtime so a pass can bind it, owned by this arena until the frame
  /// ends. Null when the runtime refuses the handle.
  ///
  /// The engine threads intermediates between passes as backend textures, so a pass recorded
  /// through the runtime has to name its source rather than allocate it. The runtime borrows: the
  /// texture is still owned by whoever allocated it.
  /// @param texture Backend texture to name.
  const gpu::Texture* importRuntimeTexture(const wgpu::Texture& texture) {
    // One runtime name per backend texture: a filter graph feeds the same intermediate to several
    // passes, and an intermediate this arena allocated already has a name, so importing would give
    // one texture two live slots.
    const WGPUTexture key = static_cast<WGPUTexture>(texture);
    for (const auto& [backend, named] : runtimeNameByBackendTexture_) {
      if (backend == key) {
        return named;
      }
    }

    gpu::Result<gpu::Texture> imported = device_.adapterDevice().importExternalTexture(
        texture, gpu::Extent2d{texture.getWidth(), texture.getHeight()},
        GpuTextureFormatFromWgpu(texture.getFormat()), GpuTextureUsageFromWgpu(texture.getUsage()));
    if (!imported.hasResult()) {
      return nullptr;
    }
    importedTextures_.push_back(std::move(imported).result());
    const gpu::Texture* result = &importedTextures_.back();
    runtimeNameByBackendTexture_.emplace_back(key, result);
    return result;
  }

  /// A whole-texture view of \p texture, owned by this arena until the frame ends. Null when the
  /// runtime refuses it.
  /// @param texture Live runtime texture. @param label Debug label for the view.
  const gpu::TextureView* createRuntimeTextureView(const gpu::Texture& texture, RcString label) {
    // One view per named texture per frame, for the same reason the names themselves are reused.
    for (const auto& [named, view] : viewByTexture_) {
      if (named == &texture) {
        return view;
      }
    }

    gpu::Result<gpu::TextureView> view = device_.adapterDevice().createTextureView(
        texture, gpu::TextureViewDescriptor{std::move(label)});
    if (!view.hasResult()) {
      return nullptr;
    }
    textureViews_.push_back(std::move(view).result());
    const gpu::TextureView* result = &textureViews_.back();
    viewByTexture_.emplace_back(&texture, result);
    return result;
  }

  /// A parameter slot holding \p data for uniform or read-only storage binding. Null buffer when
  /// the scratch cannot be grown or the upload fails.
  ///
  /// A pass cannot share the scratch the unmigrated passes bump-allocate from, because the runtime
  /// cannot bind a backend buffer. It shares the runtime one instead: a buffer per pass would put
  /// an allocation on every filter primitive of every frame.
  /// @param data Bytes to upload.
  FilterResourceCache::RuntimeParameterSlot writeRuntimeParameterSlot(
      std::span<const uint8_t> data) {
    const FilterResourceCache::RuntimeParameterSlot slot =
        resourceCache_.acquireRuntimeParameterSlot(device_, data.size());
    if (slot.buffer == nullptr ||
        device_.adapterDevice().writeBuffer(*slot.buffer, slot.offset, data).hasError()) {
      return {};
    }
    return slot;
  }

  /// A bind group over \p entries, owned by this arena until the frame ends. Null when the
  /// runtime refuses it.
  /// @param layout Layout the entries must match. @param entries Bound resources.
  /// @param label Debug label for the bind group.
  const gpu::BindGroup* createRuntimeBindGroup(const gpu::BindGroupLayout& layout,
                                               std::vector<gpu::BindGroupEntry> entries,
                                               RcString label) {
    gpu::Result<gpu::BindGroup> bindGroup = device_.adapterDevice().createBindGroup(
        gpu::BindGroupDescriptor{std::move(label), layout, std::move(entries)});
    if (!bindGroup.hasResult()) {
      return nullptr;
    }
    // No count here: the runtime counts the bind group it creates, and counting again would
    // report every migrated pass as building two.
    bindGroups_.push_back(std::move(bindGroup).result());
    return &bindGroups_.back();
  }

  /// Records one compute dispatch through the runtime and replays it into the encoder the next
  /// pass belongs in, keeping it in order with the passes recorded there directly.
  ///
  /// @param label Debug label for the pass.
  /// @param pipeline Compute pipeline to dispatch.
  /// @param bindGroup Bind group for group 0.
  /// @param workgroupsX Workgroup count along x. @param workgroupsY Workgroup count along y.
  [[nodiscard]] bool dispatchComputePass(RcString label, const gpu::ComputePipeline& pipeline,
                                         const gpu::BindGroup& bindGroup, uint32_t workgroupsX,
                                         uint32_t workgroupsY) {
    // Counting the pass here, before anything is recorded, is what keeps a runtime pass under the
    // same command-buffer bound as the wgpu ones and re-points the runtime when that bound splits
    // the encoder.
    (void)commandEncoder();

    // The submit below replays into the installed host encoder, and this pass belongs in the
    // arena's own. A foreign one would put it in a command buffer nothing here submits, and no
    // host encoder at all would send it to the queue ahead of the passes recorded here before it.
    UTILS_RELEASE_ASSERT_MSG(
        *encoderSlot_ && device_.adapterDevice().hostCommandEncoderIs(encoderSlot_->get()),
        "filter pass replayed into a command encoder the arena does not own");

    gpu::Result<std::unique_ptr<gpu::CommandEncoder>> encoder =
        device_.adapterDevice().createCommandEncoder();
    if (!encoder.hasResult()) {
      return false;
    }
    gpu::CommandEncoder& commands = *encoder.result();

    gpu::Result<gpu::ComputePassEncoder*> pass =
        commands.beginComputePass(gpu::ComputePassDescriptor{std::move(label)});
    if (!pass.hasResult()) {
      return false;
    }
    gpu::ComputePassEncoder& computePass = *pass.result();
    if (computePass.setPipeline(pipeline).hasError() ||
        computePass.setBindGroup(0, bindGroup).hasError() ||
        computePass.dispatchWorkgroups(workgroupsX, workgroupsY, 1).hasError() ||
        computePass.end().hasError()) {
      return false;
    }

    gpu::Result<gpu::CommandBuffer> commandBuffer = commands.finish();
    if (!commandBuffer.hasResult()) {
      return false;
    }
    // Submitting here is what replays the pass: with a host encoder installed the runtime records
    // into it rather than reaching the queue, so the pass lands at this point in the frame's
    // command buffer instead of ahead of everything recorded before it.
    return !device_.adapterDevice().submit(std::move(commandBuffer).result()).hasError();
  }

  /// Copies through the runtime into the owning host encoder, preserving order with filter passes.
  bool copyTexture(const wgpu::Texture& source, const wgpu::Texture& destination,
                   gpu::Extent2d size, gpu::Origin2d sourceOrigin,
                   gpu::Origin2d destinationOrigin) {
    (void)commandEncoder();
    UTILS_RELEASE_ASSERT_MSG(
        *encoderSlot_ && device_.adapterDevice().hostCommandEncoderIs(encoderSlot_->get()),
        "filter copy replayed into a command encoder the arena does not own");
    const gpu::Texture* src = importRuntimeTexture(source);
    const gpu::Texture* dst = importRuntimeTexture(destination);
    if (!src || !dst) {
      return false;
    }
    auto encoder = device_.adapterDevice().createCommandEncoder();
    if (!encoder.hasResult() ||
        encoder.result()
            ->copyTextureToTexture(*src, *dst, size, sourceOrigin, destinationOrigin)
            .hasError()) {
      return false;
    }
    auto commands = encoder.result()->finish();
    return commands.hasResult() &&
           !device_.adapterDevice().submit(std::move(commands).result()).hasError();
  }

  /// A rewritten tile source is a new logical value even though its storage is unchanged.
  void resetLogicalValues() {
    colorValues_.clear();
    retainValues({});
  }

  FilterExecutionMemory memory() const { return {textureBytes, standaloneBufferBytes, 0}; }

  wgpu::Buffer createBuffer(const wgpu::Device& device, const wgpu::BufferDescriptor& desc) {
    ScopedWgpuHandle<wgpu::Buffer> buffer(device.createBuffer(desc));
    if (!buffer) {
      return {};
    }

    device_.countBuffer();
    wgpu::Buffer result = buffer.get();
    standaloneBufferBytes += desc.size;
    backendBuffers_.push_back(std::move(buffer));
    return result;
  }

  /// Maximum compute/render passes recorded into one shared command buffer
  /// before the arena finishes + submits it and starts a fresh encoder on the
  /// same slot. The shared-encoder design keeps small filters at two queue
  /// submissions per frame; pathological filter graphs must not grow a single
  /// command buffer without bound. For example, an feMorphology with a
  /// 24,998-device-pixel radius decomposes into 1,614 passes, and wgpu-native
  /// v24's Metal completion path has been observed to stall on a single
  /// command buffer of that size. Chunked submits preserve program order, so
  /// passes recorded after the chunk still execute after the earlier chunk.
  /// The pass count lives on the engine and is reset once per frame, so the
  /// bound covers the whole frame's command buffer even when a document runs
  /// many filter graphs (one arena per execute call).
  static constexpr size_t kMaxPassesPerCommandBuffer = 64;

  /// Returns the encoder to record the NEXT pass into, counting one pass per
  /// call. Call it immediately before beginning each pass, and never cache
  /// the returned reference across another commandEncoder() call: a later
  /// call may chunk (submit + replace) the encoder in the slot.
  wgpu::CommandEncoder& commandEncoder() {
    if (passesInCommandBuffer_ >= kMaxPassesPerCommandBuffer) {
      chunkCommandBuffer();
    }
    ++passesInCommandBuffer_;
    return encoderSlot_->get();
  }

  /// Finish the current chunk, submit it to the queue, and replace the
  /// encoder slot with a fresh command encoder. Callers holding their own
  /// handle copies of the old encoder (for example a finished GeoEncoder)
  /// keep it alive through reference counting, but it must not be used to
  /// record again: it has already been submitted.
  void chunkCommandBuffer() {
    passesInCommandBuffer_ = 0;
    if (!*encoderSlot_) {
      return;
    }
    // Identity, not "some host encoder is installed": the report and the re-point below describe
    // this encoder, and claiming them for one the runtime is not replaying into would retire work
    // that has not reached the queue.
    const bool replayingIntoThisEncoder =
        device_.adapterDevice().hostCommandEncoderIs(encoderSlot_->get());
    {
      ScopedWgpuHandle<wgpu::CommandBuffer> cmd(encoderSlot_->get().finish());
      if (cmd) {
        device_.queue().submit(1, &cmd.get());
        device_.countSubmit();
        device_.adapterDevice().notifyHostSubmitted(encoderSlot_->get());
      } else {
        // Nothing recorded into this encoder can reach the queue now, so the frame's output is
        // undefined and the pending serials must not be reported as submitted. Declaring the loss
        // is what makes that observable and what stops later waits, rather than continuing as if
        // the chunk had been submitted.
        device_.markDeviceLost("filter chunk command encoder could not be finished");
      }
    }
    // A chunk boundary is a cross-submit edge inside one filter graph: pass
    // N writes a storage texture in the submitted buffer while pass N+1
    // samples it from the next buffer. On hardware Vulkan the automatic
    // cross-submit storage-write to sampled-read barrier races the async
    // queue and produces nondeterministic large-area filter corruption, so
    // force the submitted work to complete before recording continues. The
    // in-buffer path (no chunking) relies on WebGPU's implicit inter-pass
    // barriers and needs no wait.
    //
    // The wait is bounded: a driver hang here would otherwise block the
    // rendering thread forever mid-frame. On timeout the device is declared
    // lost and recording continues without the barrier; output for the
    // frame is undefined, but the caller can observe the loss and tear the
    // renderer down without further blocking.
    if (device_.isVulkan()) {
      device_.waitForQueueIdle();
    }
    wgpu::CommandEncoderDescriptor desc = {};
    desc.label = wgpuLabel("GeodeFilterChunkCE");
    encoderSlot_->reset(device_.device().createCommandEncoder(desc));
    if (replayingIntoThisEncoder) {
      device_.adapterDevice().setHostCommandEncoder(encoderSlot_->get());
    }
  }

private:
  struct OwnedTexture {
    gpu::Texture texture;
    gpu::TextureDescriptor desc;
    bool available = false;
  };

  GeodeDevice& device_;
  FilterTextureAllocator& textureAllocator_;
  FilterResourceCache& resourceCache_;
  ScopedWgpuHandle<wgpu::CommandEncoder>* encoderSlot_;
  /// Engine-owned frame-scoped pass count (see kMaxPassesPerCommandBuffer).
  size_t& passesInCommandBuffer_;
  /// Deques rather than vectors: the accessors above hand out references into these, and a
  /// vector would move them on growth.
  std::deque<OwnedTexture> textures_;
  std::deque<gpu::Texture> importedTextures_;
  std::deque<gpu::TextureView> textureViews_;
  std::deque<gpu::BindGroup> bindGroups_;
  /// Flat rather than hashed: a filter graph names a handful of textures, so a linear scan is
  /// both smaller and faster than a hash table over that many entries.
  std::vector<std::pair<WGPUTexture, const gpu::Texture*>> runtimeNameByBackendTexture_;
  std::vector<std::pair<const gpu::Texture*, const gpu::TextureView*>> viewByTexture_;
  uint64_t textureBytes = 0;
  uint64_t standaloneBufferBytes = 0;
  std::vector<ScopedWgpuHandle<wgpu::Buffer>> backendBuffers_;
  struct ColorValue {
    wgpu::Texture srgb;
    wgpu::Texture linear;
  };
  std::vector<ColorValue> colorValues_;
};

namespace {

constexpr wgpu::TextureFormat kFormat = wgpu::TextureFormat::RGBA32Float;

double boundedPositiveFilterPixels(double value) {
  if (!std::isfinite(value) || value <= 0.0) {
    return 0.0;
  }
  return std::min(value, static_cast<double>(svg::components::kMaximumFilterPixelRadius));
}

double boundedSignedFilterPixels(double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, -static_cast<double>(svg::components::kMaximumFilterPixelOffset),
                    static_cast<double>(svg::components::kMaximumFilterPixelOffset));
}

double boundedTurbulenceFrequency(double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, -svg::components::kMaximumFilterTurbulenceFrequency,
                    svg::components::kMaximumFilterTurbulenceFrequency);
}

double boundedTurbulenceSeed(double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, -svg::components::kMaximumFilterTurbulenceSeed,
                    svg::components::kMaximumFilterTurbulenceSeed);
}

int32_t boundedRoundedInt32(double value, int32_t minimum, int32_t maximum) {
  if (std::isnan(value)) {
    return 0;
  }
  if (value <= static_cast<double>(minimum)) {
    return minimum;
  }
  if (value >= static_cast<double>(maximum)) {
    return maximum;
  }
  return static_cast<int32_t>(std::round(value));
}

int32_t boundedFloorInt32(double value, int32_t minimum, int32_t maximum) {
  if (std::isnan(value)) {
    return minimum;
  }
  if (value <= static_cast<double>(minimum)) {
    return minimum;
  }
  if (value >= static_cast<double>(maximum)) {
    return maximum;
  }
  return static_cast<int32_t>(std::floor(value));
}

int32_t boundedCeilInt32(double value, int32_t minimum, int32_t maximum) {
  if (std::isnan(value)) {
    return minimum;
  }
  if (value <= static_cast<double>(minimum)) {
    return minimum;
  }
  if (value >= static_cast<double>(maximum)) {
    return maximum;
  }
  return static_cast<int32_t>(std::ceil(value));
}

/// Uniform buffer layout matching the WGSL `BlurParams` struct.
struct BlurParams {
  float stdDeviation;
  uint32_t axis;        // 0 = horizontal, 1 = vertical.
  uint32_t edgeMode;    // 0 = None, 1 = Duplicate, 2 = Wrap.
  uint32_t kernelType;  // 0 = Gaussian, 1 = Box.
  int32_t boxLeft;      // Box mode: samples on the negative side.
  int32_t boxRight;     // Box mode: samples on the positive side.
  // Optional output-space clip rectangle, applied on the final blur pass
  // so the per-primitive subregion clip pass can be folded into the blur.
  // `clipActive == 0` disables the check. Semantics match the identity
  // subregion-clip shader: pixels with coord < clipMin or coord >= clipMax
  // are zeroed.
  int32_t clipMinX;
  int32_t clipMinY;
  int32_t clipMaxX;
  int32_t clipMaxY;
  uint32_t clipActive;
  uint32_t pad1;
};

/// Uniform buffer layout mirroring the shader program's `OffsetParams` struct. Host offsets
/// are rounded in double precision before narrowing to these exactly representable float pixels.
struct OffsetParams {
  float dx;       //!< Shift along x, in pixels.
  float dy;       //!< Shift along y, in pixels.
  uint32_t pad0;  //!< Trailing word the program declares; the two sizes must agree.
  uint32_t pad1;  //!< Trailing word the program declares; the two sizes must agree.
};

/// Uniform buffer layout mirroring the shader program's `FilterColorMatrixParams` struct.
/// 4x5 matrix stored as 5 column vectors (each vec4f = one column across
/// R'/G'/B'/A' rows).
struct FilterColorMatrixParams {
  float col0[4];  // multipliers for R input
  float col1[4];  // multipliers for G input
  float col2[4];  // multipliers for B input
  float col3[4];  // multipliers for A input
  float col4[4];  // constant offset
};

/// Uniform buffer layout matching the WGSL `FloodParams` struct.
struct FloodParams {
  float color[4];  // RGBA flood color in straight alpha.
};

/// Uniform buffer layout matching the typed program's `CompositeParams` struct.
struct CompositeParams {
  uint32_t op;  // Operator index (0..6).
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
  float k1;  // Arithmetic coefficient k1.
  float k2;  // Arithmetic coefficient k2.
  float k3;  // Arithmetic coefficient k3.
  float k4;  // Arithmetic coefficient k4.
};

static_assert(sizeof(CompositeParams) == 32);
static_assert(offsetof(CompositeParams, k1) == 16);

/// Encodes the SVG operator using the shader program's shared values.
/// @param op SVG compositing operator.
gpu::shader::programs::CompositeOperator ShaderCompositeOperator(
    svg::components::filter_primitive::Composite::Operator op) {
  using Op = svg::components::filter_primitive::Composite::Operator;
  using ShaderOp = gpu::shader::programs::CompositeOperator;
  switch (op) {
    case Op::Over: return ShaderOp::Over;
    case Op::In: return ShaderOp::In;
    case Op::Out: return ShaderOp::Out;
    case Op::Atop: return ShaderOp::Atop;
    case Op::Xor: return ShaderOp::Xor;
    case Op::Lighter: return ShaderOp::Lighter;
    case Op::Arithmetic: return ShaderOp::Arithmetic;
  }
  return ShaderOp::Over;
}

/// Uniform buffer layout matching the WGSL `BlendParams` struct.
struct BlendParams {
  uint32_t mode;  // Blend mode index (0..15).
  uint32_t pad0;
  uint32_t pad1;
  uint32_t pad2;
};

/// Uniform buffer layout matching the WGSL `MorphologyParams` struct.
struct MorphologyParams {
  int32_t radiusX;
  int32_t radiusY;
  uint32_t op;  // 0 = erode, 1 = dilate.
  uint32_t pad;
};

/// GPU storage buffer layout matching the WGSL `ConvolveParams` struct.
/// Uses storage (not uniform) because WGSL uniform array<f32,N> has 16-byte element stride.
struct ConvolveParams {
  int32_t orderX;
  int32_t orderY;
  int32_t targetX;
  int32_t targetY;
  float divisor;
  float bias;
  uint32_t edgeMode;
  uint32_t preserveAlpha;
  float kernel[25];  // Row-major kernel values (max 5×5).
};

/// GPU storage buffer layout matching the WGSL `TurbulenceParams` struct.
struct TurbulenceParams {
  float baseFreqX;
  float baseFreqY;
  int32_t numOctaves;
  int32_t seed;
  uint32_t stitchTiles;
  uint32_t typeFlag;
  float tileWidth;
  float tileHeight;
  float filterFromDeviceA;
  float filterFromDeviceB;
  float filterFromDeviceC;
  float filterFromDeviceD;
};

/// Pre-computed permutation + gradient tables for feTurbulence.
/// Matches the SVG spec's Perlin noise algorithm (Park-Miller LCG + Fisher-Yates shuffle).
/// Layout matches the WGSL `TurbulenceTables` struct.
constexpr int kTurbBLen = 256;
constexpr int kTurbBLenPlus2 = kTurbBLen + 2;               // 258
constexpr int kTurbTableSize = kTurbBLen + kTurbBLenPlus2;  // 514

struct TurbulenceTables {
  int32_t lattice[kTurbTableSize];  // Permutation table (514 entries).
  float gradX[4 * kTurbTableSize];  // Gradient X: [channel * 514 + index].
  float gradY[4 * kTurbTableSize];  // Gradient Y: [channel * 514 + index].
};

// Park-Miller LCG constants matching tiny-skia/resvg.
constexpr long kRandM = 2147483647;  // 2^31 - 1  // NOLINT
constexpr long kRandA = 16807;       // NOLINT
constexpr long kRandQ = 127773;      // NOLINT
constexpr long kRandR = 2836;        // NOLINT

/// Park-Miller LCG step (Schrage's method to avoid overflow).
long turbulenceRandom(long seed) {                                    // NOLINT
  long result = kRandA * (seed % kRandQ) - kRandR * (seed / kRandQ);  // NOLINT
  if (result <= 0) {
    result += kRandM;
  }
  return result;
}

/// sRGB → linearRGB transfer for a single channel in [0,1] (matches tiny-skia's
/// `srgbToLinearChannel`). Used to convert lighting-color uniforms into linear
/// space when a lighting primitive resolves to `color-interpolation-filters:
/// linearRGB`.
float srgbToLinearChannel(float c) {
  if (c <= 0.04045f) {
    return c / 12.92f;
  }
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// Generate permutation + gradient tables from seed, matching tiny-skia exactly.
void generateTurbulenceTables(double seedVal, TurbulenceTables& tables) {
  // Seed clamping matching resvg.
  seedVal = boundedTurbulenceSeed(seedVal);
  long seed;  // NOLINT
  if (seedVal <= 0) {
    seed = -(static_cast<long>(seedVal)) % (kRandM - 1) + 1;  // NOLINT
  } else if (seedVal > kRandM - 1) {
    seed = kRandM - 1;
  } else {
    seed = static_cast<long>(seedVal);  // NOLINT
  }

  // Temporary double-precision gradient storage during generation.
  double gradient[4][kTurbTableSize][2] = {};

  // Generate gradient tables for all 4 channels.
  for (int ch = 0; ch < 4; ch++) {
    for (int k = 0; k < kTurbBLen; k++) {
      if (ch == 0) {
        tables.lattice[k] = k;
      }

      seed = turbulenceRandom(seed);
      gradient[ch][k][0] =
          static_cast<double>((seed % (kTurbBLen + kTurbBLen)) - kTurbBLen) / kTurbBLen;
      seed = turbulenceRandom(seed);
      gradient[ch][k][1] =
          static_cast<double>((seed % (kTurbBLen + kTurbBLen)) - kTurbBLen) / kTurbBLen;

      // Normalize to unit length.
      const double mag = std::sqrt(gradient[ch][k][0] * gradient[ch][k][0] +
                                   gradient[ch][k][1] * gradient[ch][k][1]);
      if (mag > 1e-10) {
        gradient[ch][k][0] /= mag;
        gradient[ch][k][1] /= mag;
      }
    }
  }

  // Fisher-Yates shuffle of the lattice selector.
  for (int i = kTurbBLen - 1; i > 0; i--) {
    seed = turbulenceRandom(seed);
    const int target = static_cast<int>(seed % kTurbBLen);
    std::swap(tables.lattice[i], tables.lattice[target]);
  }

  // Duplicate entries for wrapping.
  for (int i = 0; i < kTurbBLenPlus2; i++) {
    tables.lattice[kTurbBLen + i] = tables.lattice[i];
    for (int ch = 0; ch < 4; ch++) {
      gradient[ch][kTurbBLen + i][0] = gradient[ch][i][0];
      gradient[ch][kTurbBLen + i][1] = gradient[ch][i][1];
    }
  }

  // Convert to float SOA layout matching WGSL struct.
  for (int ch = 0; ch < 4; ch++) {
    for (int idx = 0; idx < kTurbTableSize; idx++) {
      tables.gradX[ch * kTurbTableSize + idx] = static_cast<float>(gradient[ch][idx][0]);
      tables.gradY[ch * kTurbTableSize + idx] = static_cast<float>(gradient[ch][idx][1]);
    }
  }
}

/// Uniform buffer layout matching the WGSL `DisplacementParams` struct.
struct DisplacementParams {
  float scale;
  uint32_t xChannel;
  uint32_t yChannel;
  uint32_t pad;
};

/// GPU storage buffer layout matching the WGSL diffuse `LightingParams` struct.
struct DiffuseLightingParams {
  float surfaceScale;
  float diffuseConstant;
  float pad0;
  float pad1;

  float lightR;
  float lightG;
  float lightB;
  uint32_t lightType;

  float azimuthRad;
  float elevationRad;
  float lightX;
  float lightY;
  float lightZ;
  float userLightX;
  float userLightY;
  float userLightZ;

  float pointsAtX;
  float pointsAtY;
  float pointsAtZ;
  float spotExponent;

  float userPointsAtX;
  float userPointsAtY;
  float userPointsAtZ;
  float coneAngleRad;

  float pixelToUser0;
  float pixelToUser1;
  float pixelToUser2;
  float pixelToUser3;

  float pixelToUser4;
  float pixelToUser5;
  uint32_t hasShear;
  uint32_t hasConeAngle;
  int32_t sampleMinX;
  int32_t sampleMinY;
  int32_t sampleMaxX;
  int32_t sampleMaxY;
};

/// Uniform buffer layout matching the WGSL `DropShadowParams` struct.
struct DropShadowParams {
  float color[4];  // Flood color, straight alpha.
  float dx;
  float dy;
  uint32_t pad0;
  uint32_t pad1;
};

/// Uniform buffer layout matching the WGSL `ImageParams` struct.
/// Row-major 2×3 transform: src = M * (dst_pixel + 0.5, 1).
struct ImageParams {
  float m00;
  float m01;
  float m02;
  float m10;
  float m11;
  float m12;
  uint32_t samplingMode;  ///< 0 = smooth, 1 = crisp edges, 2 = pixelated two-stage.
  float pixelatedScaleX;
  float pixelatedScaleY;
  uint32_t pad1;
};

uint32_t ImageSamplingMode(svg::ImageRendering imageRendering) {
  switch (imageRendering) {
    case svg::ImageRendering::CrispEdges:
    case svg::ImageRendering::OptimizeSpeed: return 1u;
    case svg::ImageRendering::Pixelated: return 2u;
    case svg::ImageRendering::Auto:
    case svg::ImageRendering::Smooth:
    case svg::ImageRendering::HighQuality:
    case svg::ImageRendering::OptimizeQuality: return 0u;
  }
  return 0u;
}

/// Uniform buffer layout matching the WGSL `TileParams` struct.
struct TileParams {
  int32_t srcX;
  int32_t srcY;
  int32_t srcW;
  int32_t srcH;
};

/// Uniform buffer layout mirroring the shader program's `SubregionClipParams` struct: the inverse
/// transform that maps a pixel center back to user space, then the user-space rectangle to keep.
struct SubregionClipParams {
  float invA;     //!< Pixel-x coefficient of the user-space x.
  float invB;     //!< Pixel-x coefficient of the user-space y.
  float invC;     //!< Pixel-y coefficient of the user-space x.
  float invD;     //!< Pixel-y coefficient of the user-space y.
  float invE;     //!< Constant term of the user-space x.
  float invF;     //!< Constant term of the user-space y.
  float userX0;   //!< Low x edge, inclusive.
  float userY0;   //!< Low y edge, inclusive.
  float userX1;   //!< High x edge, exclusive.
  float userY1;   //!< High y edge, exclusive.
  uint32_t pad0;  //!< Trailing word the program declares; the two sizes must agree.
  uint32_t pad1;  //!< Trailing word the program declares; the two sizes must agree.
};

/// Uniform buffer layout for the sRGB↔linearRGB color space conversion shader.
/// Uniform buffer layout mirroring the shader program's `ColorSpaceConvertParams` struct.
struct ColorSpaceConvertParams {
  uint32_t direction;  //!< Which way the transfer runs; the program's bindings header names both.
  uint32_t pad0;       //!< Trailing word the program declares; the two sizes must agree.
  uint32_t pad1;       //!< Trailing word the program declares; the two sizes must agree.
  uint32_t pad2;       //!< Trailing word the program declares; the two sizes must agree.
};

/// GPU storage buffer layout matching the WGSL specular `LightingParams` struct.
struct SpecularLightingParams {
  float surfaceScale;
  float specularConstant;
  float specularExponent;
  float pad0;

  float lightR;
  float lightG;
  float lightB;
  uint32_t lightType;

  float azimuthRad;
  float elevationRad;
  float lightX;
  float lightY;
  float lightZ;
  float userLightX;
  float userLightY;
  float userLightZ;

  float pointsAtX;
  float pointsAtY;
  float pointsAtZ;
  float spotExponent;

  float userPointsAtX;
  float userPointsAtY;
  float userPointsAtZ;
  float coneAngleRad;

  float pixelToUser0;
  float pixelToUser1;
  float pixelToUser2;
  float pixelToUser3;

  float pixelToUser4;
  float pixelToUser5;
  uint32_t hasShear;
  uint32_t hasConeAngle;
  int32_t sampleMinX;
  int32_t sampleMinY;
  int32_t sampleMaxX;
  int32_t sampleMaxY;
};

/// Map a FilterGraph EdgeMode to the shader's uint.
uint32_t toShaderEdgeMode(svg::components::filter_primitive::GaussianBlur::EdgeMode mode) {
  using EM = svg::components::filter_primitive::GaussianBlur::EdgeMode;
  switch (mode) {
    case EM::None: return 0;
    case EM::Duplicate: return 1;
    case EM::Wrap: return 2;
  }
  return 0;
}

/// Create a texture usable as both a compute output (storage) and a
/// subsequent compute / render input (texture binding).
wgpu::Texture createIntermediateTexture(FilterResourceArena& arena, const wgpu::Device&,
                                        uint32_t width, uint32_t height, const char* label) {
  return arena.createTexture(gpu::TextureDescriptor{
      RcString(label), gpu::Extent2d{width, height}, gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
}

/// Create and explicitly clear an intermediate texture to transparent black.
///
/// Newly-created WebGPU textures read as zero, but a pooled texture retains its previous contents.
/// Filter primitives that short-circuit to transparent output must therefore record a clear before
/// returning the texture.
wgpu::Texture createTransparentIntermediateTexture(FilterResourceArena& arena, uint32_t width,
                                                   uint32_t height, const char* label) {
  wgpu::Texture texture = arena.createTexture(gpu::TextureDescriptor{
      RcString(label), gpu::Extent2d{width, height}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::StorageBinding | gpu::TextureUsage::CopySrc |
          gpu::TextureUsage::RenderAttachment});
  if (!texture) {
    return {};
  }

  ScopedWgpuHandle<wgpu::TextureView> view(texture.createView());
  wgpu::RenderPassColorAttachment color{};
  color.view = view.get();
  color.loadOp = wgpu::LoadOp::Clear;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.0, 0.0, 0.0, 0.0};
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

  wgpu::RenderPassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterTransparentClearPass");
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &color;
  ScopedWgpuHandle<wgpu::RenderPassEncoder> pass(arena.commandEncoder().beginRenderPass(passDesc));
  pass.get().end();

  return texture;
}

/// Helper to create a pipeline with a standard (input, output, uniform) bind group layout.
/// Used by blur, offset, and color-matrix pipelines.
struct InputOutputUniformPipeline {
  ScopedWgpuHandle<wgpu::BindGroupLayout> bindGroupLayout;
  ScopedWgpuHandle<wgpu::ComputePipeline> pipeline;
};

InputOutputUniformPipeline createInputOutputUniformPipeline(const wgpu::Device& dev,
                                                            const char* label,
                                                            wgpu::ShaderModule shaderModule,
                                                            size_t uniformSize) {
  ScopedWgpuHandle<wgpu::ShaderModule> shader(shaderModule);
  wgpu::BindGroupLayoutEntry entries[3]{};

  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[0].texture.multisampled = false;

  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  entries[1].storageTexture.format = kFormat;
  entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[2].buffer.minBindingSize = uniformSize;

  std::string bglLabel = std::string(label) + "BGL";
  wgpu::BindGroupLayoutDescriptor bglDesc{};
  bglDesc.label = wgpuLabel(bglLabel.c_str());
  bglDesc.entryCount = 3;
  bglDesc.entries = entries;
  ScopedWgpuHandle<wgpu::BindGroupLayout> bgl(dev.createBindGroupLayout(bglDesc));

  std::string plLabel = std::string(label) + "PipelineLayout";
  wgpu::PipelineLayoutDescriptor plDesc{};
  plDesc.label = wgpuLabel(plLabel.c_str());
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bgl.get()};
  plDesc.bindGroupLayouts = layouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(dev.createPipelineLayout(plDesc));

  std::string cpLabel = std::string(label) + "Pipeline";
  wgpu::ComputePipelineDescriptor cpDesc{};
  cpDesc.label = wgpuLabel(cpLabel.c_str());
  cpDesc.layout = pipelineLayout.get();
  cpDesc.compute.module = shader.get();
  cpDesc.compute.entryPoint = wgpuLabel("main");
  ScopedWgpuHandle<wgpu::ComputePipeline> pipeline(dev.createComputePipeline(cpDesc));

  return {std::move(bgl), std::move(pipeline)};
}

/// Helper to create a pipeline with a two-input (in1, in2, output, uniform) bind group layout.
/// Used by the remaining direct two-input filter pipelines.
struct TwoInputUniformPipeline {
  ScopedWgpuHandle<wgpu::BindGroupLayout> bindGroupLayout;
  ScopedWgpuHandle<wgpu::ComputePipeline> pipeline;
};

TwoInputUniformPipeline createTwoInputUniformPipeline(const wgpu::Device& dev, const char* label,
                                                      wgpu::ShaderModule shaderModule,
                                                      size_t uniformSize) {
  ScopedWgpuHandle<wgpu::ShaderModule> shader(shaderModule);
  wgpu::BindGroupLayoutEntry entries[4]{};

  // binding 0: in1 (texture_2d)
  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Compute;
  entries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[0].texture.multisampled = false;

  // binding 1: in2 (texture_2d)
  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Compute;
  entries[1].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
  entries[1].texture.viewDimension = wgpu::TextureViewDimension::_2D;
  entries[1].texture.multisampled = false;

  // binding 2: output (storage texture)
  entries[2].binding = 2;
  entries[2].visibility = wgpu::ShaderStage::Compute;
  entries[2].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
  entries[2].storageTexture.format = kFormat;
  entries[2].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

  // binding 3: uniform buffer
  entries[3].binding = 3;
  entries[3].visibility = wgpu::ShaderStage::Compute;
  entries[3].buffer.type = wgpu::BufferBindingType::Uniform;
  entries[3].buffer.minBindingSize = uniformSize;

  std::string bglLabel = std::string(label) + "BGL";
  wgpu::BindGroupLayoutDescriptor bglDesc{};
  bglDesc.label = wgpuLabel(bglLabel.c_str());
  bglDesc.entryCount = 4;
  bglDesc.entries = entries;
  ScopedWgpuHandle<wgpu::BindGroupLayout> bgl(dev.createBindGroupLayout(bglDesc));

  std::string plLabel = std::string(label) + "PipelineLayout";
  wgpu::PipelineLayoutDescriptor plDesc{};
  plDesc.label = wgpuLabel(plLabel.c_str());
  plDesc.bindGroupLayoutCount = 1;
  WGPUBindGroupLayout layouts[1] = {bgl.get()};
  plDesc.bindGroupLayouts = layouts;
  ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(dev.createPipelineLayout(plDesc));

  std::string cpLabel = std::string(label) + "Pipeline";
  wgpu::ComputePipelineDescriptor cpDesc{};
  cpDesc.label = wgpuLabel(cpLabel.c_str());
  cpDesc.layout = pipelineLayout.get();
  cpDesc.compute.module = shader.get();
  cpDesc.compute.entryPoint = wgpuLabel("main");
  ScopedWgpuHandle<wgpu::ComputePipeline> pipeline(dev.createComputePipeline(cpDesc));

  return {std::move(bgl), std::move(pipeline)};
}

/// Dispatch a compute shader with a two-input (in1, in2, output, uniform) bind group.
void dispatchTwoInputUniform(FilterResourceArena& arena, GeodeDevice& device,
                             const wgpu::BindGroupLayout& bgl,
                             const wgpu::ComputePipeline& pipeline, const wgpu::Texture& in1,
                             const wgpu::Texture& in2, const wgpu::Texture& output,
                             const wgpu::Buffer& uniformBuffer, uint64_t uniformOffset,
                             size_t uniformSize, const char* label) {
  const uint32_t width = output.getWidth();
  const uint32_t height = output.getHeight();

  ScopedWgpuHandle<wgpu::TextureView> in1View(in1.createView());
  ScopedWgpuHandle<wgpu::TextureView> in2View(in2.createView());
  ScopedWgpuHandle<wgpu::TextureView> outputView(output.createView());

  wgpu::BindGroupEntry bgEntries[4]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = in1View.get();
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = in2View.get();
  bgEntries[2].binding = 2;
  bgEntries[2].textureView = outputView.get();
  bgEntries[3].binding = 3;
  bgEntries[3].buffer = uniformBuffer;
  bgEntries[3].offset = uniformOffset;
  bgEntries[3].size = uniformSize;

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel(label);
  bgDesc.layout = bgl;
  bgDesc.entryCount = 4;
  bgDesc.entries = bgEntries;
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup(device.device().createBindGroup(bgDesc));
  device.countBindGroup();

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel(label);
  ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(
      arena.commandEncoder().beginComputePass(passDesc));
  pass.get().setPipeline(pipeline);
  pass.get().setBindGroup(0, bindGroup.get(), 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.get().dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.get().end();
  pass.reset();
}

/// Builds a compute pipeline from build-time emitted \p wgsl and \p layoutEntries. Returns a
/// program whose handles are all null when any step fails, so a caller checks the pipeline once
/// instead of each step.
///
/// @param runtime Device to create through.
/// @param name Debug label stem for the objects created.
/// @param wgsl Emitted source of the program.
/// @param entryPoint Name of the compute entry point the source declares.
/// @param layoutEntries Bind group 0 entries, matching what the program declares.
/// @param workgroupSize Size the entry point declares, along x and y.
RuntimeComputeProgram CreateRuntimeComputeProgram(
    gpu::Device& runtime, std::string_view name, std::string_view wgsl, std::string_view entryPoint,
    std::vector<gpu::BindGroupLayoutEntry> layoutEntries, uint32_t workgroupSize) {
  const RcString entryPointName{entryPoint};
  const gpu::WorkgroupSize workgroup{workgroupSize, workgroupSize, 1};

  gpu::Result<gpu::ShaderModule> shaderModule = runtime.createShaderModule(
      gpu::ShaderModuleDescriptor{RcString(name),
                                  RcString(wgsl),
                                  gpu::ShaderSourceKind::Wgsl,
                                  {},
                                  {gpu::ComputeEntryPointInfo{entryPointName, workgroup}}});
  if (!shaderModule.hasResult()) {
    return {};
  }
  gpu::Result<gpu::BindGroupLayout> bindGroupLayout = runtime.createBindGroupLayout(
      gpu::BindGroupLayoutDescriptor{RcString(name), std::move(layoutEntries)});
  if (!bindGroupLayout.hasResult()) {
    return {};
  }
  gpu::Result<gpu::PipelineLayout> pipelineLayout = runtime.createPipelineLayout(
      gpu::PipelineLayoutDescriptor{RcString(name), {bindGroupLayout.result()}});
  if (!pipelineLayout.hasResult()) {
    return {};
  }
  gpu::Result<gpu::ComputePipeline> pipeline =
      runtime.createComputePipeline(gpu::ComputePipelineDescriptor{
          RcString(name), pipelineLayout.result(),
          gpu::ComputeState{shaderModule.result(), entryPointName}, workgroup});
  if (!pipeline.hasResult()) {
    return {};
  }

  RuntimeComputeProgram program;
  program.shaderModule = std::move(shaderModule).result();
  program.bindGroupLayout = std::move(bindGroupLayout).result();
  program.pipelineLayout = std::move(pipelineLayout).result();
  program.pipeline = std::move(pipeline).result();
  return program;
}

/// The write-only storage-texture entry a filter program declares for its
/// destination. @param binding Binding index.
gpu::BindGroupLayoutEntry StorageOutputEntry(
    uint32_t binding, gpu::TextureFormat format = gpu::TextureFormat::RGBA32Float) {
  return gpu::BindGroupLayoutEntry{binding, gpu::ShaderStage::Compute,
                                   gpu::BindingType::WriteOnlyStorageTexture2d, format};
}

/// The sampled float texture entry a filter program declares for its source.
/// @param binding Binding index.
gpu::BindGroupLayoutEntry SampledInputEntry(uint32_t binding) {
  return gpu::BindGroupLayoutEntry{binding, gpu::ShaderStage::Compute,
                                   gpu::BindingType::SampledTexture2dUnfilterableFloat};
}

/// The uniform buffer entry a filter program declares for its parameters.
/// @param binding Binding index.
gpu::BindGroupLayoutEntry UniformParamsEntry(uint32_t binding) {
  return gpu::BindGroupLayoutEntry{binding, gpu::ShaderStage::Compute,
                                   gpu::BindingType::UniformBuffer};
}

/// The bytes of an embedded build-time artifact, as a string view.
/// @param resource Embedded span; the returned view aliases it and must not outlive it.
std::string_view EmbeddedWgsl(std::span<const unsigned char> resource UTILS_LIFETIME_BOUND) {
  return std::string_view(reinterpret_cast<const char*>(resource.data()), resource.size());
}

/// The bytes of \p value, for a uniform upload of a host struct.
/// @param value Host-side block; the returned span aliases it and must not outlive it.
template <typename T>
std::span<const uint8_t> UniformBytes(const T& value UTILS_LIFETIME_BOUND) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

/// Records one source-to-destination compute dispatch with a uniform block, through the runtime.
/// Returns false without recording anything when the pipeline was never built or any resource the
/// pass needs is refused.
///
/// @param arena Frame arena owning the resources and the encoder.
/// @param program Pipeline and layout to dispatch, built from the matching program.
/// @param input Source texture, borrowed. @param output Destination texture, borrowed.
/// @param uniforms Parameter block to upload. @param label Debug label stem for the pass.
/// @param workgroupSize Size the entry point declares, along x and y.
[[nodiscard]] bool dispatchRuntimeInputOutputParameters(
    FilterResourceArena& arena, const RuntimeComputeProgram& program, const wgpu::Texture& input,
    const gpu::Texture& output, std::span<const uint8_t> uniforms, const char* label,
    uint32_t workgroupSize, const gpu::Buffer* transferTable = nullptr) {
  if (!program.pipeline.isValid()) {
    return false;
  }

  const gpu::Texture* source = arena.importRuntimeTexture(input);
  if (source == nullptr) {
    return false;
  }
  const gpu::TextureView* sourceView = arena.createRuntimeTextureView(*source, RcString(label));
  const gpu::TextureView* destinationView = arena.createRuntimeTextureView(output, RcString(label));
  const FilterResourceCache::RuntimeParameterSlot uniformSlot =
      arena.writeRuntimeParameterSlot(uniforms);
  if (sourceView == nullptr || destinationView == nullptr || uniformSlot.buffer == nullptr) {
    return false;
  }

  std::vector<gpu::BindGroupEntry> entries(transferTable == nullptr ? 3 : 4);
  entries[0] = {0, gpu::TextureViewBinding{*sourceView}};
  entries[1] = {1, gpu::TextureViewBinding{*destinationView}};
  entries[2] = {2, gpu::BufferBinding{*uniformSlot.buffer, uniformSlot.offset, uniforms.size()}};
  if (transferTable != nullptr) {
    entries[3] = {3, gpu::BufferBinding{*transferTable, 0,
                                        sizeof(gpu::shader::programs::ColorTransferSamples())}};
  }
  const gpu::BindGroup* bindGroup =
      arena.createRuntimeBindGroup(program.bindGroupLayout, std::move(entries), RcString(label));
  if (bindGroup == nullptr) {
    return false;
  }

  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();
  return arena.dispatchComputePass(RcString(label), program.pipeline, *bindGroup,
                                   (width + workgroupSize - 1) / workgroupSize,
                                   (height + workgroupSize - 1) / workgroupSize);
}

/// Records a two-input pass, with an optional uniform block, through the runtime.
/// @param arena Frame resources and encoder. @param program Pipeline and matching layout.
/// @param source Source texture. @param destination Backdrop texture. @param output Result texture.
/// @param extent Output dimensions. @param uniforms Optional parameters, bound at index three.
/// @param label Debug label. @param workgroupSize Program's workgroup width and height.
[[nodiscard]] bool dispatchRuntimeTwoInput(
    FilterResourceArena& arena, const RuntimeComputeProgram& program, const wgpu::Texture& source,
    const wgpu::Texture& destination, const gpu::Texture& output, gpu::Extent2d extent,
    std::span<const uint8_t> uniforms, const char* label, uint32_t workgroupSize) {
  if (!program.pipeline.isValid()) {
    return false;
  }
  const gpu::Texture* runtimeSource = arena.importRuntimeTexture(source);
  const gpu::Texture* runtimeDestination = arena.importRuntimeTexture(destination);
  if (runtimeSource == nullptr || runtimeDestination == nullptr) {
    return false;
  }
  const gpu::TextureView* sourceView =
      arena.createRuntimeTextureView(*runtimeSource, RcString(label));
  const gpu::TextureView* destinationView =
      arena.createRuntimeTextureView(*runtimeDestination, RcString(label));
  const gpu::TextureView* outputView = arena.createRuntimeTextureView(output, RcString(label));
  if (sourceView == nullptr || destinationView == nullptr || outputView == nullptr) {
    return false;
  }
  std::vector<gpu::BindGroupEntry> entries{{0, gpu::TextureViewBinding{*sourceView}},
                                           {1, gpu::TextureViewBinding{*destinationView}},
                                           {2, gpu::TextureViewBinding{*outputView}}};
  if (!uniforms.empty()) {
    const FilterResourceCache::RuntimeParameterSlot slot =
        arena.writeRuntimeParameterSlot(uniforms);
    if (slot.buffer == nullptr) {
      return false;
    }
    entries.push_back({3, gpu::BufferBinding{*slot.buffer, slot.offset, uniforms.size()}});
  }
  const gpu::BindGroup* bindGroup =
      arena.createRuntimeBindGroup(program.bindGroupLayout, std::move(entries), RcString(label));
  return bindGroup != nullptr &&
         arena.dispatchComputePass(RcString(label), program.pipeline, *bindGroup,
                                   (extent.width + workgroupSize - 1) / workgroupSize,
                                   (extent.height + workgroupSize - 1) / workgroupSize);
}

/// Dispatch a compute shader with a standard (input, output, uniform) bind group.
void dispatchInputOutputUniform(FilterResourceArena& arena, GeodeDevice& device,
                                const wgpu::BindGroupLayout& bgl,
                                const wgpu::ComputePipeline& pipeline, const wgpu::Texture& input,
                                const wgpu::Texture& output, const wgpu::Buffer& uniformBuffer,
                                uint64_t uniformOffset, size_t uniformSize, const char* label) {
  const uint32_t width = output.getWidth();
  const uint32_t height = output.getHeight();

  ScopedWgpuHandle<wgpu::TextureView> inputView(input.createView());
  ScopedWgpuHandle<wgpu::TextureView> outputView(output.createView());

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inputView.get();
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outputView.get();
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = uniformBuffer;
  bgEntries[2].offset = uniformOffset;
  bgEntries[2].size = uniformSize;

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel(label);
  bgDesc.layout = bgl;
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup(device.device().createBindGroup(bgDesc));
  device.countBindGroup();

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel(label);
  ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(
      arena.commandEncoder().beginComputePass(passDesc));
  pass.get().setPipeline(pipeline);
  pass.get().setBindGroup(0, bindGroup.get(), 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.get().dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.get().end();
  pass.reset();
}

/// Bump-allocate a uniform slot from the engine's persistent per-frame
/// scratch buffer and upload the params into it. The returned (buffer,
/// offset) pair is stable across frames, which is what lets pass bind
/// groups be cached by the engine's per-frame bind-group cache.
FilterResourceCache::UniformSlot writeUniformSlot(FilterResourceCache& cache, GeodeDevice& device,
                                                  const void* data, size_t size) {
  FilterResourceCache::UniformSlot slot = cache.acquireUniformSlot(device, size);
  if (!slot.buffer) {
    return {};
  }
  device.queue().writeBuffer(slot.buffer, slot.offset, data, size);
  device.countBufferWrite(size);
  return slot;
}

/// The number of inputs a primitive reads through the filter attribute surface. feComposite,
/// feBlend and feDisplacementMap each take a second input (`in2`); feMerge is variadic and reads
/// exactly the list it was built with, and every other primitive reads at most one input. The cap
/// of two is deliberate rather than a lower bound: those primitives never sample a third input, so
/// an extra entry a caller appends must not widen the node's subregion either.
size_t filterInputSlotCount(const svg::components::FilterNode& node) {
  using namespace svg::components;
  if (std::holds_alternative<filter_primitive::Composite>(node.primitive) ||
      std::holds_alternative<filter_primitive::Blend>(node.primitive) ||
      std::holds_alternative<filter_primitive::DisplacementMap>(node.primitive)) {
    return 2;
  }
  return node.inputs.size();
}

/// The input in slot @p index, or the default for a slot the graph left empty. A slot the graph
/// never populated is an unspecified `in`/`in2` attribute, and both resolve the same way: to the
/// preceding primitive's result, which is SourceGraphic only when this node is the first
/// primitive in the filter. Falling back to the node's own first input instead would silently
/// composite, blend, or displace a buffer against itself.
svg::components::FilterInput filterInputOrDefault(const svg::components::FilterNode& node,
                                                  size_t index) {
  return index < node.inputs.size() ? node.inputs[index] : svg::components::FilterInput{};
}

/// Resolve an input reference to a texture.
wgpu::Texture resolveInput(const svg::components::FilterInput& input,
                           const std::unordered_map<std::string, wgpu::Texture>& namedBuffers,
                           const wgpu::Texture& currentBuffer, const wgpu::Texture& sourceGraphic,
                           const wgpu::Texture* sourceAlpha) {
  using namespace svg::components;
  if (const auto* named = std::get_if<FilterInput::Named>(&input.value)) {
    auto it = namedBuffers.find(named->name.str());
    if (it != namedBuffers.end()) {
      return it->second;
    }
  } else if (std::holds_alternative<FilterInput::Previous>(input.value)) {
    return currentBuffer;
  } else if (const auto* stdIn = std::get_if<FilterStandardInput>(&input.value)) {
    if (*stdIn == FilterStandardInput::SourceGraphic) {
      return sourceGraphic;
    }
    if (*stdIn == FilterStandardInput::SourceAlpha && sourceAlpha != nullptr) {
      return *sourceAlpha;
    }
    return sourceGraphic;
  }
  return currentBuffer;
}

bool graphUsesStandardInput(const svg::components::FilterGraph& filterGraph,
                            svg::components::FilterStandardInput input) {
  for (const svg::components::FilterNode& node : filterGraph.nodes) {
    for (const svg::components::FilterInput& nodeInput : node.inputs) {
      const auto* standardInput =
          std::get_if<svg::components::FilterStandardInput>(&nodeInput.value);
      if (standardInput != nullptr && *standardInput == input) {
        return true;
      }
    }
  }

  return false;
}

/// Returns true if the matrix represents an identity transform
/// (diagonal ones, zero offsets). Used to short-circuit the shader dispatch
/// and avoid the unpremultiply/premultiply round-trip precision loss that
/// tiny-skia also avoids (FilterGraph.cpp:462).
bool isIdentityColorMatrix(const FilterColorMatrixParams& params) {
  // clang-format off
  return params.col0[0] == 1.0f && params.col0[1] == 0.0f && params.col0[2] == 0.0f && params.col0[3] == 0.0f
      && params.col1[0] == 0.0f && params.col1[1] == 1.0f && params.col1[2] == 0.0f && params.col1[3] == 0.0f
      && params.col2[0] == 0.0f && params.col2[1] == 0.0f && params.col2[2] == 1.0f && params.col2[3] == 0.0f
      && params.col3[0] == 0.0f && params.col3[1] == 0.0f && params.col3[2] == 0.0f && params.col3[3] == 1.0f
      && params.col4[0] == 0.0f && params.col4[1] == 0.0f && params.col4[2] == 0.0f && params.col4[3] == 0.0f;
  // clang-format on
}

/// Build the 4x5 color matrix from feColorMatrix parameters.
/// All type variants are pre-computed here so the shader only needs a
/// single generic matrix multiply.
FilterColorMatrixParams buildColorMatrix(
    const svg::components::filter_primitive::ColorMatrix& primitive) {
  FilterColorMatrixParams params{};

  auto setIdentity = [&]() {
    // Identity: R'=R, G'=G, B'=B, A'=A, offsets=0.
    params.col0[0] = 1.0f;  // R' from R
    params.col1[1] = 1.0f;  // G' from G
    params.col2[2] = 1.0f;  // B' from B
    params.col3[3] = 1.0f;  // A' from A
  };

  using Type = svg::components::filter_primitive::ColorMatrix::Type;

  switch (primitive.type) {
    case Type::Matrix: {
      if (primitive.values.size() == 20) {
        // Row-major 4x5 → column-major 5×4.
        for (int row = 0; row < 4; ++row) {
          params.col0[row] = static_cast<float>(primitive.values[row * 5 + 0]);
          params.col1[row] = static_cast<float>(primitive.values[row * 5 + 1]);
          params.col2[row] = static_cast<float>(primitive.values[row * 5 + 2]);
          params.col3[row] = static_cast<float>(primitive.values[row * 5 + 3]);
          params.col4[row] = static_cast<float>(primitive.values[row * 5 + 4]);
        }
      } else {
        setIdentity();
      }
      break;
    }

    case Type::Saturate: {
      const float s = primitive.values.empty() ? 1.0f : static_cast<float>(primitive.values[0]);
      // SVG spec saturate matrix:
      //   | 0.213+0.787s  0.715-0.715s  0.072-0.072s  0  0 |
      //   | 0.213-0.213s  0.715+0.285s  0.072-0.072s  0  0 |
      //   | 0.213-0.213s  0.715-0.715s  0.072+0.928s  0  0 |
      //   | 0             0             0              1  0 |
      params.col0[0] = 0.213f + 0.787f * s;
      params.col0[1] = 0.213f - 0.213f * s;
      params.col0[2] = 0.213f - 0.213f * s;
      params.col0[3] = 0.0f;
      params.col1[0] = 0.715f - 0.715f * s;
      params.col1[1] = 0.715f + 0.285f * s;
      params.col1[2] = 0.715f - 0.715f * s;
      params.col1[3] = 0.0f;
      params.col2[0] = 0.072f - 0.072f * s;
      params.col2[1] = 0.072f - 0.072f * s;
      params.col2[2] = 0.072f + 0.928f * s;
      params.col2[3] = 0.0f;
      params.col3[3] = 1.0f;
      break;
    }

    case Type::HueRotate: {
      const double angleDeg = primitive.values.empty() ? 0.0 : primitive.values[0];
      const double rad = angleDeg * std::numbers::pi / 180.0;
      const float c = static_cast<float>(std::cos(rad));
      const float s = static_cast<float>(std::sin(rad));
      // SVG spec hueRotate matrix (from the spec table).
      params.col0[0] = 0.213f + 0.787f * c - 0.213f * s;
      params.col0[1] = 0.213f - 0.213f * c + 0.143f * s;
      params.col0[2] = 0.213f - 0.213f * c - 0.787f * s;
      params.col0[3] = 0.0f;
      params.col1[0] = 0.715f - 0.715f * c - 0.715f * s;
      params.col1[1] = 0.715f + 0.285f * c + 0.140f * s;
      params.col1[2] = 0.715f - 0.715f * c + 0.715f * s;
      params.col1[3] = 0.0f;
      params.col2[0] = 0.072f - 0.072f * c + 0.928f * s;
      params.col2[1] = 0.072f - 0.072f * c - 0.283f * s;
      params.col2[2] = 0.072f + 0.928f * c + 0.072f * s;
      params.col2[3] = 0.0f;
      params.col3[3] = 1.0f;
      break;
    }

    case Type::LuminanceToAlpha: {
      // R'=0, G'=0, B'=0, A'= 0.2126*R + 0.7152*G + 0.0722*B.
      params.col0[3] = 0.2126f;
      params.col1[3] = 0.7152f;
      params.col2[3] = 0.0722f;
      break;
    }
  }

  return params;
}

/// Map a ConvolveMatrix EdgeMode to the shader's uint.
uint32_t toConvolveEdgeMode(svg::components::filter_primitive::ConvolveMatrix::EdgeMode mode) {
  using EM = svg::components::filter_primitive::ConvolveMatrix::EdgeMode;
  switch (mode) {
    case EM::Duplicate: return 0;
    case EM::Wrap: return 1;
    case EM::None: return 2;
  }
  return 0;
}

constexpr size_t kComponentTransferHeaderWords = 4 * 8;
using ComponentTransferData = SmallVector<float, kComponentTransferHeaderWords + 1>;

/// Encodes four 32-byte function records followed by their unquantized table values.
/// Integer fields are exact f32 values: uint8_t kinds, counts <= 1024, and offsets <= 4096.
std::optional<ComponentTransferData> BuildComponentTransferData(
    const svg::components::filter_primitive::ComponentTransfer& primitive) {
  using Func = svg::components::filter_primitive::ComponentTransfer::Func;
  const std::array<const Func*, 4> functions{&primitive.funcR, &primitive.funcG, &primitive.funcB,
                                             &primitive.funcA};
  size_t tableCount = 0;
  for (const Func* function : functions) {
    if (function->tableValues.size() > svg::components::kMaximumFilterTableValues) {
      return std::nullopt;
    }
    tableCount += function->tableValues.size();
  }
  ComponentTransferData result;
  // The runtime storage array requires one element even when no channel uses a table.
  result.resize(kComponentTransferHeaderWords + std::max<size_t>(tableCount, 1));
  float* word = result.data();
  uint32_t offset = 0;
  for (const Func* function : functions) {
    *word++ = static_cast<float>(function->type);
    *word++ = static_cast<float>(offset);
    *word++ = static_cast<float>(function->tableValues.size());
    for (double coefficient : {function->slope, function->intercept, function->amplitude,
                               function->exponent, function->offset}) {
      *word++ = static_cast<float>(coefficient);
    }
    offset += static_cast<uint32_t>(function->tableValues.size());
  }
  for (const Func* function : functions) {
    for (double value : function->tableValues) {
      *word++ = static_cast<float>(value);
    }
  }
  return result;
}

}  // namespace

GeodeFilterEngine::GeodeFilterEngine(GeodeDevice& device, bool verbose)
    : device_(device), verbose_(verbose), resourceCache_(std::make_unique<FilterResourceCache>()) {
  const wgpu::Device& dev = device_.device();

  // Gaussian and box passes record into the shared GPU command stream.
  {
    using gpu::shader::programs::GaussianBlurBinding;
    blurProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "GaussianBlur", EmbeddedWgsl(donner::embedded::kGaussianBlurWgsl),
        gpu::shader::programs::kGaussianBlurEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(GaussianBlurBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(GaussianBlurBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(GaussianBlurBinding::Params))},
        gpu::shader::programs::kGaussianBlurWorkgroupSize);
  }

  // --- feOffset pipeline, through the GPU runtime ---
  {
    using gpu::shader::programs::OffsetBinding;
    offsetProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterOffset", EmbeddedWgsl(donner::embedded::kOffsetWgsl),
        gpu::shader::programs::kOffsetEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(OffsetBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(OffsetBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(OffsetBinding::Params))},
        gpu::shader::programs::kOffsetWorkgroupSize);
  }

  // --- feColorMatrix pipeline, through the GPU runtime ---
  {
    using gpu::shader::programs::FilterColorMatrixBinding;
    colorMatrixProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterColorMatrix",
        EmbeddedWgsl(donner::embedded::kFilterColorMatrixWgsl),
        gpu::shader::programs::kFilterColorMatrixEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(FilterColorMatrixBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(FilterColorMatrixBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(FilterColorMatrixBinding::Params))},
        gpu::shader::programs::kFilterColorMatrixWorkgroupSize);
  }

  // --- feFlood pipeline (output + uniform, no input), through the GPU runtime ---
  //
  // The WGSL below is build-time output of the shader IR program (see the genrules in this
  // package). Emitting it here instead would link the IR and the WGSL emitter into every binary
  // holding this engine, which the editor's WebAssembly package cannot afford for strings that
  // are identical on every run.
  {
    using gpu::shader::programs::FloodBinding;
    floodProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterFlood", EmbeddedWgsl(donner::embedded::kFloodWgsl),
        gpu::shader::programs::kFloodEntryPoint,
        {StorageOutputEntry(static_cast<uint32_t>(FloodBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(FloodBinding::Params))},
        gpu::shader::programs::kFloodWorkgroupSize);
  }

  {
    using gpu::shader::programs::MergeBinding;
    mergeProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterMerge", EmbeddedWgsl(donner::embedded::kFilterMergeWgsl),
        gpu::shader::programs::kMergeEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(MergeBinding::SourceTexture)),
         SampledInputEntry(static_cast<uint32_t>(MergeBinding::DestinationTexture)),
         StorageOutputEntry(static_cast<uint32_t>(MergeBinding::OutputTexture))},
        gpu::shader::programs::kMergeWorkgroupSize);
  }

  {
    using gpu::shader::programs::CompositeBinding;
    compositeProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterComposite",
        EmbeddedWgsl(donner::embedded::kFilterCompositeWgsl),
        gpu::shader::programs::kCompositeEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(CompositeBinding::SourceTexture)),
         SampledInputEntry(static_cast<uint32_t>(CompositeBinding::DestinationTexture)),
         StorageOutputEntry(static_cast<uint32_t>(CompositeBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(CompositeBinding::Params))},
        gpu::shader::programs::kCompositeWorkgroupSize);
  }

  // --- feBlend W3C blend-mode pipeline (two inputs + output + uniform) ---
  {
    using gpu::shader::programs::BlendBinding;
    blendProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterBlend", EmbeddedWgsl(donner::embedded::kFilterBlendWgsl),
        gpu::shader::programs::kBlendEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(BlendBinding::SourceTexture)),
         SampledInputEntry(static_cast<uint32_t>(BlendBinding::DestinationTexture)),
         StorageOutputEntry(static_cast<uint32_t>(BlendBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(BlendBinding::Params))},
        gpu::shader::programs::kBlendWorkgroupSize);
  }

  // Morphology shares the GPU command stream with the surrounding filter passes.
  {
    using gpu::shader::programs::MorphologyBinding;
    morphologyProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterMorphology",
        EmbeddedWgsl(donner::embedded::kFilterMorphologyWgsl),
        gpu::shader::programs::kMorphologyEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(MorphologyBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(MorphologyBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(MorphologyBinding::Params))},
        gpu::shader::programs::kMorphologyWorkgroupSize);
  }

  // Packed channel functions and tables use the same bounded runtime parameter pool.
  {
    using gpu::shader::programs::ComponentTransferBinding;
    componentTransferProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterComponentTransfer",
        EmbeddedWgsl(donner::embedded::kFilterComponentTransferWgsl),
        gpu::shader::programs::kComponentTransferEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(ComponentTransferBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(ComponentTransferBinding::OutputTexture)),
         {static_cast<uint32_t>(ComponentTransferBinding::Params), gpu::ShaderStage::Compute,
          gpu::BindingType::ReadOnlyStorageBuffer}},
        gpu::shader::programs::kComponentTransferWorkgroupSize);
  }

  // --- feConvolveMatrix pipeline (input + output + storage buffer for params) ---
  // Uses ReadOnlyStorage instead of Uniform because WGSL uniform arrays
  // have 16-byte element stride, making array<f32, 25> 400 bytes vs 100.
  {
    wgpu::BindGroupLayoutEntry entries[3]{};

    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
    entries[0].texture.multisampled = false;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[1].storageTexture.format = kFormat;
    entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[2].buffer.minBindingSize = sizeof(ConvolveParams);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterConvolveMatrixBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    convolveMatrixBindGroupLayout_.reset(dev.createBindGroupLayout(bglDesc));

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterConvolveMatrixPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {convolveMatrixBindGroupLayout_.get()};
    plDesc.bindGroupLayouts = layouts;
    ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(dev.createPipelineLayout(plDesc));
    ScopedWgpuHandle<wgpu::ShaderModule> shader(createFilterConvolveMatrixShader(dev));

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterConvolveMatrixPipeline");
    cpDesc.layout = pipelineLayout.get();
    cpDesc.compute.module = shader.get();
    cpDesc.compute.entryPoint = wgpuLabel("main");
    convolveMatrixPipeline_.reset(dev.createComputePipeline(cpDesc));
  }

  // --- feTurbulence pipeline (output + params buffer + tables buffer) ---
  {
    wgpu::BindGroupLayoutEntry entries[3]{};

    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[0].storageTexture.format = kFormat;
    entries[0].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[1].buffer.minBindingSize = sizeof(TurbulenceParams);

    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[2].buffer.minBindingSize = sizeof(TurbulenceTables);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterTurbulenceBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    turbulenceBindGroupLayout_.reset(dev.createBindGroupLayout(bglDesc));

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterTurbulencePipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {turbulenceBindGroupLayout_.get()};
    plDesc.bindGroupLayouts = layouts;
    ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(dev.createPipelineLayout(plDesc));
    ScopedWgpuHandle<wgpu::ShaderModule> shader(createFilterTurbulenceShader(dev));

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterTurbulencePipeline");
    cpDesc.layout = pipelineLayout.get();
    cpDesc.compute.module = shader.get();
    cpDesc.compute.entryPoint = wgpuLabel("main");
    turbulencePipeline_.reset(dev.createComputePipeline(cpDesc));
  }

  // --- feDisplacementMap pipeline (two inputs + output + uniform) ---
  {
    auto [bgl, pipeline] = createTwoInputUniformPipeline(dev, "FilterDisplacementMap",
                                                         createFilterDisplacementMapShader(dev),
                                                         sizeof(DisplacementParams));
    displacementMapBindGroupLayout_ = std::move(bgl);
    displacementMapPipeline_ = std::move(pipeline);
  }

  // --- feDiffuseLighting pipeline (input + output + storage buffer) ---
  {
    wgpu::BindGroupLayoutEntry entries[3]{};

    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
    entries[0].texture.multisampled = false;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[1].storageTexture.format = kFormat;
    entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[2].buffer.minBindingSize = sizeof(DiffuseLightingParams);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterDiffuseLightingBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    diffuseLightingBindGroupLayout_.reset(dev.createBindGroupLayout(bglDesc));

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterDiffuseLightingPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {diffuseLightingBindGroupLayout_.get()};
    plDesc.bindGroupLayouts = layouts;
    ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(dev.createPipelineLayout(plDesc));
    ScopedWgpuHandle<wgpu::ShaderModule> shader(createFilterDiffuseLightingShader(dev));

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterDiffuseLightingPipeline");
    cpDesc.layout = pipelineLayout.get();
    cpDesc.compute.module = shader.get();
    cpDesc.compute.entryPoint = wgpuLabel("main");
    diffuseLightingPipeline_.reset(dev.createComputePipeline(cpDesc));
  }

  // --- feSpecularLighting pipeline (input + output + storage buffer) ---
  {
    wgpu::BindGroupLayoutEntry entries[3]{};

    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Compute;
    entries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::_2D;
    entries[0].texture.multisampled = false;

    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Compute;
    entries[1].storageTexture.access = wgpu::StorageTextureAccess::WriteOnly;
    entries[1].storageTexture.format = kFormat;
    entries[1].storageTexture.viewDimension = wgpu::TextureViewDimension::_2D;

    entries[2].binding = 2;
    entries[2].visibility = wgpu::ShaderStage::Compute;
    entries[2].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    entries[2].buffer.minBindingSize = sizeof(SpecularLightingParams);

    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.label = wgpuLabel("FilterSpecularLightingBGL");
    bglDesc.entryCount = 3;
    bglDesc.entries = entries;
    specularLightingBindGroupLayout_.reset(dev.createBindGroupLayout(bglDesc));

    wgpu::PipelineLayoutDescriptor plDesc{};
    plDesc.label = wgpuLabel("FilterSpecularLightingPipelineLayout");
    plDesc.bindGroupLayoutCount = 1;
    WGPUBindGroupLayout layouts[1] = {specularLightingBindGroupLayout_.get()};
    plDesc.bindGroupLayouts = layouts;
    ScopedWgpuHandle<wgpu::PipelineLayout> pipelineLayout(dev.createPipelineLayout(plDesc));
    ScopedWgpuHandle<wgpu::ShaderModule> shader(createFilterSpecularLightingShader(dev));

    wgpu::ComputePipelineDescriptor cpDesc{};
    cpDesc.label = wgpuLabel("FilterSpecularLightingPipeline");
    cpDesc.layout = pipelineLayout.get();
    cpDesc.compute.module = shader.get();
    cpDesc.compute.entryPoint = wgpuLabel("main");
    specularLightingPipeline_.reset(dev.createComputePipeline(cpDesc));
  }

  // Shadow composition shares the runtime command stream with its blur passes.
  {
    using gpu::shader::programs::DropShadowBinding;
    dropShadowProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterDropShadow",
        EmbeddedWgsl(donner::embedded::kFilterDropShadowWgsl),
        gpu::shader::programs::kDropShadowEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(DropShadowBinding::SourceTexture)),
         SampledInputEntry(static_cast<uint32_t>(DropShadowBinding::BlurredTexture)),
         StorageOutputEntry(static_cast<uint32_t>(DropShadowBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(DropShadowBinding::Params))},
        gpu::shader::programs::kDropShadowWorkgroupSize);
  }

  // --- feImage placement pipeline (input + output + uniform) ---
  {
    auto [bgl, pipeline] = createInputOutputUniformPipeline(
        dev, "FilterImage", createFilterImageShader(dev), sizeof(ImageParams));
    imageBindGroupLayout_ = std::move(bgl);
    imagePipeline_ = std::move(pipeline);
  }

  // The tile program records through the shared GPU command stream.
  {
    using gpu::shader::programs::TileBinding;
    tileProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterTile", EmbeddedWgsl(donner::embedded::kFilterTileWgsl),
        gpu::shader::programs::kTileEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(TileBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(TileBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(TileBinding::Params))},
        gpu::shader::programs::kTileWorkgroupSize);
  }

  // --- Per-primitive subregion clipping pipeline, through the GPU runtime ---
  {
    using gpu::shader::programs::SubregionClipBinding;
    subregionClipProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterSubregionClip",
        EmbeddedWgsl(donner::embedded::kSubregionClipWgsl),
        gpu::shader::programs::kSubregionClipEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(SubregionClipBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(SubregionClipBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(SubregionClipBinding::Params))},
        gpu::shader::programs::kSubregionClipWorkgroupSize);
    filterResolveProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterResolve",
        EmbeddedWgsl(donner::embedded::kFilterResolveWgsl),
        gpu::shader::programs::kSubregionClipEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(SubregionClipBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(SubregionClipBinding::OutputTexture),
                            gpu::TextureFormat::RGBA8Unorm),
         UniformParamsEntry(static_cast<uint32_t>(SubregionClipBinding::Params)),
         {3, gpu::ShaderStage::Compute, gpu::BindingType::ReadOnlyStorageBuffer}},
        gpu::shader::programs::kSubregionClipWorkgroupSize);
  }

  // --- sRGB to linear color space conversion pipeline, through the GPU runtime ---
  {
    using gpu::shader::programs::ColorSpaceConvertBinding;
    colorSpaceConvertProgram_ = CreateRuntimeComputeProgram(
        device_.adapterDevice(), "FilterColorSpaceConvert",
        EmbeddedWgsl(donner::embedded::kColorSpaceConvertWgsl),
        gpu::shader::programs::kColorSpaceConvertEntryPoint,
        {SampledInputEntry(static_cast<uint32_t>(ColorSpaceConvertBinding::InputTexture)),
         StorageOutputEntry(static_cast<uint32_t>(ColorSpaceConvertBinding::OutputTexture)),
         UniformParamsEntry(static_cast<uint32_t>(ColorSpaceConvertBinding::Params)),
         {static_cast<uint32_t>(ColorSpaceConvertBinding::TransferTable), gpu::ShaderStage::Compute,
          gpu::BindingType::ReadOnlyStorageBuffer}},
        gpu::shader::programs::kColorSpaceConvertWorkgroupSize);
    const auto& samples = gpu::shader::programs::ColorTransferSamples();
    auto table = device_.adapterDevice().createBuffer(
        {"FilterColorTransferTable", sizeof(samples),
         gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst});
    if (table.hasResult()) {
      colorTransferTable_ = std::move(table).result();
      if (device_.adapterDevice()
              .writeBuffer(colorTransferTable_, 0, UniformBytes(samples))
              .hasError()) {
        colorTransferTable_ = {};
      }
    }
  }
}

GeodeFilterEngine::~GeodeFilterEngine() = default;

void GeodeFilterEngine::beginFrame() {
  framePassesInCommandBuffer_ = 0;
  if (resourceCache_) {
    resourceCache_->beginFrame();
  }
}

namespace {

double FilterDeviceScaleX(const Transform2d& transform) {
  return std::max(transform.transformVector(Vector2d(1.0, 0.0)).length(), 1e-12);
}

double FilterDeviceScaleY(const Transform2d& transform, double scaleX) {
  return NearZero(scaleX, 1e-12) ? std::abs(transform.data[3])
                                 : std::max(std::abs(transform.determinant()) / scaleX, 1e-12);
}

double FilterLengthInPixels(double value, double bound, double scale, bool objectBounds) {
  return std::abs(objectBounds ? value * bound : value) * scale;
}

Vector2d FilterOffsetInPixels(Vector2d offset, Vector2d bounds, bool objectBounds,
                              const Transform2d& transform) {
  const Vector2d userOffset =
      objectBounds ? Vector2d(offset.x * bounds.x, offset.y * bounds.y) : offset;
  return transform.transformVector(userOffset);
}

}  // namespace

struct FilterExecutionCoordinates {
  using FilterGraph = svg::components::FilterGraph;
  using FilterInput = svg::components::FilterInput;
  using FilterNode = svg::components::FilterNode;

  FilterExecutionCoordinates(const FilterGraph& graph, const wgpu::Texture& sourceGraphic,
                             const Box2d& filterRegion, const Transform2d& deviceFromFilter)
      : graph(graph),
        filterRegion(filterRegion),
        deviceFromFilter(deviceFromFilter),
        scaleX(FilterDeviceScaleX(deviceFromFilter)),
        scaleY(FilterDeviceScaleY(deviceFromFilter, scaleX)),
        isObjectBoundingBox(graph.primitiveUnits == svg::PrimitiveUnits::ObjectBoundingBox),
        boundingBoxWidth(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->width()
                                                              : 1.0),
        boundingBoxHeight(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->height()
                                                               : 1.0),
        boundingBoxX(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->topLeft.x
                                                          : 0.0),
        boundingBoxY(graph.elementBoundingBox.has_value() ? graph.elementBoundingBox->topLeft.y
                                                          : 0.0),
        filterFromDevice(deviceFromFilter.inverse()),
        primitiveUnitsBounds(computePrimitiveUnitsBounds(sourceGraphic)),
        previousOutputSubregion(filterRegion) {}

  double toPixelX(double value) const {
    return FilterLengthInPixels(value, boundingBoxWidth, scaleX, isObjectBoundingBox);
  }

  double toPixelY(double value) const {
    return FilterLengthInPixels(value, boundingBoxHeight, scaleY, isObjectBoundingBox);
  }

  Vector2d toPixelOffset(double dx, double dy) const {
    return FilterOffsetInPixels({dx, dy}, {boundingBoxWidth, boundingBoxHeight},
                                isObjectBoundingBox, deviceFromFilter);
  }

  Box2d resolveInputSubregion(const FilterInput& input) const {
    if (const auto* named = std::get_if<FilterInput::Named>(&input.value)) {
      auto it = namedSubregions.find(named->name.str());
      return it != namedSubregions.end() ? it->second : previousOutputSubregion;
    }
    return std::holds_alternative<FilterInput::Previous>(input.value) ? previousOutputSubregion
                                                                      : filterRegion;
  }

  Box2d computeNodeSubregion(const FilterNode& node) const {
    using namespace svg::components;
    const bool sourceGenerator =
        std::holds_alternative<filter_primitive::Flood>(node.primitive) ||
        std::holds_alternative<filter_primitive::Turbulence>(node.primitive) ||
        std::holds_alternative<filter_primitive::Image>(node.primitive) ||
        std::holds_alternative<filter_primitive::Tile>(node.primitive);
    const bool explicitSubregion = node.x.has_value() || node.y.has_value() ||
                                   node.width.has_value() || node.height.has_value();
    if (explicitSubregion) {
      return explicitNodeSubregion(node);
    }
    const size_t slots = filterInputSlotCount(node);
    if (sourceGenerator || slots == 0) {
      return filterRegion;
    }
    Box2d inputBounds = resolveInputSubregion(filterInputOrDefault(node, 0));
    for (size_t index = 1; index < slots; ++index) {
      inputBounds =
          Box2d::Union(inputBounds, resolveInputSubregion(filterInputOrDefault(node, index)));
    }
    expandPrimitiveBounds(node, inputBounds);
    return intersect(inputBounds, filterRegion);
  }

  double resolvePrimitivePosition(const Lengthd& length, Lengthd::Extent extent, double origin,
                                  double boundingBoxDimension) const {
    return resolvePosition(length, extent, origin, boundingBoxDimension);
  }

  double resolvePrimitiveSize(const Lengthd& length, Lengthd::Extent extent,
                              double boundingBoxDimension) const {
    return resolveSize(length, extent, boundingBoxDimension);
  }

  const FilterGraph& graph;
  Box2d filterRegion;
  const Transform2d& deviceFromFilter;
  double scaleX = 1.0;
  double scaleY = 1.0;
  bool isObjectBoundingBox = false;
  double boundingBoxWidth = 1.0;
  double boundingBoxHeight = 1.0;
  double boundingBoxX = 0.0;
  double boundingBoxY = 0.0;
  Transform2d filterFromDevice;
  Box2d primitiveUnitsBounds;
  Box2d previousOutputSubregion;
  std::unordered_map<std::string, Box2d> namedSubregions;

private:
  Box2d computePrimitiveUnitsBounds(const wgpu::Texture& sourceGraphic) const {
    if (isObjectBoundingBox) {
      return graph.elementBoundingBox.value_or(Box2d(Vector2d(0.0, 0.0), Vector2d(1.0, 1.0)));
    }
    const double userWidth = NearZero(scaleX, 1e-12)
                                 ? static_cast<double>(sourceGraphic.getWidth())
                                 : static_cast<double>(sourceGraphic.getWidth()) / scaleX;
    const double userHeight = NearZero(scaleY, 1e-12)
                                  ? static_cast<double>(sourceGraphic.getHeight())
                                  : static_cast<double>(sourceGraphic.getHeight()) / scaleY;
    return Box2d::FromXYWH(0.0, 0.0, userWidth, userHeight);
  }

  double resolvePosition(const Lengthd& length, Lengthd::Extent extent, double origin,
                         double boundingBoxDimension) const {
    if (isObjectBoundingBox && length.unit == Lengthd::Unit::None) {
      return origin + length.value * boundingBoxDimension;
    }
    if (length.unit == Lengthd::Unit::Percent) {
      const double referenceOrigin =
          isObjectBoundingBox ? (extent == Lengthd::Extent::X ? boundingBoxX : boundingBoxY) : 0.0;
      const double referenceSize =
          isObjectBoundingBox ? boundingBoxDimension
                              : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                              : primitiveUnitsBounds.height());
      return referenceOrigin + referenceSize * length.value / 100.0;
    }
    return length.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  }

  double resolveSize(const Lengthd& length, Lengthd::Extent extent,
                     double boundingBoxDimension) const {
    if (isObjectBoundingBox && length.unit == Lengthd::Unit::None) {
      return length.value * boundingBoxDimension;
    }
    if (length.unit == Lengthd::Unit::Percent) {
      const double referenceSize =
          isObjectBoundingBox ? boundingBoxDimension
                              : (extent == Lengthd::Extent::X ? primitiveUnitsBounds.width()
                                                              : primitiveUnitsBounds.height());
      return referenceSize * length.value / 100.0;
    }
    return length.toPixels(primitiveUnitsBounds, FontMetrics(), extent);
  }

  static Box2d intersect(const Box2d& first, const Box2d& second) {
    return Box2d(Vector2d(std::max(first.topLeft.x, second.topLeft.x),
                          std::max(first.topLeft.y, second.topLeft.y)),
                 Vector2d(std::min(first.bottomRight.x, second.bottomRight.x),
                          std::min(first.bottomRight.y, second.bottomRight.y)));
  }

  Box2d explicitNodeSubregion(const FilterNode& node) const {
    const double x = node.x.has_value() ? resolvePosition(*node.x, Lengthd::Extent::X,
                                                          isObjectBoundingBox ? boundingBoxX : 0.0,
                                                          boundingBoxWidth)
                                        : filterRegion.topLeft.x;
    const double y = node.y.has_value() ? resolvePosition(*node.y, Lengthd::Extent::Y,
                                                          isObjectBoundingBox ? boundingBoxY : 0.0,
                                                          boundingBoxHeight)
                                        : filterRegion.topLeft.y;
    const double width = node.width.has_value()
                             ? resolveSize(*node.width, Lengthd::Extent::X, boundingBoxWidth)
                             : filterRegion.width();
    const double height = node.height.has_value()
                              ? resolveSize(*node.height, Lengthd::Extent::Y, boundingBoxHeight)
                              : filterRegion.height();
    return intersect(Box2d(Vector2d(x, y), Vector2d(x + width, y + height)), filterRegion);
  }

  void expandPrimitiveBounds(const FilterNode& node, Box2d& inputBounds) const {
    using namespace svg::components;
    if (const auto* blur = std::get_if<filter_primitive::GaussianBlur>(&node.primitive)) {
      const double x = std::ceil((blur->stdDeviationX >= 0
                                      ? boundedPositiveFilterPixels(toPixelX(blur->stdDeviationX))
                                      : 0.0) *
                                 3.0);
      const double y = std::ceil((blur->stdDeviationY >= 0
                                      ? boundedPositiveFilterPixels(toPixelY(blur->stdDeviationY))
                                      : 0.0) *
                                 3.0);
      inputBounds = Box2d(Vector2d(inputBounds.topLeft.x - x, inputBounds.topLeft.y - y),
                          Vector2d(inputBounds.bottomRight.x + x, inputBounds.bottomRight.y + y));
      return;
    }
    if (const auto* drop = std::get_if<filter_primitive::DropShadow>(&node.primitive)) {
      expandDropShadow(*drop, inputBounds);
      return;
    }
    if (const auto* morphology = std::get_if<filter_primitive::Morphology>(&node.primitive);
        morphology && morphology->op == filter_primitive::Morphology::Operator::Dilate) {
      const double x = boundedPositiveFilterPixels(toPixelX(morphology->radiusX));
      const double y = boundedPositiveFilterPixels(toPixelY(morphology->radiusY));
      inputBounds = Box2d(Vector2d(inputBounds.topLeft.x - x, inputBounds.topLeft.y - y),
                          Vector2d(inputBounds.bottomRight.x + x, inputBounds.bottomRight.y + y));
    }
  }

  void expandDropShadow(const svg::components::filter_primitive::DropShadow& drop,
                        Box2d& inputBounds) const {
    const double x = std::ceil((drop.stdDeviationX >= 0
                                    ? boundedPositiveFilterPixels(toPixelX(drop.stdDeviationX))
                                    : 0.0) *
                               3.0);
    const double y = std::ceil((drop.stdDeviationY >= 0
                                    ? boundedPositiveFilterPixels(toPixelY(drop.stdDeviationY))
                                    : 0.0) *
                               3.0);
    const Vector2d offset = toPixelOffset(drop.dx, drop.dy);
    const Vector2d boundedOffset(boundedSignedFilterPixels(offset.x),
                                 boundedSignedFilterPixels(offset.y));
    const Box2d shadow(Vector2d(inputBounds.topLeft.x + boundedOffset.x - x,
                                inputBounds.topLeft.y + boundedOffset.y - y),
                       Vector2d(inputBounds.bottomRight.x + boundedOffset.x + x,
                                inputBounds.bottomRight.y + boundedOffset.y + y));
    inputBounds = Box2d::Union(inputBounds, shadow);
  }
};

namespace fp = svg::components::filter_primitive;

/// Values and coordinates retained while one filter graph executes.
struct FilterGraphExecution {
  FilterGraphExecution(GeodeFilterEngine& engine, const svg::components::FilterGraph& graph,
                       const wgpu::Texture& sourceGraphic, const Box2d& filterRegion,
                       const Transform2d& deviceFromFilter,
                       FilterTextureAllocator& textureAllocator,
                       ScopedWgpuHandle<wgpu::CommandEncoder>& commandEncoder,
                       svg::components::FilterExecutionBudget* executionBudget,
                       std::optional<FilterTilePlan> admittedPlan)
      : engine(engine),
        graph(graph),
        sourceGraphic(sourceGraphic),
        executionBudget(executionBudget),
        admittedPlan(admittedPlan),
        arena(engine.device_, textureAllocator, *engine.resourceCache_, commandEncoder,
              engine.framePassesInCommandBuffer_),
        coordinates(graph, sourceGraphic, filterRegion, deviceFromFilter),
        currentBuffer(sourceGraphic),
        axisAligned(NearZero(deviceFromFilter.data[1], 1e-6) &&
                    NearZero(deviceFromFilter.data[2], 1e-6)) {}

  wgpu::Texture run();
  wgpu::Texture runNodes();
  wgpu::Texture runTiled(const FilterTilePlan& plan);
  wgpu::Texture inputFor(const svg::components::FilterNode& node, size_t index) const;
  wgpu::Texture clip(const wgpu::Texture& input, const Box2d& region, bool transformed,
                     bool resolve = false);
  void record(const svg::components::FilterNode& node, const Box2d& subregion,
              const wgpu::Texture& output, size_t nodeIndex);
  void findLastNamedUses();
  void retainLiveValues(size_t nodeIndex);

  GeodeFilterEngine& engine;
  const svg::components::FilterGraph& graph;
  wgpu::Texture sourceGraphic;
  Vector2d tileOrigin = Vector2d::Zero();
  svg::components::FilterExecutionBudget* executionBudget;
  std::optional<FilterTilePlan> admittedPlan;
  FilterResourceArena arena;
  FilterExecutionCoordinates coordinates;
  wgpu::Texture currentBuffer;
  std::optional<wgpu::Texture> sourceAlpha;
  std::unordered_map<std::string, wgpu::Texture> namedBuffers;
  std::unordered_map<std::string, size_t> lastNamedUse;
  bool axisAligned;
  uint64_t executedTiles = 0;
  uint64_t admittedWorkUnits = 0;
};

/// The primitive visitor owns node-local conversion and clip decisions.
struct FilterNodeExecution {
  FilterNodeExecution(FilterGraphExecution& execution, const svg::components::FilterNode& node)
      : execution(execution),
        node(node),
        engine(execution.engine),
        arena(execution.arena),
        coordinates(execution.coordinates),
        input(execution.inputFor(node, 0)),
        subregion(coordinates.computeNodeSubregion(node)),
        linearRgb(
            node.colorInterpolationFilters.value_or(execution.graph.colorInterpolationFilters) !=
            svg::ColorInterpolationFilters::SRGB),
        outputLinear(linearRgb) {}

  wgpu::Texture run();
  wgpu::Texture toNodeSpace(const wgpu::Texture& texture);
  bool convertBinaryInputs(wgpu::Texture& second);
  std::optional<Box2d> blurClip(double sx, double sy) const;
  wgpu::Texture apply(const fp::GaussianBlur& primitive);
  wgpu::Texture apply(const fp::Offset& primitive);
  wgpu::Texture apply(const fp::ColorMatrix& primitive);
  wgpu::Texture apply(const fp::Flood& primitive);
  wgpu::Texture apply(const fp::Merge& primitive);
  wgpu::Texture apply(const fp::Composite& primitive);
  wgpu::Texture apply(const fp::Blend& primitive);
  wgpu::Texture apply(const fp::Morphology& primitive);
  wgpu::Texture apply(const fp::ComponentTransfer& primitive);
  wgpu::Texture apply(const fp::ConvolveMatrix& primitive);
  wgpu::Texture apply(const fp::Turbulence& primitive);
  wgpu::Texture apply(const fp::DisplacementMap& primitive);
  wgpu::Texture apply(const fp::DiffuseLighting& primitive);
  wgpu::Texture apply(const fp::SpecularLighting& primitive);
  wgpu::Texture apply(const fp::DropShadow& primitive);
  wgpu::Texture apply(const fp::Image& primitive);
  wgpu::Texture apply(const fp::Tile& primitive);

  FilterGraphExecution& execution;
  const svg::components::FilterNode& node;
  GeodeFilterEngine& engine;
  FilterResourceArena& arena;
  FilterExecutionCoordinates& coordinates;
  wgpu::Texture input;
  Box2d subregion;
  bool linearRgb;
  bool outputLinear;
  bool clipMergedIntoBlur = false;
  bool finalResolve = false;
};

namespace {

bool CanTileFilter(const svg::components::FilterGraph& graph, uint32_t width, uint32_t height,
                   const Transform2d& transform) {
  return !graph.empty() && width && height && width <= 4096 && height <= 4096 &&
         NearZero(transform.data[1], 1e-6) && NearZero(transform.data[2], 1e-6);
}

/// One pass of a 3-pass box-blur approximation of a Gaussian.
/// Mirrors tiny-skia's `computeBoxPasses` (third_party/tiny-skia-cpp/src/
/// tiny_skia/filter/GaussianBlur.cpp) so the Geode and software backends
/// match in pixel coverage and effective sigma.
struct BoxPass {
  int32_t left;
  int32_t right;
};

struct BoxBlurPlan {
  std::array<BoxPass, 3> passes{};
  int numPasses = 0;
};

BoxBlurPlan computeBoxPasses(double sigma) {
  // Same window-size formula as tiny-skia: window = round(sigma * 3*sqrt(2π)/4).
  constexpr double kMaxSigma = svg::components::kMaximumFilterPixelRadius;
  if (!std::isfinite(sigma) || sigma > kMaxSigma) {
    sigma = kMaxSigma;
  }
  const double kWindowScale = 3.0 * std::sqrt(2.0 * std::numbers::pi_v<double>) / 4.0;
  const int window = std::max(1, static_cast<int>(std::floor(sigma * kWindowScale + 0.5)));

  BoxBlurPlan plan;
  if (window <= 1) {
    return plan;
  }

  if ((window & 1) != 0) {
    const int radius = window / 2;
    for (int i = 0; i < 3; ++i) {
      plan.passes[i] = {radius, radius};
    }
    plan.numPasses = 3;
  } else {
    const int half = window / 2;
    plan.passes[0] = {half, half - 1};
    plan.passes[1] = {half - 1, half};
    plan.passes[2] = {half, half};
    plan.numPasses = 3;
  }
  return plan;
}

/// Conservative sampling support of one graph in device pixels.
struct FilterSamplingHalo {
  Vector2d scale;
  Vector2d bounds;
  Transform2d transform;
  bool objectBounds = false;
  Vector2d halo = Vector2d::Zero();

  double deviation(double value, double bound, double pixelScale) const {
    return value >= 0 ? boundedPositiveFilterPixels(
                            FilterLengthInPixels(value, bound, pixelScale, objectBounds))
                      : 0;
  }

  static double blur(double deviation) {
    if (!(deviation > 0)) {
      return 0;
    }
    const BoxBlurPlan boxes = deviation >= 2.0 ? computeBoxPasses(deviation) : BoxBlurPlan{};
    if (boxes.numPasses != 0) {
      int left = 0;
      int right = 0;
      for (int index = 0; index < boxes.numPasses; ++index) {
        left += boxes.passes[index].left;
        right += boxes.passes[index].right;
      }
      return std::max(left, right);
    }
    return std::min(std::ceil(3.0f * static_cast<float>(deviation)), 127.0f);
  }

  bool add(const fp::GaussianBlur& value) {
    if (value.edgeMode == fp::GaussianBlur::EdgeMode::Wrap) {
      return false;
    }
    halo += Vector2d(blur(deviation(value.stdDeviationX, bounds.x, scale.x)),
                     blur(deviation(value.stdDeviationY, bounds.y, scale.y)));
    return true;
  }

  bool add(const fp::Morphology& value) {
    if (value.radiusX < 0 || value.radiusY < 0) {
      return true;
    }
    halo += Vector2d(std::ceil(deviation(value.radiusX, bounds.x, scale.x)),
                     std::ceil(deviation(value.radiusY, bounds.y, scale.y)));
    return true;
  }

  bool add(const fp::Offset& value) {
    const Vector2d offset =
        FilterOffsetInPixels({value.dx, value.dy}, bounds, objectBounds, transform);
    halo += Vector2d(std::ceil(std::abs(boundedSignedFilterPixels(offset.x))),
                     std::ceil(std::abs(boundedSignedFilterPixels(offset.y))));
    return true;
  }

  bool add(const fp::DropShadow& value) {
    add(fp::Offset{value.dx, value.dy});
    halo += Vector2d(blur(deviation(value.stdDeviationX, bounds.x, scale.x)),
                     blur(deviation(value.stdDeviationY, bounds.y, scale.y)));
    return true;
  }

  bool add(const fp::ConvolveMatrix& value) {
    if (value.edgeMode == fp::ConvolveMatrix::EdgeMode::Wrap || value.orderX <= 0 ||
        value.orderY <= 0 || value.orderX > 25 || value.orderY > 25) {
      return false;
    }
    halo += Vector2d(value.orderX, value.orderY);
    return true;
  }

  template <typename T>
  bool add(const T&) {
    return std::is_same_v<T, fp::Flood> || std::is_same_v<T, fp::ColorMatrix> ||
           std::is_same_v<T, fp::ComponentTransfer> || std::is_same_v<T, fp::Blend> ||
           std::is_same_v<T, fp::Composite> || std::is_same_v<T, fp::Merge>;
  }
};

std::optional<Vector2d> ComputeFilterSamplingHalo(const svg::components::FilterGraph& graph,
                                                  const Transform2d& transform) {
  const double scaleX = FilterDeviceScaleX(transform);
  const double scaleY = FilterDeviceScaleY(transform, scaleX);
  const Vector2d bounds =
      graph.elementBoundingBox ? graph.elementBoundingBox->size() : Vector2d(1.0, 1.0);
  FilterSamplingHalo support{Vector2d(scaleX, scaleY), bounds, transform,
                             graph.primitiveUnits == svg::PrimitiveUnits::ObjectBoundingBox};
  for (const auto& node : graph.nodes) {
    if (!std::visit([&](const auto& primitive) { return support.add(primitive); },
                    node.primitive) ||
        !std::isfinite(support.halo.x) || !std::isfinite(support.halo.y)) {
      return std::nullopt;
    }
  }
  return support.halo;
}

FilterTilePlan FitFilterTiles(FilterTilePlan plan, Vector2d halo, gpu::Extent2d maximumExtent) {
  const uint32_t tileWidth = std::min(plan.width, maximumExtent.width);
  const uint32_t tileHeight = std::min(plan.height, maximumExtent.height);
  if ((tileWidth < plan.width && halo.x * 2 >= tileWidth) ||
      (tileHeight < plan.height && halo.y * 2 >= tileHeight)) {
    return plan;
  }
  plan.tileWidth = tileWidth;
  plan.tileHeight = tileHeight;
  plan.coreWidth =
      tileWidth == plan.width ? plan.width : tileWidth - 2 * static_cast<uint32_t>(halo.x);
  plan.coreHeight =
      tileHeight == plan.height ? plan.height : tileHeight - 2 * static_cast<uint32_t>(halo.y);
  plan.tiles = uint64_t{(plan.width + plan.coreWidth - 1) / plan.coreWidth} *
               ((plan.height + plan.coreHeight - 1) / plan.coreHeight);
  return plan;
}

bool FilterPlanFits(const svg::components::FilterGraph& graph, const FilterTilePlan& plan,
                    uint64_t retainedBufferBytes, uint64_t& bytes) {
  using namespace svg::components;
  uint64_t work = 0;
  if (!FilterGraphExecutionCost(graph, plan.workPixels(), FilterMemoryModel::GpuAllNodes, work,
                                bytes, plan.pixels(), plan.tiles)) {
    return false;
  }
  bytes +=
      plan.additionalTextureBytes() + uint64_t{plan.width} * plan.height * 4 + retainedBufferBytes;
  return bytes <= kMaximumFilterFrameBytes;
}

FilterTilePlan ChooseFilterTiles(const svg::components::FilterGraph& graph, FilterTilePlan full,
                                 Vector2d halo, uint32_t preferredExtent, bool adaptive,
                                 uint64_t retainedBufferBytes) {
  const auto preferred = FitFilterTiles(full, halo, {preferredExtent, preferredExtent});
  uint64_t bytes = 0;
  if (preferred.tileWidth <= preferredExtent && preferred.tileHeight <= preferredExtent &&
      FilterPlanFits(graph, preferred, retainedBufferBytes, bytes)) {
    return preferred;
  }
  if (!adaptive) {
    return full;
  }
  FilterTilePlan best = full;
  uint64_t bestBytes = UINT64_MAX;
  for (uint32_t extent = preferredExtent; extent < std::max(full.width, full.height);
       extent += 128) {
    const std::array<gpu::Extent2d, 3> extents{
        {{extent, full.height}, {full.width, extent}, {extent, extent}}};
    for (const auto& size : extents) {
      const auto candidate = FitFilterTiles(full, halo, size);
      if (candidate.tiles > 1 && FilterPlanFits(graph, candidate, retainedBufferBytes, bytes) &&
          bytes < bestBytes) {
        best = candidate;
        bestBytes = bytes;
      }
    }
  }
  return best;
}

bool ValidFilterPlanAxis(uint32_t size, uint32_t tile, uint32_t core) {
  return size > 0 && tile > 0 && core > 0 && tile <= size && core <= tile;
}

bool WellFormedFilterPlan(const FilterTilePlan& plan, uint32_t width, uint32_t height) {
  if (plan.width != width || plan.height != height ||
      !ValidFilterPlanAxis(width, plan.tileWidth, plan.coreWidth) ||
      !ValidFilterPlanAxis(height, plan.tileHeight, plan.coreHeight) ||
      uint64_t{width} * height > svg::components::kMaximumFilterSurfacePixels) {
    return false;
  }
  const uint64_t columns = (uint64_t{width} + plan.coreWidth - 1) / plan.coreWidth;
  const uint64_t rows = (uint64_t{height} + plan.coreHeight - 1) / plan.coreHeight;
  return plan.tiles == columns * rows;
}

bool FilterPlanAxisSupportsHalo(uint32_t size, uint32_t tile, uint32_t core, double halo) {
  return tile == size || uint64_t{core} + 2 * static_cast<uint64_t>(std::ceil(halo)) <= tile;
}

bool AdmittedFilterPlanFitsSource(const svg::components::FilterGraph& graph,
                                  const FilterTilePlan& plan, uint32_t width, uint32_t height,
                                  const Transform2d& transform) {
  if (!WellFormedFilterPlan(plan, width, height)) {
    return false;
  }
  if (plan.tiles == 1) {
    return true;
  }
  if (!CanTileFilter(graph, width, height, transform)) {
    return false;
  }
  const auto halo = ComputeFilterSamplingHalo(graph, transform);
  return halo && FilterPlanAxisSupportsHalo(width, plan.tileWidth, plan.coreWidth, halo->x) &&
         FilterPlanAxisSupportsHalo(height, plan.tileHeight, plan.coreHeight, halo->y);
}

}  // namespace

FilterTilePlan GeodeFilterEngine::executionPlan(const svg::components::FilterGraph& graph,
                                                uint32_t width, uint32_t height,
                                                const Transform2d& deviceFromFilter) const {
  const FilterTilePlan plan{width, height, width, height, width, height, 1};
  if (!CanTileFilter(graph, width, height, deviceFromFilter)) {
    return plan;
  }
  const auto halo = ComputeFilterSamplingHalo(graph, deviceFromFilter);
  return halo ? ChooseFilterTiles(graph, plan, *halo, preferredTileExtent_, adaptiveTiles_,
                                  retainedBufferBytes())
              : plan;
}

void GeodeFilterEngine::setMaximumTileExtentForTesting(uint32_t extent) {
  UTILS_RELEASE_ASSERT(extent >= 16 && extent <= 512);
  preferredTileExtent_ = extent;
  adaptiveTiles_ = false;
}

uint64_t GeodeFilterEngine::retainedBufferBytes() const {
  std::lock_guard<std::mutex> lock(resourceCache_->mutex);
  return resourceCache_->retainedBytes +
         (colorTransferTable_.isValid() ? svg::components::kGpuFilterTransferTableBytes : 0);
}

wgpu::Texture GeodeFilterEngine::execute(const svg::components::FilterGraph& graph,
                                         const wgpu::Texture& sourceGraphic,
                                         const Box2d& filterRegion,
                                         const Transform2d& deviceFromFilter,
                                         FilterTextureAllocator& textureAllocator,
                                         ScopedWgpuHandle<wgpu::CommandEncoder>& commandEncoder,
                                         svg::components::FilterExecutionBudget* executionBudget,
                                         std::optional<FilterTilePlan> admittedPlan) {
  FilterGraphExecution execution(*this, graph, sourceGraphic, filterRegion, deviceFromFilter,
                                 textureAllocator, commandEncoder, executionBudget, admittedPlan);
  const wgpu::Texture output = execution.run();
  lastExecutionMemory_ = execution.arena.memory();
  lastExecutionMemory_.persistentBuffers = retainedBufferBytes();
  lastExecutionMemory_.tileExecutions = execution.executedTiles;
  lastExecutionMemory_.workUnits = execution.admittedWorkUnits;
  return output;
}

wgpu::Texture FilterGraphExecution::inputFor(const svg::components::FilterNode& node,
                                             size_t index) const {
  return resolveInput(filterInputOrDefault(node, index), namedBuffers, currentBuffer, sourceGraphic,
                      sourceAlpha ? &*sourceAlpha : nullptr);
}

wgpu::Texture FilterGraphExecution::clip(const wgpu::Texture& input, const Box2d& region,
                                         bool transformed, bool resolve) {
  if (transformed) {
    return engine.applySubregionClip(arena, input, coordinates.filterFromDevice, region.topLeft.x,
                                     region.topLeft.y, region.bottomRight.x, region.bottomRight.y,
                                     resolve);
  }
  const Box2d pixelAABB = coordinates.deviceFromFilter.transformBox(region);
  return engine.applySubregionClip(arena, input, Transform2d(),
                                   std::floor(pixelAABB.topLeft.x) - tileOrigin.x,
                                   std::floor(pixelAABB.topLeft.y) - tileOrigin.y,
                                   std::ceil(pixelAABB.bottomRight.x) - tileOrigin.x,
                                   std::ceil(pixelAABB.bottomRight.y) - tileOrigin.y, resolve);
}

void FilterGraphExecution::findLastNamedUses() {
  for (size_t index = 0; index < graph.nodes.size(); ++index) {
    for (const svg::components::FilterInput& input : graph.nodes[index].inputs) {
      if (const auto* named = std::get_if<svg::components::FilterInput::Named>(&input.value)) {
        lastNamedUse[named->name.str()] = index;
      }
    }
  }
}

void FilterGraphExecution::retainLiveValues(size_t nodeIndex) {
  const auto expired = [&](const auto& entry) {
    const auto last = lastNamedUse.find(entry.first);
    return last == lastNamedUse.end() || last->second <= nodeIndex;
  };
  std::erase_if(namedBuffers, expired);
  std::erase_if(coordinates.namedSubregions, expired);
  SmallVector<wgpu::Texture, 8> live{sourceGraphic, currentBuffer};
  if (sourceAlpha) {
    live.push_back(*sourceAlpha);
  }
  for (const auto& [name, texture] : namedBuffers) {
    live.push_back(texture);
  }
  arena.retainValues(std::span<const wgpu::Texture>(live.data(), live.size()));
}

void FilterGraphExecution::record(const svg::components::FilterNode& node, const Box2d& subregion,
                                  const wgpu::Texture& output, size_t nodeIndex) {
  if (node.result.has_value()) {
    coordinates.namedSubregions[node.result->str()] = subregion;
    namedBuffers[node.result->str()] = output;
  }
  coordinates.previousOutputSubregion = subregion;
  currentBuffer = output;
  retainLiveValues(nodeIndex);
}

wgpu::Texture FilterGraphExecution::run() {
  using namespace svg::components;
  FilterTilePlan plan =
      admittedPlan ? *admittedPlan
                   : engine.executionPlan(graph, sourceGraphic.getWidth(),
                                          sourceGraphic.getHeight(), coordinates.deviceFromFilter);
  if (admittedPlan &&
      !AdmittedFilterPlanFitsSource(graph, plan, sourceGraphic.getWidth(),
                                    sourceGraphic.getHeight(), coordinates.deviceFromFilter)) {
    return {};
  }
  if (plan.tiles > 1 && !(sourceGraphic.getUsage() & wgpu::TextureUsage::CopySrc)) {
    if (admittedPlan) {
      return {};
    }
    plan = {plan.width, plan.height, plan.width, plan.height, plan.width, plan.height, 1};
  }
  FilterExecutionBudget localBudget;
  FilterExecutionBudget& budget = executionBudget ? *executionBudget : localBudget;
  const uint64_t workBefore = budget.workUnits();
  auto reservation = budget.reserve(graph, plan.workPixels(), FilterMemoryModel::GpuAllNodes,
                                    plan.additionalTextureBytes(), engine.retainedBufferBytes(),
                                    plan.pixels(), plan.tiles);
  if (!reservation) {
    return admittedPlan ? wgpu::Texture{} : sourceGraphic;
  }
  admittedWorkUnits = budget.workUnits() - workBefore;
  const wgpu::Texture result = plan.tiles > 1 ? runTiled(plan) : runNodes();
  budget.release(*reservation);
  return result;
}

wgpu::Texture FilterGraphExecution::runTiled(const FilterTilePlan& plan) {
  const wgpu::Texture fullSource = sourceGraphic;
  const wgpu::Texture output = arena.createTexture(
      {"FilterTiledOutput",
       {plan.width, plan.height},
       gpu::TextureFormat::RGBA8Unorm,
       gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc | gpu::TextureUsage::CopyDst});
  if (!output) {
    return {};
  }
  const wgpu::Texture tileInput =
      arena.createTexture({"FilterTileInput",
                           {plan.tileWidth, plan.tileHeight},
                           gpu::TextureFormat::RGBA8Unorm,
                           gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  if (!tileInput) {
    return {};
  }
  const uint32_t haloX = (plan.tileWidth - plan.coreWidth) / 2;
  const uint32_t haloY = (plan.tileHeight - plan.coreHeight) / 2;
  for (uint32_t y = 0; y < plan.height; y += plan.coreHeight) {
    for (uint32_t x = 0; x < plan.width; x += plan.coreWidth) {
      const uint32_t originX = std::min(x > haloX ? x - haloX : 0, plan.width - plan.tileWidth);
      const uint32_t originY = std::min(y > haloY ? y - haloY : 0, plan.height - plan.tileHeight);
      if (!arena.copyTexture(fullSource, tileInput, {plan.tileWidth, plan.tileHeight},
                             {originX, originY}, {})) {
        return {};
      }
      arena.resetLogicalValues();
      sourceGraphic = currentBuffer = tileInput;
      sourceAlpha.reset();
      namedBuffers.clear();
      coordinates.namedSubregions.clear();
      coordinates.previousOutputSubregion = coordinates.filterRegion;
      tileOrigin = Vector2d(originX, originY);
      const wgpu::Texture result = runNodes();
      if (!result || !arena.copyTexture(result, output,
                                        {std::min(plan.coreWidth, plan.width - x),
                                         std::min(plan.coreHeight, plan.height - y)},
                                        {x - originX, y - originY}, {x, y})) {
        return {};
      }
    }
  }
  return output;
}

wgpu::Texture FilterGraphExecution::runNodes() {
  using namespace svg::components;
  ++executedTiles;
  if (graphUsesStandardInput(graph, FilterStandardInput::SourceAlpha)) {
    sourceAlpha = engine.applySourceAlpha(arena, sourceGraphic);
    if (!*sourceAlpha) {
      return {};
    }
  }
  findLastNamedUses();
  for (size_t index = 0; index < graph.nodes.size(); ++index) {
    const FilterNode& node = graph.nodes[index];
    FilterNodeExecution primitive(*this, node);
    primitive.finalResolve = index + 1 == graph.nodes.size() &&
                             std::holds_alternative<fp::DropShadow>(node.primitive) &&
                             axisAligned && primitive.subregion == coordinates.filterRegion;
    const wgpu::Texture output = primitive.run();
    if (!output) {
      return {};
    }
    record(node, primitive.subregion, output, index);
    if (primitive.finalResolve) {
      return clip(currentBuffer, coordinates.filterRegion, false, true);
    }
  }
  const wgpu::Texture srgb =
      engine.applyColorSpaceConversion(arena, currentBuffer, /*srgbToLinear=*/false);
  return srgb ? clip(srgb, coordinates.filterRegion, !axisAligned, /*resolve=*/true)
              : wgpu::Texture();
}

wgpu::Texture FilterNodeExecution::run() {
  if (!input) {
    return {};
  }
  wgpu::Texture output =
      std::visit([this](const auto& primitive) { return apply(primitive); }, node.primitive);
  if (!output) {
    return {};
  }
  if (!clipMergedIntoBlur && !finalResolve) {
    const bool explicitSubregion = node.x || node.y || node.width || node.height;
    output = execution.clip(output, subregion, explicitSubregion && !execution.axisAligned);
  }
  if (output) {
    arena.markLinearRgb(output, outputLinear);
  }
  return output;
}

wgpu::Texture FilterNodeExecution::toNodeSpace(const wgpu::Texture& texture) {
  return engine.applyColorSpaceConversion(arena, texture, linearRgb);
}

bool FilterNodeExecution::convertBinaryInputs(wgpu::Texture& second) {
  second = execution.inputFor(node, 1);
  input = toNodeSpace(input);
  if (!input) {
    return false;
  }
  second = toNodeSpace(second);
  return static_cast<bool>(second);
}

std::optional<Box2d> FilterNodeExecution::blurClip(double sx, double sy) const {
  if (!execution.axisAligned || (sx <= 0.0 && sy <= 0.0)) {
    return std::nullopt;
  }
  const Box2d pixels = coordinates.deviceFromFilter.transformBox(subregion);
  const Box2d rounded(Vector2d(std::floor(pixels.topLeft.x) - execution.tileOrigin.x,
                               std::floor(pixels.topLeft.y) - execution.tileOrigin.y),
                      Vector2d(std::ceil(pixels.bottomRight.x) - execution.tileOrigin.x,
                               std::ceil(pixels.bottomRight.y) - execution.tileOrigin.y));
  const std::array<double, 4> corners{rounded.topLeft.x, rounded.topLeft.y, rounded.bottomRight.x,
                                      rounded.bottomRight.y};
  // The folded clip uses i32 uniforms; extreme and NaN corners retain the separate float clip.
  if (!std::all_of(corners.begin(), corners.end(), [](double value) {
        return value >= double{std::numeric_limits<int32_t>::min()} &&
               value <= double{std::numeric_limits<int32_t>::max()};
      })) {
    return std::nullopt;
  }
  return rounded;
}

wgpu::Texture FilterNodeExecution::apply(const fp::GaussianBlur& primitive) {
  const double sx = primitive.stdDeviationX >= 0
                        ? boundedPositiveFilterPixels(coordinates.toPixelX(primitive.stdDeviationX))
                        : 0.0;
  const double sy = primitive.stdDeviationY >= 0
                        ? boundedPositiveFilterPixels(coordinates.toPixelY(primitive.stdDeviationY))
                        : 0.0;
  const std::optional<Box2d> clip = blurClip(sx, sy);
  clipMergedIntoBlur = clip.has_value();
  return engine.applyGaussianBlur(arena, toNodeSpace(input), sx, sy,
                                  toShaderEdgeMode(primitive.edgeMode), clip ? &*clip : nullptr);
}

wgpu::Texture FilterNodeExecution::apply(const fp::Offset& primitive) {
  const Vector2d offset = coordinates.toPixelOffset(primitive.dx, primitive.dy);
  fp::Offset scaled = primitive;
  scaled.dx = boundedSignedFilterPixels(offset.x);
  scaled.dy = boundedSignedFilterPixels(offset.y);
  outputLinear = arena.isLinearRgb(input);
  return engine.applyOffset(arena, input, scaled);
}

wgpu::Texture FilterNodeExecution::apply(const fp::ColorMatrix& primitive) {
  if (isIdentityColorMatrix(buildColorMatrix(primitive))) {
    outputLinear = arena.isLinearRgb(input);
    return input;
  }
  return engine.applyColorMatrix(arena, toNodeSpace(input), primitive);
}

wgpu::Texture FilterNodeExecution::apply(const fp::Flood& primitive) {
  outputLinear = false;
  return engine.applyFlood(arena, input.getWidth(), input.getHeight(), primitive);
}

wgpu::Texture FilterNodeExecution::apply(const fp::Merge&) {
  return engine.applyMerge(arena, node, execution.namedBuffers, execution.currentBuffer,
                           execution.sourceGraphic,
                           execution.sourceAlpha ? &*execution.sourceAlpha : nullptr, linearRgb);
}

wgpu::Texture FilterNodeExecution::apply(const fp::Composite& primitive) {
  wgpu::Texture second;
  return convertBinaryInputs(second) ? engine.applyComposite(arena, input, second, primitive)
                                     : wgpu::Texture();
}

wgpu::Texture FilterNodeExecution::apply(const fp::Blend& primitive) {
  wgpu::Texture second;
  return convertBinaryInputs(second) ? engine.applyBlend(arena, input, second, primitive)
                                     : wgpu::Texture();
}

wgpu::Texture FilterNodeExecution::apply(const fp::Morphology& primitive) {
  // Test signed SVG radii before toPixelX/Y turn them into magnitudes.
  if (primitive.radiusX < 0 || primitive.radiusY < 0 ||
      (primitive.radiusX == 0 && primitive.radiusY == 0)) {
    outputLinear = arena.isLinearRgb(input);
    return input;
  }
  const int rx = boundedRoundedInt32(coordinates.toPixelX(primitive.radiusX), 0,
                                     svg::components::kMaximumFilterPixelRadius);
  const int ry = boundedRoundedInt32(coordinates.toPixelY(primitive.radiusY), 0,
                                     svg::components::kMaximumFilterPixelRadius);
  return engine.applyMorphology(arena, toNodeSpace(input), primitive, rx, ry);
}

wgpu::Texture FilterNodeExecution::apply(const fp::ComponentTransfer& primitive) {
  return engine.applyComponentTransfer(arena, toNodeSpace(input), primitive);
}

wgpu::Texture FilterNodeExecution::apply(const fp::ConvolveMatrix& primitive) {
  return engine.applyConvolveMatrix(arena, toNodeSpace(input), primitive);
}

wgpu::Texture FilterNodeExecution::apply(const fp::Turbulence& primitive) {
  return engine.applyTurbulence(arena, input.getWidth(), input.getHeight(), primitive,
                                coordinates.deviceFromFilter);
}

wgpu::Texture FilterNodeExecution::apply(const fp::DisplacementMap& primitive) {
  double scale = std::abs(primitive.scale);
  if (coordinates.isObjectBoundingBox) {
    scale *= std::sqrt(coordinates.boundingBoxWidth * coordinates.boundingBoxHeight);
  }
  scale *= std::sqrt(coordinates.scaleX * coordinates.scaleY);
  wgpu::Texture second;
  return convertBinaryInputs(second)
             ? engine.applyDisplacementMap(arena, input, second, primitive, scale)
             : wgpu::Texture();
}

wgpu::Texture FilterNodeExecution::apply(const fp::DiffuseLighting& primitive) {
  const Box2d bounds = coordinates.resolveInputSubregion(filterInputOrDefault(node, 0));
  return engine.applyDiffuseLighting(arena, input, primitive, execution.graph,
                                     coordinates.deviceFromFilter, bounds, linearRgb);
}

wgpu::Texture FilterNodeExecution::apply(const fp::SpecularLighting& primitive) {
  const Box2d bounds = coordinates.resolveInputSubregion(filterInputOrDefault(node, 0));
  return engine.applySpecularLighting(arena, input, primitive, execution.graph,
                                      coordinates.deviceFromFilter, bounds, linearRgb);
}

wgpu::Texture FilterNodeExecution::apply(const fp::DropShadow& primitive) {
  const double sx = primitive.stdDeviationX >= 0
                        ? boundedPositiveFilterPixels(coordinates.toPixelX(primitive.stdDeviationX))
                        : 0.0;
  const double sy = primitive.stdDeviationY >= 0
                        ? boundedPositiveFilterPixels(coordinates.toPixelY(primitive.stdDeviationY))
                        : 0.0;
  const Vector2d offset = coordinates.toPixelOffset(primitive.dx, primitive.dy);
  return engine.applyDropShadow(arena, toNodeSpace(input), primitive, sx, sy,
                                boundedSignedFilterPixels(offset.x),
                                boundedSignedFilterPixels(offset.y));
}

wgpu::Texture FilterNodeExecution::apply(const fp::Image& primitive) {
  const bool isOBB = coordinates.isObjectBoundingBox;
  const double bboxW = coordinates.boundingBoxWidth;
  const double bboxH = coordinates.boundingBoxHeight;
  const double x =
      node.x ? coordinates.resolvePrimitivePosition(*node.x, Lengthd::Extent::X,
                                                    isOBB ? coordinates.boundingBoxX : 0.0, bboxW)
             : coordinates.filterRegion.topLeft.x;
  const double y =
      node.y ? coordinates.resolvePrimitivePosition(*node.y, Lengthd::Extent::Y,
                                                    isOBB ? coordinates.boundingBoxY : 0.0, bboxH)
             : coordinates.filterRegion.topLeft.y;
  const double width =
      node.width ? coordinates.resolvePrimitiveSize(*node.width, Lengthd::Extent::X, bboxW)
                 : coordinates.filterRegion.width();
  const double height =
      node.height ? coordinates.resolvePrimitiveSize(*node.height, Lengthd::Extent::Y, bboxH)
                  : coordinates.filterRegion.height();
  outputLinear = false;
  return engine.applyImage(arena, primitive, input.getWidth(), input.getHeight(), execution.graph,
                           node, coordinates.deviceFromFilter,
                           Box2d::FromXYWH(x, y, width, height));
}

wgpu::Texture FilterNodeExecution::apply(const fp::Tile&) {
  outputLinear = arena.isLinearRgb(input);
  const Box2d source = coordinates.resolveInputSubregion(filterInputOrDefault(node, 0));
  // transformBox normalizes inverted rectangles, so reject empty user-space bounds first.
  if (source.width() <= 0.0 || source.height() <= 0.0) {
    return engine.applyTile(arena, input, 0, 0, 0, 0);
  }
  const Box2d pixels = coordinates.deviceFromFilter.transformBox(source);
  constexpr int32_t kMaximumExtent = svg::components::kMaximumFilterPixelOffset;
  return engine.applyTile(arena, input,
                          boundedFloorInt32(pixels.topLeft.x, -kMaximumExtent, kMaximumExtent),
                          boundedFloorInt32(pixels.topLeft.y, -kMaximumExtent, kMaximumExtent),
                          boundedCeilInt32(pixels.width(), 0, kMaximumExtent),
                          boundedCeilInt32(pixels.height(), 0, kMaximumExtent));
}

namespace {

struct BlurDispatch {
  uint32_t axis = 0;
  float deviation = 0;
  std::optional<BoxPass> box;
};

struct BlurDispatchPlan {
  std::array<BlurDispatch, 6> passes{};
  size_t count = 0;
};

BlurDispatchPlan CreateBlurDispatchPlan(double stdDeviationX, double stdDeviationY) {
  BlurDispatchPlan result;
  const std::array<double, 2> deviations{stdDeviationX, stdDeviationY};
  for (uint32_t axis = 0; axis < deviations.size(); ++axis) {
    const double deviation = deviations[axis];
    if (!(deviation > 0.0)) {
      continue;
    }
    const BoxBlurPlan boxes = deviation >= 2.0 ? computeBoxPasses(deviation) : BoxBlurPlan{};
    if (boxes.numPasses == 0) {
      result.passes[result.count++] =
          BlurDispatch{axis, static_cast<float>(deviation), std::nullopt};
    } else {
      for (int i = 0; i < boxes.numPasses; ++i) {
        result.passes[result.count++] = BlurDispatch{axis, 0, boxes.passes[i]};
      }
    }
  }
  return result;
}

}  // namespace

wgpu::Texture GeodeFilterEngine::applyGaussianBlur(FilterResourceArena& arena,
                                                   const wgpu::Texture& input, double stdDeviationX,
                                                   double stdDeviationY, uint32_t edgeMode,
                                                   const Box2d* outputClip) {
  if (!input) {
    return {};
  }

  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();
  const BlurDispatchPlan plan = CreateBlurDispatchPlan(stdDeviationX, stdDeviationY);
  std::array<wgpu::Texture, 2> scratch{};
  wgpu::Texture current = input;
  for (size_t i = 0; i < plan.count; ++i) {
    wgpu::Texture& output = scratch[i % scratch.size()];
    if (!output) {
      output =
          createIntermediateTexture(arena, device_.device(), width, height, "GaussianBlurScratch");
    }
    const BlurDispatch& pass = plan.passes[i];
    const Box2d* clip = i + 1 == plan.count ? outputClip : nullptr;
    if (pass.box) {
      current = runBoxBlurPass(arena, current, output, width, height, pass.box->left,
                               pass.box->right, pass.axis, edgeMode, clip);
    } else {
      current = runBlurPass(arena, current, output, width, height, pass.deviation, pass.axis,
                            edgeMode, clip);
    }
    if (!current) {
      return {};
    }
  }
  return current;
}

wgpu::Texture GeodeFilterEngine::runBlurPass(FilterResourceArena& arena, const wgpu::Texture& input,
                                             const wgpu::Texture& output, uint32_t width,
                                             uint32_t height, float stdDeviation, uint32_t axis,
                                             uint32_t edgeMode, const Box2d* clip) {
  if (!input || !output) {
    return {};
  }

  BlurParams params{};
  params.stdDeviation = stdDeviation;
  params.axis = axis;
  params.edgeMode = edgeMode;
  params.kernelType = 0;
  params.boxLeft = 0;
  params.boxRight = 0;
  if (clip != nullptr) {
    // Corners arrive pre-rounded (floor/ceil), so the integer casts are exact.
    params.clipMinX = static_cast<int32_t>(clip->topLeft.x);
    params.clipMinY = static_cast<int32_t>(clip->topLeft.y);
    params.clipMaxX = static_cast<int32_t>(clip->bottomRight.x);
    params.clipMaxY = static_cast<int32_t>(clip->bottomRight.y);
    params.clipActive = 1;
  }
  params.pad1 = 0;

  const gpu::Texture* runtimeOutput = arena.importRuntimeTexture(output);
  if (runtimeOutput == nullptr ||
      !dispatchRuntimeInputOutputParameters(arena, blurProgram_, input, *runtimeOutput,
                                            UniformBytes(params), "GaussianBlurPass",
                                            gpu::shader::programs::kGaussianBlurWorkgroupSize)) {
    return {};
  }
  return output;
}

wgpu::Texture GeodeFilterEngine::runBoxBlurPass(FilterResourceArena& arena,
                                                const wgpu::Texture& input,
                                                const wgpu::Texture& output, uint32_t width,
                                                uint32_t height, int32_t boxLeft, int32_t boxRight,
                                                uint32_t axis, uint32_t edgeMode,
                                                const Box2d* clip) {
  if (!input || !output) {
    return {};
  }

  BlurParams params{};
  params.stdDeviation = 0.0f;
  params.axis = axis;
  params.edgeMode = edgeMode;
  params.kernelType = 1;
  params.boxLeft = boxLeft;
  params.boxRight = boxRight;
  if (clip != nullptr) {
    params.clipMinX = static_cast<int32_t>(clip->topLeft.x);
    params.clipMinY = static_cast<int32_t>(clip->topLeft.y);
    params.clipMaxX = static_cast<int32_t>(clip->bottomRight.x);
    params.clipMaxY = static_cast<int32_t>(clip->bottomRight.y);
    params.clipActive = 1;
  }
  params.pad1 = 0;

  const gpu::Texture* runtimeOutput = arena.importRuntimeTexture(output);
  if (runtimeOutput == nullptr ||
      !dispatchRuntimeInputOutputParameters(arena, blurProgram_, input, *runtimeOutput,
                                            UniformBytes(params), "BoxBlurPass",
                                            gpu::shader::programs::kGaussianBlurWorkgroupSize)) {
    return {};
  }
  return output;
}

wgpu::Texture GeodeFilterEngine::applyOffset(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::Offset& primitive) {
  if (!input) {
    return {};
  }

  // Zero offset → passthrough.
  if (primitive.dx == 0.0 && primitive.dy == 0.0) {
    return input;
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterOffsetOutput", gpu::Extent2d{input.getWidth(), input.getHeight()},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  OffsetParams params{};
  // Round before narrowing: doubles on either side of a half can become the same float.
  params.dx = static_cast<float>(std::round(primitive.dx));
  params.dy = static_cast<float>(std::round(primitive.dy));
  params.pad0 = 0;
  params.pad1 = 0;

  if (!dispatchRuntimeInputOutputParameters(arena, offsetProgram_, input, *output,
                                            UniformBytes(params), "FilterOffsetPass",
                                            gpu::shader::programs::kOffsetWorkgroupSize)) {
    return {};
  }

  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applyColorMatrix(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::ColorMatrix& primitive) {
  if (!input) {
    return {};
  }

  FilterColorMatrixParams params = buildColorMatrix(primitive);

  // Match tiny-skia's identity shortcut: skip the shader to avoid the
  // unpremultiply→multiply→premultiply round-trip that introduces precision
  // loss on semi-transparent pixels (e.g. gradient edges).
  if (isIdentityColorMatrix(params)) {
    return input;
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterColorMatrixOutput", gpu::Extent2d{input.getWidth(), input.getHeight()},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  if (!dispatchRuntimeInputOutputParameters(
          arena, colorMatrixProgram_, input, *output, UniformBytes(params), "FilterColorMatrixPass",
          gpu::shader::programs::kFilterColorMatrixWorkgroupSize)) {
    return {};
  }

  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applySourceAlpha(FilterResourceArena& arena,
                                                  const wgpu::Texture& input) {
  if (!input) {
    return {};
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterSourceAlphaOutput", gpu::Extent2d{input.getWidth(), input.getHeight()},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  // SourceAlpha is the color matrix that keeps alpha and zeroes the colors, so it runs through the
  // same program rather than a second pipeline that would have to agree with it.
  FilterColorMatrixParams params{};
  params.col3[3] = 1.0f;

  if (!dispatchRuntimeInputOutputParameters(
          arena, colorMatrixProgram_, input, *output, UniformBytes(params), "FilterSourceAlphaPass",
          gpu::shader::programs::kFilterColorMatrixWorkgroupSize)) {
    return {};
  }

  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applyFlood(
    FilterResourceArena& arena, uint32_t width, uint32_t height,
    const svg::components::filter_primitive::Flood& primitive) {
  if (!floodProgram_.pipeline.isValid()) {
    return {};
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterFloodOutput", gpu::Extent2d{width, height}, gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  // Authored flood colors enter both renderers as premultiplied bytes; subsequent math stays float.
  const css::RGBA rgba = primitive.floodColor.asRGBA();
  const double alpha = (rgba.a / 255.0) * std::clamp(primitive.floodOpacity, 0.0, 1.0);
  FloodParams params{};
  params.color[0] = static_cast<float>(std::round(rgba.r * alpha)) / 255.0f;
  params.color[1] = static_cast<float>(std::round(rgba.g * alpha)) / 255.0f;
  params.color[2] = static_cast<float>(std::round(rgba.b * alpha)) / 255.0f;
  params.color[3] = static_cast<float>(std::round(255.0 * alpha)) / 255.0f;

  const gpu::TextureView* outputView =
      arena.createRuntimeTextureView(*output, "FilterFloodOutputView");
  const FilterResourceCache::RuntimeParameterSlot uniforms =
      arena.writeRuntimeParameterSlot(UniformBytes(params));
  if (outputView == nullptr || uniforms.buffer == nullptr) {
    return {};
  }

  // Flood has no input texture - only output + uniform.
  const gpu::BindGroup* bindGroup = arena.createRuntimeBindGroup(
      floodProgram_.bindGroupLayout,
      {gpu::BindGroupEntry{
           static_cast<uint32_t>(gpu::shader::programs::FloodBinding::OutputTexture),
           gpu::TextureViewBinding{*outputView}},
       gpu::BindGroupEntry{
           static_cast<uint32_t>(gpu::shader::programs::FloodBinding::Params),
           gpu::BufferBinding{*uniforms.buffer, uniforms.offset, sizeof(FloodParams)}}},
      "FilterFloodBindGroup");
  if (bindGroup == nullptr) {
    return {};
  }

  constexpr uint32_t kWorkgroup = gpu::shader::programs::kFloodWorkgroupSize;
  if (!arena.dispatchComputePass("FilterFloodPass", floodProgram_.pipeline, *bindGroup,
                                 (width + kWorkgroup - 1) / kWorkgroup,
                                 (height + kWorkgroup - 1) / kWorkgroup)) {
    return {};
  }

  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applyMerge(
    FilterResourceArena& arena, const svg::components::FilterNode& node,
    const std::unordered_map<std::string, wgpu::Texture>& namedBuffers,
    const wgpu::Texture& currentBuffer, const wgpu::Texture& sourceGraphic,
    const wgpu::Texture* sourceAlpha, bool linearRGB) {
  const uint32_t width = currentBuffer.getWidth();
  const uint32_t height = currentBuffer.getHeight();

  if (node.inputs.empty()) {
    svg::components::filter_primitive::Flood transparent;
    transparent.floodColor = css::Color(css::RGBA(0, 0, 0, 0));
    transparent.floodOpacity = 0.0;
    return applyFlood(arena, width, height, transparent);
  }

  const auto inWorkingSpace = [&](const wgpu::Texture& texture) {
    return applyColorSpaceConversion(arena, texture, linearRGB);
  };

  // Resolve first input as the initial accumulator.
  wgpu::Texture accumulator = inWorkingSpace(
      resolveInput(node.inputs[0], namedBuffers, currentBuffer, sourceGraphic, sourceAlpha));

  if (!accumulator) {
    return {};
  }

  // Alpha-over composite each subsequent input on top.
  for (size_t i = 1; i < node.inputs.size(); ++i) {
    wgpu::Texture src = inWorkingSpace(
        resolveInput(node.inputs[i], namedBuffers, currentBuffer, sourceGraphic, sourceAlpha));
    accumulator = runMergePass(arena, src, accumulator, width, height);
    if (!accumulator) {
      return {};
    }
  }

  return accumulator;
}

wgpu::Texture GeodeFilterEngine::runMergePass(FilterResourceArena& arena, const wgpu::Texture& src,
                                              const wgpu::Texture& dst, uint32_t width,
                                              uint32_t height) {
  if (!src || !dst) {
    return {};
  }
  const gpu::Extent2d extent{width, height};
  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterMergeOutput", extent, gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr ||
      !dispatchRuntimeTwoInput(arena, mergeProgram_, src, dst, *output, extent, {},
                               "FilterMergePass", gpu::shader::programs::kMergeWorkgroupSize)) {
    return {};
  }
  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applyComposite(
    FilterResourceArena& arena, const wgpu::Texture& in1, const wgpu::Texture& in2,
    const svg::components::filter_primitive::Composite& primitive) {
  if (!in1 || !in2) {
    return {};
  }
  const gpu::Extent2d extent{in1.getWidth(), in1.getHeight()};
  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterCompositeOutput", extent, gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  CompositeParams params{};
  params.op = static_cast<uint32_t>(ShaderCompositeOperator(primitive.op));
  params.pad0 = 0;
  params.pad1 = 0;
  params.pad2 = 0;
  params.k1 = static_cast<float>(primitive.k1);
  params.k2 = static_cast<float>(primitive.k2);
  params.k3 = static_cast<float>(primitive.k3);
  params.k4 = static_cast<float>(primitive.k4);

  if (!dispatchRuntimeTwoInput(arena, compositeProgram_, in1, in2, *output, extent,
                               UniformBytes(params), "FilterCompositePass",
                               gpu::shader::programs::kCompositeWorkgroupSize)) {
    return {};
  }
  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applyBlend(
    FilterResourceArena& arena, const wgpu::Texture& in1, const wgpu::Texture& in2,
    const svg::components::filter_primitive::Blend& primitive) {
  if (!in1 || !in2) {
    return {};
  }

  const gpu::Extent2d extent{in1.getWidth(), in1.getHeight()};
  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterBlendOutput", extent, gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  BlendParams params{};
  params.mode = static_cast<uint32_t>(primitive.mode);
  params.pad0 = 0;
  params.pad1 = 0;
  params.pad2 = 0;

  if (!dispatchRuntimeTwoInput(arena, blendProgram_, in1, in2, *output, extent,
                               UniformBytes(params), "FilterBlendPass",
                               gpu::shader::programs::kBlendWorkgroupSize)) {
    return {};
  }
  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applyMorphology(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::Morphology& primitive, int pixelRadiusX,
    int pixelRadiusY) {
  if (!input) {
    return {};
  }

  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();
  constexpr int kMaximumRadiusPerPass = 31;
  const std::array<int, 2> radii{std::max(pixelRadiusX, 0), std::max(pixelRadiusY, 0)};
  constexpr const char* kLabels[] = {"FilterMorphologyPassX", "FilterMorphologyPassY"};
  std::array<const gpu::Texture*, 2> scratch{};
  size_t scratchIndex = 0;
  wgpu::Texture current = input;
  for (uint32_t axis = 0; axis < radii.size(); ++axis) {
    int remaining = radii[axis];
    while (remaining > 0) {
      const int radius = std::min(remaining, kMaximumRadiusPerPass);
      const gpu::Texture*& output = scratch[scratchIndex];
      if (!output) {
        output = arena.createRuntimeTexture(
            gpu::TextureDescriptor{"FilterMorphologyOutput",
                                   {width, height},
                                   gpu::TextureFormat::RGBA32Float,
                                   gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled |
                                       gpu::TextureUsage::CopySrc});
      }
      if (!output) {
        return {};
      }
      using Op = svg::components::filter_primitive::Morphology::Operator;
      MorphologyParams params{};
      params.radiusX = axis == 0 ? radius : 0;
      params.radiusY = axis == 1 ? radius : 0;
      params.op = primitive.op == Op::Dilate ? 1u : 0u;
      if (!dispatchRuntimeInputOutputParameters(arena, morphologyProgram_, current, *output,
                                                UniformBytes(params), kLabels[axis],
                                                gpu::shader::programs::kMorphologyWorkgroupSize)) {
        return {};
      }
      current = device_.adapterDevice().wgpuTextureOf(*output);
      scratchIndex = (scratchIndex + 1) % scratch.size();
      remaining -= radius;
    }
  }
  return current;
}

wgpu::Texture GeodeFilterEngine::applyComponentTransfer(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::ComponentTransfer& primitive) {
  if (!input) {
    return {};
  }

  const auto data = BuildComponentTransferData(primitive);
  if (!data) {
    return {};
  }
  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterComponentTransferOutput",
      {input.getWidth(), input.getHeight()},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }
  const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data->data()),
                                       data->size() * sizeof(float));
  if (!dispatchRuntimeInputOutputParameters(
          arena, componentTransferProgram_, input, *output, bytes, "FilterComponentTransferPass",
          gpu::shader::programs::kComponentTransferWorkgroupSize)) {
    return {};
  }
  return device_.adapterDevice().wgpuTextureOf(*output);
}

namespace {

bool HasValidConvolveKernel(const svg::components::filter_primitive::ConvolveMatrix& primitive,
                            int requiredSize) {
  return primitive.orderX > 0 && primitive.orderY > 0 && requiredSize <= 25 &&
         static_cast<int>(primitive.kernelMatrix.size()) == requiredSize;
}

}  // namespace

wgpu::Texture GeodeFilterEngine::applyConvolveMatrix(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::ConvolveMatrix& primitive) {
  if (!input) {
    return {};
  }

  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  const int targetX = primitive.targetX.value_or(primitive.orderX / 2);
  const int targetY = primitive.targetY.value_or(primitive.orderY / 2);
  const int requiredSize = primitive.orderX * primitive.orderY;

  // Compute effective divisor.
  double divisor = 1.0;
  if (primitive.divisor.has_value()) {
    divisor = primitive.divisor.value();
  } else {
    double sum = 0.0;
    for (double v : primitive.kernelMatrix) {
      sum += v;
    }
    divisor = (std::abs(sum) < 1e-10) ? 1.0 : sum;
  }

  // SVG spec validation: invalid parameters produce transparent black.
  // Matches the CPU reference's guard in FilterGraph.cpp.
  // The shader kernel array holds 25 elements, so any orderX*orderY <= 25 is valid.
  const bool invalid = !HasValidConvolveKernel(primitive, requiredSize) || targetX < 0 ||
                       targetX >= primitive.orderX || targetY < 0 || targetY >= primitive.orderY ||
                       (primitive.divisor.has_value() && primitive.divisor.value() == 0.0);

  if (invalid) {
    if (verbose_) {
      std::cerr << "GeodeFilterEngine: feConvolveMatrix invalid params (" << primitive.orderX << "×"
                << primitive.orderY << ", targetX=" << targetX << ", targetY=" << targetY
                << ", divisor=" << divisor << "); outputting transparent black\n";
    }
    // Return a transparent texture (matches CPU reference behavior).
    return createTransparentIntermediateTexture(arena, width, height,
                                                "FilterConvolveMatrixTransparent");
  }

  wgpu::Texture output =
      createIntermediateTexture(arena, dev, width, height, "FilterConvolveMatrixOutput");
  if (!output) {
    return {};
  }

  ConvolveParams params{};
  params.orderX = primitive.orderX;
  params.orderY = primitive.orderY;
  params.targetX = targetX;
  params.targetY = targetY;
  params.divisor = static_cast<float>(divisor);
  params.bias = static_cast<float>(primitive.bias);
  params.edgeMode = toConvolveEdgeMode(primitive.edgeMode);
  params.preserveAlpha = primitive.preserveAlpha ? 1u : 0u;

  // Fill kernel array (row-major, max 25 entries).
  std::fill(std::begin(params.kernel), std::end(params.kernel), 0.0f);
  const int count = std::min(static_cast<int>(primitive.kernelMatrix.size()),
                             primitive.orderX * primitive.orderY);
  for (int i = 0; i < count && i < 25; ++i) {
    params.kernel[i] = static_cast<float>(primitive.kernelMatrix[i]);
  }

  // Upload as a storage buffer (not uniform - array<f32,25> has 16-byte stride in uniform).
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("ConvolveMatrixParamsStorage");
  bufDesc.size = sizeof(ConvolveParams);
  bufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer paramsBuffer = arena.createBuffer(dev, bufDesc);
  device_.queue().writeBuffer(paramsBuffer, 0, &params, sizeof(params));
  device_.countBufferWrite(sizeof(params));

  // Build bind group.
  ScopedWgpuHandle<wgpu::TextureView> inputView(input.createView());
  ScopedWgpuHandle<wgpu::TextureView> outputView(output.createView());

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inputView.get();
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outputView.get();
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = paramsBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(ConvolveParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterConvolveMatrixBindGroup");
  bgDesc.layout = convolveMatrixBindGroupLayout_.get();
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup(dev.createBindGroup(bgDesc));
  device_.countBindGroup();

  wgpu::CommandEncoder& encoder = arena.commandEncoder();

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterConvolveMatrixPass");
  ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(encoder.beginComputePass(passDesc));
  pass.get().setPipeline(convolveMatrixPipeline_.get());
  pass.get().setBindGroup(0, bindGroup.get(), 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.get().dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.get().end();
  pass.reset();

  return output;
}

wgpu::Texture GeodeFilterEngine::applyTurbulence(
    FilterResourceArena& arena, uint32_t width, uint32_t height,
    const svg::components::filter_primitive::Turbulence& primitive,
    const Transform2d& deviceFromFilter) {
  const wgpu::Device& dev = device_.device();
  const double baseFrequencyX = boundedTurbulenceFrequency(primitive.baseFrequencyX);
  const double baseFrequencyY = boundedTurbulenceFrequency(primitive.baseFrequencyY);
  const double seed = boundedTurbulenceSeed(primitive.seed);

  // Negative baseFrequency is invalid per SVG Filter Effects §15.20.3; produce transparent black
  // (matching resvg/tiny-skia behavior).
  if (baseFrequencyX < 0.0 || baseFrequencyY < 0.0) {
    return createTransparentIntermediateTexture(arena, width, height,
                                                "FilterTurbulenceTransparent");
  }

  wgpu::Texture output =
      createIntermediateTexture(arena, dev, width, height, "FilterTurbulenceOutput");
  if (!output) {
    return {};
  }

  TurbulenceParams params{};
  params.baseFreqX = static_cast<float>(baseFrequencyX);
  params.baseFreqY = static_cast<float>(baseFrequencyY);
  params.numOctaves = primitive.numOctaves;
  params.seed = boundedRoundedInt32(seed, std::numeric_limits<int32_t>::min(),
                                    std::numeric_limits<int32_t>::max());
  params.stitchTiles = primitive.stitchTiles ? 1u : 0u;
  params.typeFlag =
      primitive.type == svg::components::filter_primitive::Turbulence::Type::Turbulence ? 1u : 0u;
  // Tile dimensions in user space (for stitchTiles).
  params.tileWidth = static_cast<float>(width);
  params.tileHeight = static_cast<float>(height);

  const double determinant = deviceFromFilter.determinant();
  if (NearZero(determinant, 1e-12)) {
    params.filterFromDeviceA = 1.0f;
    params.filterFromDeviceB = 0.0f;
    params.filterFromDeviceC = 0.0f;
    params.filterFromDeviceD = 1.0f;
  } else {
    const Transform2d filterFromDevice = deviceFromFilter.inverse();
    params.filterFromDeviceA = static_cast<float>(filterFromDevice.data[0]);
    params.filterFromDeviceB = static_cast<float>(filterFromDevice.data[2]);
    params.filterFromDeviceC = static_cast<float>(filterFromDevice.data[1]);
    params.filterFromDeviceD = static_cast<float>(filterFromDevice.data[3]);
  }

  // Generate permutation + gradient tables from the seed.
  TurbulenceTables tables{};
  generateTurbulenceTables(seed, tables);

  // Upload params as storage buffer.
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("TurbulenceParamsStorage");
  bufDesc.size = sizeof(TurbulenceParams);
  bufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer paramsBuffer = arena.createBuffer(dev, bufDesc);
  device_.queue().writeBuffer(paramsBuffer, 0, &params, sizeof(params));
  device_.countBufferWrite(sizeof(params));

  // Upload tables as storage buffer.
  wgpu::BufferDescriptor tablesBufDesc{};
  tablesBufDesc.label = wgpuLabel("TurbulenceTablesStorage");
  tablesBufDesc.size = sizeof(TurbulenceTables);
  tablesBufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  tablesBufDesc.mappedAtCreation = false;
  wgpu::Buffer tablesBuffer = arena.createBuffer(dev, tablesBufDesc);
  device_.queue().writeBuffer(tablesBuffer, 0, &tables, sizeof(tables));
  device_.countBufferWrite(sizeof(tables));

  ScopedWgpuHandle<wgpu::TextureView> outputView(output.createView());

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = outputView.get();
  bgEntries[1].binding = 1;
  bgEntries[1].buffer = paramsBuffer;
  bgEntries[1].offset = 0;
  bgEntries[1].size = sizeof(TurbulenceParams);
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = tablesBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(TurbulenceTables);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterTurbulenceBindGroup");
  bgDesc.layout = turbulenceBindGroupLayout_.get();
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup(dev.createBindGroup(bgDesc));
  device_.countBindGroup();

  wgpu::CommandEncoder& encoder = arena.commandEncoder();

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterTurbulencePass");
  ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(encoder.beginComputePass(passDesc));
  pass.get().setPipeline(turbulencePipeline_.get());
  pass.get().setBindGroup(0, bindGroup.get(), 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.get().dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.get().end();
  pass.reset();

  return output;
}

wgpu::Texture GeodeFilterEngine::applyDisplacementMap(
    FilterResourceArena& arena, const wgpu::Texture& in1, const wgpu::Texture& in2,
    const svg::components::filter_primitive::DisplacementMap& primitive, double pixelScale) {
  if (!in1 || !in2) {
    return {};
  }

  const wgpu::Device& dev = device_.device();
  const uint32_t width = in1.getWidth();
  const uint32_t height = in1.getHeight();

  wgpu::Texture output =
      createIntermediateTexture(arena, dev, width, height, "FilterDisplacementMapOutput");
  if (!output) {
    return {};
  }

  using Channel = svg::components::filter_primitive::DisplacementMap::Channel;
  auto toIndex = [](Channel ch) -> uint32_t {
    switch (ch) {
      case Channel::R: return 0;
      case Channel::G: return 1;
      case Channel::B: return 2;
      case Channel::A: return 3;
    }
    return 3;
  };

  DisplacementParams params{};
  params.scale = static_cast<float>(pixelScale);
  params.xChannel = toIndex(primitive.xChannelSelector);
  params.yChannel = toIndex(primitive.yChannelSelector);
  params.pad = 0;

  auto uniformBuffer = writeUniformSlot(*resourceCache_, device_, &params, sizeof(params));
  if (!uniformBuffer.buffer) {
    return {};
  }

  dispatchTwoInputUniform(arena, device_, displacementMapBindGroupLayout_.get(),
                          displacementMapPipeline_.get(), in1, in2, output, uniformBuffer.buffer,
                          uniformBuffer.offset, sizeof(DisplacementParams),
                          "FilterDisplacementMapPass");
  return output;
}

namespace {

/// Fill common light-source fields into the params struct fields starting at
/// the given pointers. This avoids duplicating the logic between diffuse and specular.
void fillLightParams(const svg::components::filter_primitive::LightSource& light,
                     const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter,
                     uint32_t* lightType, float* azimuthRad, float* elevationRad, float* lightX,
                     float* lightY, float* lightZ, float* userLightX, float* userLightY,
                     float* userLightZ, float* pointsAtX, float* pointsAtY, float* pointsAtZ,
                     float* userPointsAtX, float* userPointsAtY, float* userPointsAtZ,
                     float* spotExponent, float* coneAngleRad, uint32_t* hasConeAngle,
                     float* pixelToUser0, float* pixelToUser1, float* pixelToUser2,
                     float* pixelToUser3, float* pixelToUser4, float* pixelToUser5,
                     uint32_t* hasShear) {
  using LT = svg::components::filter_primitive::LightSource::Type;
  switch (light.type) {
    case LT::Distant: *lightType = 0; break;
    case LT::Point: *lightType = 1; break;
    case LT::Spot: *lightType = 2; break;
  }

  constexpr double kDegToRad = std::numbers::pi / 180.0;
  *azimuthRad = static_cast<float>(light.azimuth * kDegToRad);
  *elevationRad = static_cast<float>(light.elevation * kDegToRad);

  const bool isOBB = graph.primitiveUnits == svg::PrimitiveUnits::ObjectBoundingBox &&
                     graph.elementBoundingBox.has_value();
  const double bboxW = isOBB ? graph.elementBoundingBox->width() : 1.0;
  const double bboxH = isOBB ? graph.elementBoundingBox->height() : 1.0;
  const double bboxX = isOBB ? graph.elementBoundingBox->topLeft.x : 0.0;
  const double bboxY = isOBB ? graph.elementBoundingBox->topLeft.y : 0.0;

  double userX = light.x;
  double userY = light.y;
  double userZ = light.z;
  double userPointsAtXValue = light.pointsAtX;
  double userPointsAtYValue = light.pointsAtY;
  double userPointsAtZValue = light.pointsAtZ;
  if (isOBB) {
    userX = bboxX + light.x * bboxW;
    userY = bboxY + light.y * bboxH;
    userZ = light.z * bboxH;
    userPointsAtXValue = bboxX + light.pointsAtX * bboxW;
    userPointsAtYValue = bboxY + light.pointsAtY * bboxH;
    userPointsAtZValue = light.pointsAtZ * bboxH;
  }

  const Vector2d lightPixel = deviceFromFilter.transformPosition(Vector2d(userX, userY));
  const Vector2d pointsAtPixel =
      deviceFromFilter.transformPosition(Vector2d(userPointsAtXValue, userPointsAtYValue));
  const Vector2d sx = deviceFromFilter.transformPosition(Vector2d(1, 0)) -
                      deviceFromFilter.transformPosition(Vector2d(0, 0));
  const Vector2d sy = deviceFromFilter.transformPosition(Vector2d(0, 1)) -
                      deviceFromFilter.transformPosition(Vector2d(0, 0));
  const double pixelScale =
      std::sqrt((sx.x * sx.x + sx.y * sx.y + sy.x * sy.x + sy.y * sy.y) / 2.0);

  *lightX = static_cast<float>(lightPixel.x);
  *lightY = static_cast<float>(lightPixel.y);
  *lightZ = static_cast<float>(userZ * pixelScale);
  *userLightX = static_cast<float>(userX);
  *userLightY = static_cast<float>(userY);
  *userLightZ = static_cast<float>(userZ);
  *pointsAtX = static_cast<float>(pointsAtPixel.x);
  *pointsAtY = static_cast<float>(pointsAtPixel.y);
  *pointsAtZ = static_cast<float>(userPointsAtZValue * pixelScale);
  *userPointsAtX = static_cast<float>(userPointsAtXValue);
  *userPointsAtY = static_cast<float>(userPointsAtYValue);
  *userPointsAtZ = static_cast<float>(userPointsAtZValue);
  *spotExponent = static_cast<float>(light.spotExponent);
  *coneAngleRad = light.limitingConeAngle.has_value()
                      ? static_cast<float>(light.limitingConeAngle.value() * kDegToRad)
                      : 0.0f;
  *hasConeAngle = light.limitingConeAngle.has_value() ? 1u : 0u;

  const Transform2d inv = deviceFromFilter.inverse();
  *pixelToUser0 = static_cast<float>(inv.data[0]);
  *pixelToUser1 = static_cast<float>(inv.data[2]);
  *pixelToUser2 = static_cast<float>(inv.data[4]);
  *pixelToUser3 = static_cast<float>(inv.data[1]);
  *pixelToUser4 = static_cast<float>(inv.data[3]);
  *pixelToUser5 = static_cast<float>(inv.data[5]);

  const double a = deviceFromFilter.data[0];
  const double b = deviceFromFilter.data[1];
  const double c = deviceFromFilter.data[2];
  const double d = deviceFromFilter.data[3];
  const double dot = a * c + b * d;
  const double lenSq1 = a * a + b * b;
  const double lenSq2 = c * c + d * d;
  *hasShear = dot * dot > 0.0003 * lenSq1 * lenSq2 ? 1u : 0u;
}

}  // namespace

wgpu::Texture GeodeFilterEngine::applyDiffuseLighting(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::DiffuseLighting& primitive,
    const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter,
    const Box2d& sampleSubregion, bool linearRGB) {
  if (!input) {
    return {};
  }

  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // Per Filter Effects §15.9, a lighting primitive with no child light source has
  // no defined light and produces transparent-black output. The CPU path matches
  // this (FilterGraphExecutor substitutes a default-constructed graph Image, which
  // tiny-skia renders as transparent). Skip the lighting dispatch entirely rather
  // than fabricating a head-on distant light (which would render a lit surface).
  if (!primitive.light.has_value()) {
    return createTransparentIntermediateTexture(arena, width, height,
                                                "FilterDiffuseLightingTransparent");
  }

  wgpu::Texture output =
      createIntermediateTexture(arena, dev, width, height, "FilterDiffuseLightingOutput");
  if (!output) {
    return {};
  }

  DiffuseLightingParams params{};
  params.surfaceScale = static_cast<float>(primitive.surfaceScale);
  params.diffuseConstant = static_cast<float>(primitive.diffuseConstant);
  params.pad0 = 0.0f;
  params.pad1 = 0.0f;

  // feDiffuseLighting reads the input *alpha* as a height map (color-space-
  // independent) and modulates the light color. In linearRGB, tiny-skia converts
  // the light color sRGB→linear and the lit output linear→sRGB; match it.
  const css::RGBA rgba = primitive.lightingColor.asRGBA();
  params.lightR = static_cast<float>(rgba.r) / 255.0f;
  params.lightG = static_cast<float>(rgba.g) / 255.0f;
  params.lightB = static_cast<float>(rgba.b) / 255.0f;
  if (linearRGB) {
    params.lightR = srgbToLinearChannel(params.lightR);
    params.lightG = srgbToLinearChannel(params.lightG);
    params.lightB = srgbToLinearChannel(params.lightB);
  }

  fillLightParams(primitive.light.value(), graph, deviceFromFilter, &params.lightType,
                  &params.azimuthRad, &params.elevationRad, &params.lightX, &params.lightY,
                  &params.lightZ, &params.userLightX, &params.userLightY, &params.userLightZ,
                  &params.pointsAtX, &params.pointsAtY, &params.pointsAtZ, &params.userPointsAtX,
                  &params.userPointsAtY, &params.userPointsAtZ, &params.spotExponent,
                  &params.coneAngleRad, &params.hasConeAngle, &params.pixelToUser0,
                  &params.pixelToUser1, &params.pixelToUser2, &params.pixelToUser3,
                  &params.pixelToUser4, &params.pixelToUser5, &params.hasShear);

  const Box2d samplePixels = deviceFromFilter.transformBox(sampleSubregion);
  params.sampleMinX = boundedFloorInt32(samplePixels.topLeft.x, 0, static_cast<int32_t>(width) - 1);
  params.sampleMinY =
      boundedFloorInt32(samplePixels.topLeft.y, 0, static_cast<int32_t>(height) - 1);
  params.sampleMaxX =
      boundedCeilInt32(samplePixels.bottomRight.x, 1, static_cast<int32_t>(width)) - 1;
  params.sampleMaxY =
      boundedCeilInt32(samplePixels.bottomRight.y, 1, static_cast<int32_t>(height)) - 1;

  // Upload as storage buffer.
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("DiffuseLightingParamsStorage");
  bufDesc.size = sizeof(DiffuseLightingParams);
  bufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer paramsBuffer = arena.createBuffer(dev, bufDesc);
  device_.queue().writeBuffer(paramsBuffer, 0, &params, sizeof(params));
  device_.countBufferWrite(sizeof(params));

  // Build bind group.
  ScopedWgpuHandle<wgpu::TextureView> inputView(input.createView());
  ScopedWgpuHandle<wgpu::TextureView> outputView(output.createView());

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inputView.get();
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outputView.get();
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = paramsBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(DiffuseLightingParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterDiffuseLightingBindGroup");
  bgDesc.layout = diffuseLightingBindGroupLayout_.get();
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup(dev.createBindGroup(bgDesc));
  device_.countBindGroup();

  wgpu::CommandEncoder& encoder = arena.commandEncoder();

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterDiffuseLightingPass");
  ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(encoder.beginComputePass(passDesc));
  pass.get().setPipeline(diffuseLightingPipeline_.get());
  pass.get().setBindGroup(0, bindGroup.get(), 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.get().dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.get().end();
  pass.reset();

  return output;
}

wgpu::Texture GeodeFilterEngine::applySpecularLighting(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::SpecularLighting& primitive,
    const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter,
    const Box2d& sampleSubregion, bool linearRGB) {
  if (!input) {
    return {};
  }

  const wgpu::Device& dev = device_.device();
  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // Per SVG spec, specularExponent must be in [1, 128]: values < 1 produce
  // transparent output, values > 128 clamp to 128 (matches tiny-skia).
  if (primitive.specularExponent < 1.0) {
    return createTransparentIntermediateTexture(arena, width, height,
                                                "FilterSpecularLightingTransparent");
  }

  wgpu::Texture output =
      createIntermediateTexture(arena, dev, width, height, "FilterSpecularLightingOutput");
  if (!output) {
    return {};
  }

  SpecularLightingParams params{};
  params.surfaceScale = static_cast<float>(primitive.surfaceScale);
  params.specularConstant = static_cast<float>(primitive.specularConstant);
  params.specularExponent = static_cast<float>(std::min(primitive.specularExponent, 128.0));
  params.pad0 = 0.0f;

  // feSpecularLighting: in linearRGB tiny-skia converts the light color sRGB→
  // linear and the output linear→sRGB; match it. (Input alpha = height map.)
  const css::RGBA rgba = primitive.lightingColor.asRGBA();
  params.lightR = static_cast<float>(rgba.r) / 255.0f;
  params.lightG = static_cast<float>(rgba.g) / 255.0f;
  params.lightB = static_cast<float>(rgba.b) / 255.0f;
  if (linearRGB) {
    params.lightR = srgbToLinearChannel(params.lightR);
    params.lightG = srgbToLinearChannel(params.lightG);
    params.lightB = srgbToLinearChannel(params.lightB);
  }

  if (primitive.light.has_value()) {
    fillLightParams(primitive.light.value(), graph, deviceFromFilter, &params.lightType,
                    &params.azimuthRad, &params.elevationRad, &params.lightX, &params.lightY,
                    &params.lightZ, &params.userLightX, &params.userLightY, &params.userLightZ,
                    &params.pointsAtX, &params.pointsAtY, &params.pointsAtZ, &params.userPointsAtX,
                    &params.userPointsAtY, &params.userPointsAtZ, &params.spotExponent,
                    &params.coneAngleRad, &params.hasConeAngle, &params.pixelToUser0,
                    &params.pixelToUser1, &params.pixelToUser2, &params.pixelToUser3,
                    &params.pixelToUser4, &params.pixelToUser5, &params.hasShear);
  } else {
    params.lightType = 0;
    params.azimuthRad = 0.0f;
    params.elevationRad = 0.0f;
  }

  const Box2d samplePixels = deviceFromFilter.transformBox(sampleSubregion);
  params.sampleMinX = boundedFloorInt32(samplePixels.topLeft.x, 0, static_cast<int32_t>(width) - 1);
  params.sampleMinY =
      boundedFloorInt32(samplePixels.topLeft.y, 0, static_cast<int32_t>(height) - 1);
  params.sampleMaxX =
      boundedCeilInt32(samplePixels.bottomRight.x, 1, static_cast<int32_t>(width)) - 1;
  params.sampleMaxY =
      boundedCeilInt32(samplePixels.bottomRight.y, 1, static_cast<int32_t>(height)) - 1;

  // Upload as storage buffer.
  wgpu::BufferDescriptor bufDesc{};
  bufDesc.label = wgpuLabel("SpecularLightingParamsStorage");
  bufDesc.size = sizeof(SpecularLightingParams);
  bufDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  bufDesc.mappedAtCreation = false;
  wgpu::Buffer paramsBuffer = arena.createBuffer(dev, bufDesc);
  device_.queue().writeBuffer(paramsBuffer, 0, &params, sizeof(params));
  device_.countBufferWrite(sizeof(params));

  // Build bind group.
  ScopedWgpuHandle<wgpu::TextureView> inputView(input.createView());
  ScopedWgpuHandle<wgpu::TextureView> outputView(output.createView());

  wgpu::BindGroupEntry bgEntries[3]{};
  bgEntries[0].binding = 0;
  bgEntries[0].textureView = inputView.get();
  bgEntries[1].binding = 1;
  bgEntries[1].textureView = outputView.get();
  bgEntries[2].binding = 2;
  bgEntries[2].buffer = paramsBuffer;
  bgEntries[2].offset = 0;
  bgEntries[2].size = sizeof(SpecularLightingParams);

  wgpu::BindGroupDescriptor bgDesc{};
  bgDesc.label = wgpuLabel("FilterSpecularLightingBindGroup");
  bgDesc.layout = specularLightingBindGroupLayout_.get();
  bgDesc.entryCount = 3;
  bgDesc.entries = bgEntries;
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup(dev.createBindGroup(bgDesc));
  device_.countBindGroup();

  wgpu::CommandEncoder& encoder = arena.commandEncoder();

  wgpu::ComputePassDescriptor passDesc{};
  passDesc.label = wgpuLabel("FilterSpecularLightingPass");
  ScopedWgpuHandle<wgpu::ComputePassEncoder> pass(encoder.beginComputePass(passDesc));
  pass.get().setPipeline(specularLightingPipeline_.get());
  pass.get().setBindGroup(0, bindGroup.get(), 0, nullptr);

  const uint32_t workgroupsX = (width + 7) / 8;
  const uint32_t workgroupsY = (height + 7) / 8;
  pass.get().dispatchWorkgroups(workgroupsX, workgroupsY, 1);
  pass.get().end();
  pass.reset();

  return output;
}

wgpu::Texture GeodeFilterEngine::applyDropShadow(
    FilterResourceArena& arena, const wgpu::Texture& input,
    const svg::components::filter_primitive::DropShadow& primitive, double pixelStdDevX,
    double pixelStdDevY, double pixelDx, double pixelDy) {
  if (!input) {
    return {};
  }

  const uint32_t width = input.getWidth();
  const uint32_t height = input.getHeight();

  // Blur the input first (if any deviation). The compose shader reads the
  // blurred texture's alpha at (coord - offset); blurring RGB at the same
  // time is free because we already have to walk the taps.
  wgpu::Texture blurred =
      applyGaussianBlur(arena, input, pixelStdDevX, pixelStdDevY, /*edgeMode=*/0);
  if (!blurred) {
    return {};
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterDropShadowOutput",
      {width, height},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (!output) {
    return {};
  }

  const css::RGBA rgba = primitive.floodColor.asRGBA();
  const float floodA = (static_cast<float>(rgba.a) / 255.0f) *
                       static_cast<float>(std::clamp(primitive.floodOpacity, 0.0, 1.0));

  DropShadowParams params{};
  // Straight-alpha flood color; the shader premultiplies by the blurred
  // source's alpha per-pixel rather than baking it in here.
  params.color[0] = static_cast<float>(rgba.r) / 255.0f;
  params.color[1] = static_cast<float>(rgba.g) / 255.0f;
  params.color[2] = static_cast<float>(rgba.b) / 255.0f;
  if (arena.isLinearRgb(input)) {
    for (size_t channel = 0; channel < 3; ++channel) {
      params.color[channel] = srgbToLinearChannel(params.color[channel]);
    }
  }
  params.color[3] = floodA;
  // Keep the software path's rounding boundary before converting the uniform to float.
  params.dx = static_cast<float>(std::round(pixelDx));
  params.dy = static_cast<float>(std::round(pixelDy));
  params.pad0 = 0;
  params.pad1 = 0;

  if (!dispatchRuntimeTwoInput(arena, dropShadowProgram_, input, blurred, *output, {width, height},
                               UniformBytes(params), "FilterDropShadowPass",
                               gpu::shader::programs::kDropShadowWorkgroupSize)) {
    return {};
  }
  return device_.adapterDevice().wgpuTextureOf(*output);
}

bool HasSafeFilterImageSource(const svg::components::filter_primitive::Image& primitive,
                              uint32_t maximumTextureDimension) {
  return primitive.imageData &&
         svg::HasExactRgbaPayload(*primitive.imageData, primitive.imageWidth,
                                  primitive.imageHeight) &&
         static_cast<uint32_t>(primitive.imageWidth) <= maximumTextureDimension &&
         static_cast<uint32_t>(primitive.imageHeight) <= maximumTextureDimension &&
         static_cast<uint32_t>(primitive.imageWidth) <= std::numeric_limits<uint32_t>::max() / 4u;
}

std::optional<ImageParams> CreateFragmentImageParams(
    const svg::components::filter_primitive::Image& primitive,
    const svg::components::FilterGraph& graph, const Transform2d& deviceFromFilter) {
  if (!primitive.isFragmentReference) {
    return std::nullopt;
  }
  ImageParams params{};
  const bool hasRotation =
      !NearZero(deviceFromFilter.data[1], 1e-6) || !NearZero(deviceFromFilter.data[2], 1e-6);
  if (hasRotation && !NearZero(deviceFromFilter.determinant(), 1e-12)) {
    const double scaleX = graph.userToPixelScale.x;
    const double scaleY = graph.userToPixelScale.y;
    const Transform2d viewBoxScaleInv = Transform2d::Scale(
        NearZero(scaleX, 1e-12) ? 1.0 : 1.0 / scaleX, NearZero(scaleY, 1e-12) ? 1.0 : 1.0 / scaleY);
    const Transform2d regionOffset = Transform2d::Translate(primitive.fragmentRegionTopLeft.x,
                                                            primitive.fragmentRegionTopLeft.y);
    const Transform2d deviceFromFragment = viewBoxScaleInv * regionOffset * deviceFromFilter;
    const Transform2d fragmentFromDevice = deviceFromFragment.inverse();
    params.m00 = static_cast<float>(fragmentFromDevice.data[0]);
    params.m01 = static_cast<float>(fragmentFromDevice.data[2]);
    params.m02 = static_cast<float>(fragmentFromDevice.data[4]);
    params.m10 = static_cast<float>(fragmentFromDevice.data[1]);
    params.m11 = static_cast<float>(fragmentFromDevice.data[3]);
    params.m12 = static_cast<float>(fragmentFromDevice.data[5]);
    params.pixelatedScaleX =
        static_cast<float>(deviceFromFragment.transformVector(Vector2d(1.0, 0.0)).length());
    params.pixelatedScaleY =
        static_cast<float>(deviceFromFragment.transformVector(Vector2d(0.0, 1.0)).length());
  } else {
    const double scaleX = graph.userToPixelScale.x > 0.0 ? graph.userToPixelScale.x : 1.0;
    const double scaleY = graph.userToPixelScale.y > 0.0 ? graph.userToPixelScale.y : 1.0;
    params.m00 = 1.0f;
    params.m02 = static_cast<float>(-primitive.fragmentRegionTopLeft.x * scaleX);
    params.m11 = 1.0f;
    params.m12 = static_cast<float>(-primitive.fragmentRegionTopLeft.y * scaleY);
    params.pixelatedScaleX = 1.0f;
    params.pixelatedScaleY = 1.0f;
  }
  params.samplingMode = ImageSamplingMode(primitive.imageRendering);
  return params;
}

wgpu::Texture GeodeFilterEngine::renderTransparentImage(FilterResourceArena& arena,
                                                        const wgpu::Texture& output) {
  wgpu::Texture emptyTex = arena.createTexture(gpu::TextureDescriptor{
      "FilterImageEmptySource", gpu::Extent2d{1, 1}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  if (!emptyTex) {
    return {};
  }
  const uint8_t zero[4] = {0, 0, 0, 0};
  wgpu::TexelCopyTextureInfo dstInfo{};
  dstInfo.texture = emptyTex;
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = 4;
  layout.rowsPerImage = 1;
  wgpu::Extent3D extent = {1, 1, 1};
  device_.queue().writeTexture(dstInfo, zero, 4, layout, extent);
  device_.countTextureWrite(4);

  ImageParams params{};
  params.m02 = -1000.0f;
  params.m12 = -1000.0f;
  auto ub = writeUniformSlot(*resourceCache_, device_, &params, sizeof(params));
  if (!ub.buffer) {
    return {};
  }
  dispatchInputOutputUniform(arena, device_, imageBindGroupLayout_.get(), imagePipeline_.get(),
                             emptyTex, output, ub.buffer, ub.offset, sizeof(ImageParams),
                             "FilterImageEmptyPass");
  return output;
}

namespace {

Vector2d FilterImageAlignment(svg::PreserveAspectRatio::Align align) {
  using Align = svg::PreserveAspectRatio::Align;
  switch (align) {
    case Align::None:
    case Align::XMinYMin: return {0.0, 0.0};
    case Align::XMidYMin: return {0.5, 0.0};
    case Align::XMaxYMin: return {1.0, 0.0};
    case Align::XMinYMid: return {0.0, 0.5};
    case Align::XMidYMid: return {0.5, 0.5};
    case Align::XMaxYMid: return {1.0, 0.5};
    case Align::XMinYMax: return {0.0, 1.0};
    case Align::XMidYMax: return {0.5, 1.0};
    case Align::XMaxYMax: return {1.0, 1.0};
  }
  return {0.0, 0.0};
}

ImageParams CreateRasterFilterImageParams(const svg::components::filter_primitive::Image& primitive,
                                          const svg::components::FilterGraph& graph,
                                          const Box2d& placementRegionUser) {
  const uint32_t imgW = static_cast<uint32_t>(primitive.imageWidth);
  const uint32_t imgH = static_cast<uint32_t>(primitive.imageHeight);
  // Work out the placement rectangle in output-pixel coordinates. Prefer
  // the node's primitive subregion when given; fall back to the filter
  // region so the image covers the full primitive area.
  const double scaleX = graph.userToPixelScale.x > 0.0 ? graph.userToPixelScale.x : 1.0;
  const double scaleY = graph.userToPixelScale.y > 0.0 ? graph.userToPixelScale.y : 1.0;

  // The placement rectangle arrives already resolved in user space (percent/OBB
  // units handled, absent x/y/width/height defaulted to the filter region by the
  // caller). Project it into output-pixel coordinates. The previous in-function
  // logic only honored the subregion when all four attributes were present and
  // otherwise meet-fit the image to the whole filter region and cropped it,
  // which broke partial subregions (e.g. `feImage x=.. width=..`) and percent
  // units (resvg `with-subregion-{1,2,3}`).
  double regionX = placementRegionUser.topLeft.x * scaleX;
  double regionY = placementRegionUser.topLeft.y * scaleY;
  double regionW = placementRegionUser.width() * scaleX;
  double regionH = placementRegionUser.height() * scaleY;

  if (regionW <= 0.0 || regionH <= 0.0) {
    regionW = static_cast<double>(imgW);
    regionH = static_cast<double>(imgH);
  }

  // Compute the src-from-dst transform (image-from-output). For a
  // straight image placement at (regionX, regionY) with size (regionW,
  // regionH) and source size (imgW, imgH), the transform is:
  //   src.x = (dst.x - regionX) * imgW / regionW
  //   src.y = (dst.y - regionY) * imgH / regionH
  //
  // preserveAspectRatio: meet / slice scales isotropically; 'none'
  // scales independently as above. The simple non-preserveAR case covers
  // the common resvg test cases - the preserveAspectRatio computation
  // mirrors the CPU feImage path's convention (fit box to subregion).
  double scaleImgX = regionW > 0.0 ? static_cast<double>(imgW) / regionW : 1.0;
  double scaleImgY = regionH > 0.0 ? static_cast<double>(imgH) / regionH : 1.0;
  double offsetX = -regionX * scaleImgX;
  double offsetY = -regionY * scaleImgY;

  using PAR = svg::PreserveAspectRatio;
  if (primitive.preserveAspectRatio.align != PAR::Align::None) {
    // Uniform scale per preserveAspectRatio semantics. Compute the
    // meet/slice scale and the alignment offsets, then invert to build
    // image-from-output.
    const double scaleMeet =
        std::min(regionW / static_cast<double>(imgW), regionH / static_cast<double>(imgH));
    const double scaleSlice =
        std::max(regionW / static_cast<double>(imgW), regionH / static_cast<double>(imgH));
    const double s = primitive.preserveAspectRatio.meetOrSlice == PAR::MeetOrSlice::Slice
                         ? scaleSlice
                         : scaleMeet;
    const double drawnW = static_cast<double>(imgW) * s;
    const double drawnH = static_cast<double>(imgH) * s;

    const Vector2d alignment = FilterImageAlignment(primitive.preserveAspectRatio.align);
    const double drawX = regionX + (regionW - drawnW) * alignment.x;
    const double drawY = regionY + (regionH - drawnH) * alignment.y;

    scaleImgX = 1.0 / s;
    scaleImgY = 1.0 / s;
    offsetX = -drawX / s;
    offsetY = -drawY / s;
  }

  ImageParams params{};
  params.m00 = static_cast<float>(scaleImgX);
  params.m01 = 0.0f;
  params.m02 = static_cast<float>(offsetX);
  params.m10 = 0.0f;
  params.m11 = static_cast<float>(scaleImgY);
  params.m12 = static_cast<float>(offsetY);
  params.samplingMode = ImageSamplingMode(primitive.imageRendering);
  params.pixelatedScaleX = static_cast<float>(1.0 / std::abs(scaleImgX));
  params.pixelatedScaleY = static_cast<float>(1.0 / std::abs(scaleImgY));
  params.pad1 = 0;
  return params;
}

}  // namespace

wgpu::Texture GeodeFilterEngine::applyImage(
    FilterResourceArena& arena, const svg::components::filter_primitive::Image& primitive,
    uint32_t width, uint32_t height, const svg::components::FilterGraph& graph,
    const svg::components::FilterNode& node, const Transform2d& deviceFromFilter,
    const Box2d& placementRegionUser) {
  const wgpu::Device& dev = device_.device();

  const bool hasSafeTextureExtent =
      HasSafeFilterImageSource(primitive, device_.maxTextureDimension2D());
  wgpu::Texture output = createIntermediateTexture(arena, dev, width, height, "FilterImageOutput");
  if (!output) {
    return {};
  }

  // Empty, malformed, degenerate, or device-oversized sources remain transparent.
  if (!hasSafeTextureExtent) {
    return renderTransparentImage(arena, output);
  }

  // Upload the image's straight-alpha RGBA pixels as a premultiplied
  // texture - Geode operates in premultiplied throughout the filter graph
  // (consistent with feFlood / feMerge).
  const uint32_t imgW = static_cast<uint32_t>(primitive.imageWidth);
  const uint32_t imgH = static_cast<uint32_t>(primitive.imageHeight);
  const std::vector<uint8_t> premul = svg::PremultiplyRgba(*primitive.imageData);

  wgpu::Texture imgTex = arena.createTexture(gpu::TextureDescriptor{
      "FilterImageSource", gpu::Extent2d{imgW, imgH}, gpu::TextureFormat::RGBA8Unorm,
      gpu::TextureUsage::Sampled | gpu::TextureUsage::CopyDst});
  if (!imgTex) {
    return {};
  }

  wgpu::TexelCopyTextureInfo dstInfo{};
  dstInfo.texture = imgTex;
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = imgW * 4u;
  layout.rowsPerImage = imgH;
  wgpu::Extent3D extent = {imgW, imgH, 1};
  device_.queue().writeTexture(dstInfo, premul.data(), premul.size(), layout, extent);
  device_.countTextureWrite(premul.size());

  const std::optional<ImageParams> fragmentParams =
      CreateFragmentImageParams(primitive, graph, deviceFromFilter);
  const ImageParams params =
      fragmentParams ? *fragmentParams
                     : CreateRasterFilterImageParams(primitive, graph, placementRegionUser);
  auto uniformBuffer = writeUniformSlot(*resourceCache_, device_, &params, sizeof(params));
  if (!uniformBuffer.buffer) {
    return {};
  }

  dispatchInputOutputUniform(arena, device_, imageBindGroupLayout_.get(), imagePipeline_.get(),
                             imgTex, output, uniformBuffer.buffer, uniformBuffer.offset,
                             sizeof(ImageParams),
                             fragmentParams ? "FilterImageFragRefPass" : "FilterImagePass");
  return output;
}

wgpu::Texture GeodeFilterEngine::applyTile(FilterResourceArena& arena, const wgpu::Texture& input,
                                           int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH) {
  if (!input) {
    return {};
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterTileOutput", gpu::Extent2d{input.getWidth(), input.getHeight()},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }
  const TileParams params{srcX, srcY, srcW, srcH};
  if (!dispatchRuntimeInputOutputParameters(arena, tileProgram_, input, *output,
                                            UniformBytes(params), "FilterTilePass",
                                            gpu::shader::programs::kTileWorkgroupSize)) {
    return {};
  }
  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applySubregionClip(FilterResourceArena& arena,
                                                    const wgpu::Texture& input,
                                                    const Transform2d& filterFromDevice,
                                                    double usrX0, double usrY0, double usrX1,
                                                    double usrY1, bool resolve) {
  if (!input || (resolve && !colorTransferTable_.isValid())) {
    return {};
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterSubregionClipOutput", gpu::Extent2d{input.getWidth(), input.getHeight()},
      resolve ? gpu::TextureFormat::RGBA8Unorm : gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  SubregionClipParams params{};
  params.invA = static_cast<float>(filterFromDevice.data[0]);
  params.invB = static_cast<float>(filterFromDevice.data[1]);
  params.invC = static_cast<float>(filterFromDevice.data[2]);
  params.invD = static_cast<float>(filterFromDevice.data[3]);
  params.invE = static_cast<float>(filterFromDevice.data[4]);
  params.invF = static_cast<float>(filterFromDevice.data[5]);
  params.userX0 = static_cast<float>(usrX0);
  params.userY0 = static_cast<float>(usrY0);
  params.userX1 = static_cast<float>(usrX1);
  params.userY1 = static_cast<float>(usrY1);
  params.pad0 = resolve && arena.isLinearRgb(input) ? 1u : 0u;
  params.pad1 = 0;

  if (!dispatchRuntimeInputOutputParameters(
          arena, resolve ? filterResolveProgram_ : subregionClipProgram_, input, *output,
          UniformBytes(params), "FilterSubregionClipPass",
          gpu::shader::programs::kSubregionClipWorkgroupSize,
          resolve ? &colorTransferTable_ : nullptr)) {
    return {};
  }

  return device_.adapterDevice().wgpuTextureOf(*output);
}

wgpu::Texture GeodeFilterEngine::applyColorSpaceConversion(FilterResourceArena& arena,
                                                           const wgpu::Texture& input,
                                                           bool srgbToLinear) {
  if (!input) {
    return {};
  }

  if (const wgpu::Texture converted = arena.convertedTexture(input, srgbToLinear)) {
    return converted;
  }

  const gpu::Texture* output = arena.createRuntimeTexture(gpu::TextureDescriptor{
      "FilterColorSpaceConvertOutput", gpu::Extent2d{input.getWidth(), input.getHeight()},
      gpu::TextureFormat::RGBA32Float,
      gpu::TextureUsage::StorageBinding | gpu::TextureUsage::Sampled | gpu::TextureUsage::CopySrc});
  if (output == nullptr) {
    return {};
  }

  ColorSpaceConvertParams params{};
  params.direction = srgbToLinear ? gpu::shader::programs::kColorSpaceConvertSrgbToLinear
                                  : gpu::shader::programs::kColorSpaceConvertLinearToSrgb;
  params.pad0 = 0;
  params.pad1 = 0;
  params.pad2 = 0;

  if (!colorTransferTable_.isValid() ||
      !dispatchRuntimeInputOutputParameters(arena, colorSpaceConvertProgram_, input, *output,
                                            UniformBytes(params), "FilterColorSpaceConvertPass",
                                            gpu::shader::programs::kColorSpaceConvertWorkgroupSize,
                                            &colorTransferTable_)) {
    return {};
  }

  const wgpu::Texture converted = device_.adapterDevice().wgpuTextureOf(*output);
  arena.rememberConversion(input, converted, srgbToLinear);
  return converted;
}

}  // namespace donner::geode
