#pragma once
/// @file
/// Per-entity GPU residence for Geode's cached encoded-path geometry.
///
/// `GeodePathCacheComponent` keeps the CPU-side `EncodedPath` across
/// frames, but the renderer still re-uploaded that geometry to a fresh
/// per-frame bump arena on every draw - the measured headline cost was a
/// static Ghostscript Tiger writing ~1.44 MB per frame across 2,432
/// `writeBuffer` calls even though the CPU encode cache hit, plus one
/// `createBindGroup` per draw (304/frame).
///
/// This component gives each cached path a persistent GPU buffer that
/// survives frames, so an unchanged document's steady-state frame writes
/// ~zero geometry bytes and re-uses a cached bind group. It lives BESIDE
/// `GeodePathCacheComponent` on the same entity and is removed by the
/// same `ComputedPathComponent` on_update / on_destroy listener
/// (`RendererGeode::Impl::onComputedPathChanged`), so the GPU residence
/// invalidates exactly when the geometry changes. Registry teardown
/// (document close) destroys the component and RAII-frees the buffer -
/// the eviction story for "many distinct documents".
///
/// Lives in `donner::geode` (not `donner::svg::geode`) to match the other
/// Geode types referenced unqualified via `geode::` inside `donner::svg`.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"

namespace donner::geode {

class GeodeDevice;

/**
 * Document-scoped suballocation slab for GPU residence.
 *
 * Per-slot residence previously gave every cached path its own combined GPU
 * buffer, so a first frame allocated one buffer per resident slot (Lion:
 * 133). The slab replaces those with sub-ranges of a small number of
 * growable chunk buffers owned by the document's registry context, cutting
 * first-frame `bufferCreates` to one chunk per growth step (1 to 2 for
 * typical documents).
 *
 * Allocations are bump-placed and returned through a first-fit free list,
 * so a geometry edit that re-uploads a slot reuses the freed range. Each
 * chunk's bytes are accounted in the device's live-resident-bytes gauge on
 * creation and released when the slab is destroyed with its registry. Note
 * the gauge therefore reports slab CAPACITY, not live payload bytes: it
 * grows when a chunk is created and never shrinks on slot reset, which is
 * the right shape for leak detection but over-reads against the previous
 * per-slot accounting.
 *
 * The slab is bound to one device (`GeodeDevice::deviceId()`): a document
 * rendered by a second device gets a fresh slab, and the old chunks are
 * released without touching the old device's objects (WebGPU retains any
 * still-referenced buffers through submitted command buffers).
 *
 * Not thread-safe: allocate/free/beginFrame mutate the free list and bump
 * cursors without locking. The renderer serializes one frame per device at
 * a time, so a document's slab is only touched from one thread.
 */
class GeodeResidentSlab {
public:
  /// One sub-range of a slab chunk.
  struct Allocation {
    wgpu::Buffer buffer;  // Borrowed handle; owned by the slab chunk.
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  /// Create an empty slab bound to `deviceId` (usually the current
  /// renderer's device). Chunks are allocated lazily on first use.
  explicit GeodeResidentSlab(uint64_t deviceId) : owningDeviceId_(deviceId) {}

  ~GeodeResidentSlab() {
    if (gauge_ && accountedBytes_ != 0) {
      gauge_->fetch_sub(accountedBytes_, std::memory_order_relaxed);
    }
  }

  GeodeResidentSlab(const GeodeResidentSlab&) = delete;
  GeodeResidentSlab& operator=(const GeodeResidentSlab&) = delete;

  /// Device this slab's chunks were created on.
  uint64_t owningDeviceId() const { return owningDeviceId_; }

  /// Merge the previous frame's freed ranges into the reusable free list.
  /// Gated on `frameIndex` (the renderer's frame counter) so the merge runs
  /// at most once per frame no matter how many times the renderer touches
  /// the slab: the accessor calls this on every slot lookup, which covers
  /// every draw entry point (full-document draws and multi-document
  /// tile/thumbnail frames alike) without trusting any single call site.
  ///
  /// Freed ranges must NOT be reused within the frame that freed them: a
  /// re-upload that reuses its own just-freed range would overwrite the
  /// geometry before the already-recorded draws of the same frame read it
  /// (the resident slots hold one buffer range per slot, not one buffer per
  /// draw). Merging at the first touch of frame N is safe because ranges
  /// pending at that point were freed no later than frame N-1, whose
  /// command buffer was submitted at its endFrame; a second renderer on the
  /// same device has its own frame counter, and a stale-index skip only
  /// defers the merge (the safe direction). Frames on one device are
  /// serialized by contract, so no merge can run under another renderer's
  /// unsubmitted frame.
  void beginFrame(uint64_t frameIndex) {
    if (frameIndex == lastMergedFrame_) {
      return;
    }
    lastMergedFrame_ = frameIndex;
    for (const FreeRange& range : pendingFrees_) {
      auto it = freeRanges_.begin();
      while (it != freeRanges_.end() &&
             (it->chunk < range.chunk || (it->chunk == range.chunk && it->offset < range.offset))) {
        ++it;
      }
      bool merged = false;
      if (it != freeRanges_.end() && it->chunk == range.chunk &&
          it->offset == range.offset + range.size) {
        it->offset = range.offset;
        it->size += range.size;
        merged = true;
      }
      if (it != freeRanges_.begin()) {
        auto prev = std::prev(it);
        if (prev->chunk == range.chunk && prev->offset + prev->size == range.offset) {
          if (merged) {
            prev->size += it->size;
            freeRanges_.erase(it);
          } else {
            prev->size += range.size;
          }
          continue;
        }
      }
      if (!merged) {
        freeRanges_.insert(it, range);
      }
    }
    pendingFrees_.clear();
  }

  /// Bump- or free-list-allocate `size` bytes aligned to `alignment`.
  /// Creates a new chunk (and counts it) when nothing fits. Returns false
  /// when the device cannot create a buffer.
  bool allocate(GeodeDevice& device, uint64_t size, uint64_t alignment, Allocation& out) {
    if (size == 0) {
      return false;
    }

    // First-fit from the free list (ranges carry their chunk index and
    // stay sorted by chunk then offset).
    for (auto it = freeRanges_.begin(); it != freeRanges_.end(); ++it) {
      const uint64_t start = alignUp(it->offset, alignment);
      if (start + size <= it->offset + it->size) {
        out.buffer = chunks_[it->chunk].buffer.get();
        out.offset = start;
        out.size = size;
        if (start == it->offset) {
          it->offset += size;
          it->size -= size;
          if (it->size == 0) {
            freeRanges_.erase(it);
          }
        } else {
          const uint64_t tail = (it->offset + it->size) - (start + size);
          it->size = start - it->offset;
          if (tail > 0) {
            freeRanges_.insert(std::next(it), FreeRange{it->chunk, start + size, tail});
          }
        }
        return true;
      }
    }

    // Bump-allocate from the newest chunk, growing when it does not fit.
    if (chunks_.empty() || alignUp(chunks_.back().cursor, alignment) + size > chunks_.back().size) {
      uint64_t newSize = chunks_.empty() ? kInitialChunkBytes : chunks_.back().size * 2u;
      while (newSize < alignUp(size, alignment)) {
        newSize *= 2u;
      }
      wgpu::BufferDescriptor desc = {};
      desc.label = wgpuLabel("GeodeResidentSlab");
      desc.size = newSize;
      desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Uniform |
                   wgpu::BufferUsage::CopyDst;
      Chunk chunk;
      chunk.buffer.reset(device.device().createBuffer(desc));
      if (!chunk.buffer) {
        return false;
      }
      device.countBuffer();
      chunk.size = newSize;
      chunks_.push_back(std::move(chunk));
      if (!gauge_) {
        gauge_ = device.residentBytesGauge();
      }
      gauge_->fetch_add(static_cast<int64_t>(newSize), std::memory_order_relaxed);
      accountedBytes_ += static_cast<int64_t>(newSize);
    }

    Chunk& chunk = chunks_.back();
    out.buffer = chunk.buffer.get();
    out.offset = alignUp(chunk.cursor, alignment);
    out.size = size;
    chunk.cursor = out.offset + size;
    return true;
  }

  /// Return an allocation for reuse at the NEXT frame boundary. The
  /// range is not reusable within the current frame: see beginFrame().
  void free(const Allocation& alloc) {
    if (alloc.size == 0) {
      return;
    }
    // Locate the owning chunk.
    size_t chunkIndex = 0;
    for (; chunkIndex < chunks_.size(); ++chunkIndex) {
      if (chunks_[chunkIndex].buffer.get() == alloc.buffer) {
        break;
      }
    }
    if (chunkIndex == chunks_.size()) {
      return;  // Stale allocation (chunk no longer live); ignore.
    }
    pendingFrees_.push_back(FreeRange{chunkIndex, alloc.offset, alloc.size});
  }

private:
  static constexpr uint64_t kInitialChunkBytes = 1u << 20;  // 1 MiB.

  static uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
  }

  struct Chunk {
    ScopedWgpuHandle<wgpu::Buffer> buffer;
    uint64_t size = 0;
    uint64_t cursor = 0;
  };

  struct FreeRange {
    uint64_t chunk = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  uint64_t owningDeviceId_ = 0;
  /// Frame index of the last pending-free merge; ~0 = never merged. See
  /// beginFrame().
  uint64_t lastMergedFrame_ = ~uint64_t{0};
  std::vector<Chunk> chunks_;
  std::vector<FreeRange> freeRanges_;
  std::vector<FreeRange> pendingFrees_;
  std::shared_ptr<std::atomic<int64_t>> gauge_;
  int64_t accountedBytes_ = 0;
};

/// GPU-resident geometry for one cached `EncodedPath` (a fill slot or a
/// stroke slot). Owns a single combined-usage GPU buffer that holds the
/// path's eight analytic dual-ray SSBO regions and per-draw uniform block, plus the cached fill
/// bind group. Bounding vertices live in that uniform and are expanded from vertex_index, so the
/// resident geometry needs no vertex buffer. The buffer
/// and bind group are built once by `GeoEncoder` on first residence and
/// reused every subsequent unchanged frame.
///
/// Move-only (owns wgpu handles). Default-constructed slots are empty;
/// `GeoEncoder::fillPathResident` populates them lazily.
struct GeodeResidentSlot {
  /// Combined Storage|Uniform|CopyDst buffer, borrowed from the owning
  /// `GeodeResidentSlab` chunk. Region offsets are ABSOLUTE buffer
  /// offsets. Layout (each region offset satisfies the binding's
  /// alignment requirement):
  ///   [ bands | curves | hRefs | vBands | vCurves | vRefs | hGrid | vGrid | uniform ]
  wgpu::Buffer buffer;
  /// Slab that owns `buffer`; null until residence is established. Held
  /// by shared_ptr so a device change (which swaps the registry's slab)
  /// cannot leave a slot referencing a destroyed slab: old slots keep the
  /// old slab alive until their own reset drops the reference.
  std::shared_ptr<GeodeResidentSlab> slab;
  /// Whole-slot allocation inside the slab, returned via `free` on reset.
  uint64_t allocationOffset = 0;
  uint64_t allocationSize = 0;

  /// Cached fill bind group. All fourteen bindings reference stable
  /// objects (this slot's `buffer` sub-ranges + device-owned dummy
  /// texture/sampler/identity-instance handles), so it survives frames
  /// and encoders. Rebuilt only when the geometry buffer is
  /// re-allocated.
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup;

  /// A byte sub-range of `buffer`. `size == 0` is never bound directly -
  /// empty SSBO regions reserve a 4-byte zero-filled slot so the shader's
  /// band-count gate keeps them un-dereferenced (matching the per-frame
  /// arena `allocStorageOrDummy` dummy).
  struct Region {
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  Region bands;    ///< Horizontal band SSBO (binding 1).
  Region curves;   ///< Horizontal curve SSBO (binding 2).
  Region hRefs;    ///< Horizontal curve-reference SSBO (binding 12).
  Region vBands;   ///< Vertical band SSBO (binding 8).
  Region vCurves;  ///< Vertical curve SSBO (binding 9).
  Region vRefs;    ///< Vertical curve-reference SSBO (binding 13).
  Region hGrid;    ///< Horizontal band grid (binding 10).
  Region vGrid;    ///< Vertical band grid (binding 11).
  Region uniform;  ///< Per-draw uniform block (binding 0, 256-aligned).

  uint32_t vertexCount = 0;  ///< Triangle-fan draw count generated from vertex_index.

  /// Frame index in which this slot was last drawn via the resident path.
  /// A slot's single uniform buffer + cached bind group can only serve ONE
  /// draw per frame (all draws recorded against a frame's command buffer
  /// read the buffer's final contents at submit time). When the same slot
  /// is drawn again in the same frame at a different transform/color
  /// (markers, non-adjacent repeated `<use>`), the second and later draws
  /// fall back to the per-frame arena path so each gets its own uniform. The
  /// steady-state win is unaffected for the common case of a path drawn
  /// once per frame (Tiger / Lion). Sentinel `~0` means "never drawn".
  uint64_t lastResidentFrame = ~uint64_t{0};

  /// Process-unique id (see `GeodeDevice::deviceId()`) of the device that
  /// created `buffer` / `bindGroup`. A document's ECS residence components
  /// can outlive the device that filled them and later be rendered by a
  /// second `RendererGeode` / `GeodeDevice`; WebGPU rejects a bind group or
  /// buffer from device A inside device B's render pass. When this id does
  /// not match the current device at draw time the slot is treated as
  /// non-resident and re-uploaded onto the current device (the stale handles
  /// are released without touching the old device). `0` means "no device
  /// owns this slot yet".
  uint64_t owningDeviceId = 0;

  /// True once `buffer` + `bindGroup` hold the current encode. Cleared
  /// when the slot is reset or when a re-upload is required.
  bool resident = false;

  /// Identity guard defending against any missed invalidation: the
  /// address of the `EncodedPath` last uploaded plus a cheap size
  /// fingerprint. If either differs at draw time the geometry is
  /// re-uploaded. Component removal is the primary invalidation; this is
  /// belt-and-suspenders for the in-place stroke-slot rebuild path.
  const void* encodedKey = nullptr;
  uint64_t encodedFingerprint = 0;

  /// Bytes last written to the uniform region. A draw whose recomputed
  /// uniform matches this skips the `writeBuffer` entirely (steady-state
  /// static frame => zero buffer writes); a camera/color change rewrites
  /// only this 368-byte region and keeps the cached bind group.
  std::vector<uint8_t> lastUniform;

  GeodeResidentSlot() = default;
  ~GeodeResidentSlot() { reset(); }

  GeodeResidentSlot(const GeodeResidentSlot&) = delete;
  GeodeResidentSlot& operator=(const GeodeResidentSlot&) = delete;
  GeodeResidentSlot(GeodeResidentSlot&&) noexcept = default;
  GeodeResidentSlot& operator=(GeodeResidentSlot&& other) noexcept {
    if (this != &other) {
      reset();
      buffer = other.buffer;
      other.buffer = wgpu::Buffer();
      slab = std::move(other.slab);
      allocationOffset = other.allocationOffset;
      allocationSize = other.allocationSize;
      other.allocationOffset = 0;
      other.allocationSize = 0;
      bindGroup = std::move(other.bindGroup);
      bands = other.bands;
      curves = other.curves;
      hRefs = other.hRefs;
      vBands = other.vBands;
      vCurves = other.vCurves;
      vRefs = other.vRefs;
      hGrid = other.hGrid;
      vGrid = other.vGrid;
      uniform = other.uniform;
      vertexCount = other.vertexCount;
      lastResidentFrame = other.lastResidentFrame;
      resident = other.resident;
      encodedKey = other.encodedKey;
      encodedFingerprint = other.encodedFingerprint;
      owningDeviceId = other.owningDeviceId;
      lastUniform = std::move(other.lastUniform);
      other.resident = false;
      other.encodedKey = nullptr;
      other.owningDeviceId = 0;
    }
    return *this;
  }

  /// Return the slab allocation and drop the bind group + borrowed handle.
  /// Safe to call on an empty slot. A geometry mutation can remove this
  /// component after a draw has referenced the buffer but before the
  /// frame's command encoder is submitted. Do not call `Buffer::destroy()`
  /// here: WebGPU makes that buffer unusable immediately, invalidating the
  /// already-recorded draw. Releasing our references is sufficient because
  /// the command encoder keeps the referenced resources alive until it is
  /// done.
  void reset() {
    if (slab && allocationSize != 0) {
      GeodeResidentSlab::Allocation alloc{buffer, allocationOffset, allocationSize};
      slab->free(alloc);
    }
    // The slab pointer itself is intentionally kept: it outlives any one
    // residence (it is owned by the registry context), and the re-upload
    // path needs it right after reset(). `residentFillSlot` refreshes it
    // when the document crosses devices.
    allocationOffset = 0;
    allocationSize = 0;
    buffer = wgpu::Buffer();
    bindGroup.reset();
    resident = false;
    encodedKey = nullptr;
    encodedFingerprint = 0;
    owningDeviceId = 0;
    lastUniform.clear();
  }
};

/// GPU-resident geometry for one gradient-painted fill. Mirrors
/// \ref GeodeResidentSlot: a combined-usage
/// buffer holds the same eight analytic dual-ray SSBO regions, but the
/// uniform region holds the 672-byte gradient uniform block (stops inline,
/// `shaders/slug_gradient.wgsl`) and the cached bind group uses the
/// 11-binding gradient pipeline layout with the device-owned dummy
/// clip-mask texture/sampler in bindings 3 and 4. Residence is only taken
/// when no clip mask, clip polygon, or mask pass is active (the gradient
/// shader's clip flags must stay zero for the cached bind group to remain
/// stable), exactly like the solid-fill residence gate.
struct GeodeResidentGradientSlot {
  /// Combined Storage|Uniform|CopyDst buffer, borrowed from the owning
  /// `GeodeResidentSlab` chunk; region offsets are ABSOLUTE buffer
  /// offsets. Region layout matches \ref GeodeResidentSlot, with the
  /// uniform region sized for `GradientUniforms` (672 bytes).
  wgpu::Buffer buffer;
  /// Slab that owns `buffer`; null until residence is established. Held
  /// by shared_ptr so a device change (which swaps the registry's slab)
  /// cannot leave a slot referencing a destroyed slab: old slots keep the
  /// old slab alive until their own reset drops the reference.
  std::shared_ptr<GeodeResidentSlab> slab;
  /// Whole-slot allocation inside the slab, returned via `free` on reset.
  uint64_t allocationOffset = 0;
  uint64_t allocationSize = 0;

  /// Cached 11-binding gradient bind group. All bindings reference stable
  /// objects (this slot's buffer sub-ranges + device-owned dummy
  /// clip-mask texture/sampler), so it survives frames and encoders.
  ScopedWgpuHandle<wgpu::BindGroup> bindGroup;

  /// A byte sub-range of `buffer` (same semantics as GeodeResidentSlot::Region).
  struct Region {
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  Region bands;    ///< Horizontal band SSBO (binding 1).
  Region curves;   ///< Horizontal curve SSBO (binding 2).
  Region hRefs;    ///< Horizontal curve-reference SSBO (binding 9).
  Region vBands;   ///< Vertical band SSBO (binding 5).
  Region vCurves;  ///< Vertical curve SSBO (binding 6).
  Region vRefs;    ///< Vertical curve-reference SSBO (binding 10).
  Region hGrid;    ///< Horizontal band grid (binding 7).
  Region vGrid;    ///< Vertical band grid (binding 8).
  Region uniform;  ///< Per-draw gradient uniform block (binding 0, 256-aligned).

  uint32_t vertexCount = 0;  ///< Triangle-fan draw count generated from vertex_index.

  /// Frame index in which this slot was last drawn via the resident path.
  /// A slot's single uniform buffer + cached bind group can only serve ONE
  /// draw per frame; see GeodeResidentSlot::lastResidentFrame.
  uint64_t lastResidentFrame = ~uint64_t{0};

  /// Process-unique device id of the device that created the resources;
  /// see GeodeResidentSlot::owningDeviceId.
  uint64_t owningDeviceId = 0;

  /// True once `buffer` + `bindGroup` hold the current encode.
  bool resident = false;

  /// Identity guard: address + cheap fingerprint of the uploaded encode.
  const void* encodedKey = nullptr;
  uint64_t encodedFingerprint = 0;

  /// Bytes last written to the uniform region; unchanged draws skip the write.
  std::vector<uint8_t> lastUniform;

  GeodeResidentGradientSlot() = default;
  ~GeodeResidentGradientSlot() { reset(); }

  GeodeResidentGradientSlot(const GeodeResidentGradientSlot&) = delete;
  GeodeResidentGradientSlot& operator=(const GeodeResidentGradientSlot&) = delete;
  GeodeResidentGradientSlot(GeodeResidentGradientSlot&&) noexcept = default;
  GeodeResidentGradientSlot& operator=(GeodeResidentGradientSlot&& other) noexcept {
    if (this != &other) {
      reset();
      buffer = other.buffer;
      other.buffer = wgpu::Buffer();
      slab = std::move(other.slab);
      allocationOffset = other.allocationOffset;
      allocationSize = other.allocationSize;
      other.allocationOffset = 0;
      other.allocationSize = 0;
      bindGroup = std::move(other.bindGroup);
      bands = other.bands;
      curves = other.curves;
      hRefs = other.hRefs;
      vBands = other.vBands;
      vCurves = other.vCurves;
      vRefs = other.vRefs;
      hGrid = other.hGrid;
      vGrid = other.vGrid;
      uniform = other.uniform;
      vertexCount = other.vertexCount;
      lastResidentFrame = other.lastResidentFrame;
      resident = other.resident;
      encodedKey = other.encodedKey;
      encodedFingerprint = other.encodedFingerprint;
      owningDeviceId = other.owningDeviceId;
      lastUniform = std::move(other.lastUniform);
      other.resident = false;
      other.encodedKey = nullptr;
      other.owningDeviceId = 0;
    }
    return *this;
  }

  /// Return the slab allocation and drop the bind group + borrowed handle.
  /// Never calls `Buffer::destroy()`; see GeodeResidentSlot::reset.
  void reset() {
    if (slab && allocationSize != 0) {
      GeodeResidentSlab::Allocation alloc{buffer, allocationOffset, allocationSize};
      slab->free(alloc);
    }
    // The slab pointer itself is intentionally kept: it outlives any one
    // residence (it is owned by the registry context), and the re-upload
    // path needs it right after reset(). `residentFillSlot` refreshes it
    // when the document crosses devices.
    allocationOffset = 0;
    allocationSize = 0;
    buffer = wgpu::Buffer();
    bindGroup.reset();
    resident = false;
    encodedKey = nullptr;
    encodedFingerprint = 0;
    owningDeviceId = 0;
    lastUniform.clear();
  }
};

/// Sibling of `GeodePathCacheComponent`: holds the GPU residence for an
/// entity's fill and stroke encodes. Installed lazily by `RendererGeode`
/// at the solid-fill draw sites; removed by the same entt listener that
/// clears `GeodePathCacheComponent` when geometry changes.
struct GeodeResidentPathComponent {
  GeodeResidentSlot fillSlot;
  GeodeResidentSlot strokeSlot;
  /// Gradient-painted fill residence.
  GeodeResidentGradientSlot gradientFillSlot;
  /// Gradient-painted stroke residence. Holds the
  /// cached stroke-outline encode plus the resolved gradient uniform, so
  /// an unchanged gradient-stroked outline re-uploads zero geometry.
  GeodeResidentGradientSlot gradientStrokeSlot;
};

}  // namespace donner::geode
