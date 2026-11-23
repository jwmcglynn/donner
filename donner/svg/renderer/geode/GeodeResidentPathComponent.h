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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/Descriptors.h"
#include "donner/gpu/Handles.h"
#include "donner/gpu/shader/programs/SlugFill.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeFillVariants.h"
#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"
#include "donner/svg/renderer/geode/GeodePerDevice.h"
#include "donner/svg/renderer/geode/GeodeResourceBudget.h"

namespace donner::geode {

/// Layout of the per-instance record at binding 7 of the Slug fill
/// pipeline. Must match `InstanceRecord` in `donner/gpu/shader/programs/SlugFillSource.h`.
/// The vertex stage composes `uniforms.mvp * transform`; the fragment
/// stage reads the remaining fields through a flat instance-id varying,
/// so overlapping batched instances still blend in painter (instance)
/// order. All geometry bases are element offsets relative to the bound
/// buffer range's start (chunk-relative for resident geometry).
using InstanceRecord = gpu::shader::programs::SlugFillInstance;
static_assert(sizeof(InstanceRecord) == 256, "InstanceRecord struct layout mismatch");

/// Hands \p buffers and \p bindGroups to \p owner, to be released on the thread of the context
/// that created them. They are dropped here only once \p owner is gone, which is after their device
/// is gone, so releasing them does nothing.
/// @param owner Retirement of the context that created the handles.
/// @param buffers Buffers to let go of; all of \p owner's device.
/// @param bindGroups Bind groups to let go of; all of \p owner's device.
inline void RetireSlabHandles(const std::weak_ptr<GeodeHandleRetirement>& owner,
                              std::vector<gpu::Buffer> buffers,
                              std::vector<gpu::BindGroup> bindGroups) {
  if (const std::shared_ptr<GeodeHandleRetirement> retirement = owner.lock()) {
    // A slab only ever holds its own device's handles. One of another device's would be released
    // here, on a thread that device may not be using, so stop rather than corrupt its tables.
    UTILS_RELEASE_ASSERT_MSG(retirement->retire(buffers, bindGroups) == 0,
                             "a resident slab held another device's GPU handle");
  }
}

/**
 * Document-scoped, painter-ordered record slab for ordered draw batching
 * (ordered batching).
 *
 * Cross-entity batches issue ONE draw whose instances read
 * `instances[firstInstance + i]` from a contiguous record array. Records
 * therefore live in a document-scoped slab instead of inside each entity's
 * geometry chunk: slot indices are allocated in draw order, freed ranges
 * defer to the next frame (the same discipline as
 * `GeodeResidentSlab::beginFrame`), and a batch covers only consecutive
 * slots of one buffer, splitting at free-list reuse or buffer-growth
 * boundaries.
 *
 * Buffer growth keeps old slots in place: a grown slab allocates a bigger
 * buffer and future slots land there, so no record ever moves. Batches
 * that would straddle the buffer boundary split instead.
 *
 * Bound to one device like `GeodeResidentSlab`; not thread-safe.
 */
class GeodeRecordSlab {
public:
  /// One record slot: its index in the contiguous array and its absolute
  /// byte offset in the owning buffer.
  struct Slot {
    gpu::BufferRef buffer;
    uint64_t offset = 0;
    uint32_t index = 0;
    /// Stable identity of `buffer` (see `GeodeDevice::AllocateBufferId`).
    /// Anything that caches keyed on this slot must compare the id: the
    /// handle address is recycled once the owning slab is destroyed.
    uint64_t bufferId = 0;
  };

  /// @param deviceId Device the slab's buffers are created on.
  /// @param owner Retirement of the context that owns the device (see
  ///   `GeodeDevice::handleRetirement`); must be live. The slab's buffers and bind groups are
  ///   handed there when it is destroyed, so they are released on that context's thread whichever
  ///   thread destroys the slab.
  /// @param budget Document budget its bytes are charged to, if any.
  GeodeRecordSlab(uint64_t deviceId, std::weak_ptr<GeodeHandleRetirement> owner,
                  std::shared_ptr<GeodeDocumentGeometryBudget> budget = nullptr)
      : owningDeviceId_(deviceId), budget_(std::move(budget)), owner_(std::move(owner)) {
    UTILS_RELEASE_ASSERT_MSG(!owner_.expired(), "a record slab needs its context's retirement");
  }

  ~GeodeRecordSlab() {
    if (budget_ && accountedBytes_ != 0) {
      budget_->releaseResidentBytes(accountedBytes_);
    }
    std::vector<gpu::Buffer> buffers;
    for (Chunk& chunk : chunks_) {
      buffers.push_back(std::move(chunk.buffer));
    }
    for (BatchUniform& uniform : batchUniforms_) {
      buffers.push_back(std::move(uniform.buffer));
    }
    RetireSlabHandles(owner_, std::move(buffers), std::move(retiredBindGroups_));
  }

  GeodeRecordSlab(const GeodeRecordSlab&) = delete;
  GeodeRecordSlab& operator=(const GeodeRecordSlab&) = delete;

  /// Device on which this record slab's buffers were created.
  uint64_t owningDeviceId() const { return owningDeviceId_; }

  /// Document budget charged for resident records, if one was supplied.
  const std::shared_ptr<GeodeDocumentGeometryBudget>& documentBudget() const { return budget_; }

  /// Merge the previous frame's freed slots into the reusable free list.
  /// The frame-index guard makes the merge idempotent per device-scoped generation.
  /// Callers defer this merge while an older frame remains open on the device, because its
  /// unsubmitted commands may still reference freed slots and retired bind groups.
  void beginFrame(uint64_t frameIndex) {
    if (frameIndex == lastMergedFrame_) {
      return;
    }
    lastMergedFrame_ = frameIndex;
    retiredBindGroups_.clear();
    // Drop the prefix allocations consumed, then merge the sorted frees in: linear in the list
    // plus k log k for k frees, where inserting each free in place was quadratic in k.
    freeIndices_.erase(freeIndices_.begin(),
                       freeIndices_.begin() + static_cast<std::ptrdiff_t>(freeHead_));
    freeHead_ = 0;
    std::sort(pendingFrees_.begin(), pendingFrees_.end());
    const auto merged = static_cast<std::ptrdiff_t>(freeIndices_.size());
    freeIndices_.insert(freeIndices_.end(), pendingFrees_.begin(), pendingFrees_.end());
    std::inplace_merge(freeIndices_.begin(), freeIndices_.begin() + merged, freeIndices_.end());
    // A slot freed more than once before this merge, or freed again while still on the free list,
    // is listed once, so it is handed out once. This cannot catch a stale free of a slot that has
    // already been handed out again: that slot would be listed free while it is live.
    freeIndices_.erase(std::unique(freeIndices_.begin(), freeIndices_.end()), freeIndices_.end());
    freeListSteps_ += freeIndices_.size();
    pendingFrees_.clear();
  }

  /// Free-list entries visited by \ref beginFrame merges and allocations so far, so a test can
  /// hold the free list to linear work.
  [[nodiscard]] uint64_t freeListStepsForTesting() const { return freeListSteps_; }

  /// Park a bind group a slot is dropping until the next frame boundary.
  ///
  /// A slot's cached bind group can be dropped mid-frame (geometry re-upload, component removal,
  /// encode change) while draws recorded earlier in the frame still name it and have not reached
  /// the backend yet. Destroying it there fails those draws closed, so it is held here and
  /// released at the next \ref beginFrame, by which point the frame that recorded them has been
  /// submitted.
  ///
  /// @param bindGroup Bind group to release at the next frame boundary.
  void retireBindGroup(gpu::BindGroup bindGroup) {
    if (bindGroup.isValid()) {
      retiredBindGroups_.push_back(std::move(bindGroup));
    }
  }

  /// Allocate the next record slot (free-list first, then bump in the
  /// newest buffer). Returns false when the device cannot create a buffer.
  bool allocateSlot(GeodeDevice& device, Slot& out) {
    if (freeHead_ < freeIndices_.size()) {
      // Lowest free index first, so slots handed out in draw order stay consecutive. The cursor
      // consumes the sorted list from the front in constant time; the next merge drops the prefix.
      const uint32_t index = freeIndices_[freeHead_++];
      ++freeListSteps_;
      out = slotAt(index);
      // A stale index that no longer resolves stays out of the free list, but never silently
      // consumes the allocation attempt.
      return out.buffer.isValid();
    }
    if (chunks_.empty() || usedBytes_ + sizeof(InstanceRecord) > chunks_.back().size) {
      uint64_t newSize = chunks_.empty() ? kInitialRecordSlabBytes : chunks_.back().size * 2u;
      while (newSize < sizeof(InstanceRecord)) {
        if (newSize > std::numeric_limits<uint64_t>::max() / 2u) {
          return false;
        }
        newSize *= 2u;
      }
      if (budget_ && !budget_->reserveResidentBytes(newSize)) {
        return false;
      }
      gpu::Result<gpu::Buffer> created = device.runtimeDevice().createBuffer(gpu::BufferDescriptor{
          "GeodeRecordSlab", newSize, gpu::BufferUsage::Storage | gpu::BufferUsage::CopyDst});
      if (created.hasError()) {
        if (budget_) {
          budget_->releaseResidentBytes(newSize);
        }
        return false;
      }
      chunks_.push_back(
          Chunk{std::move(created).result(), newSize, GeodeDevice::AllocateBufferId()});
      accountedBytes_ += newSize;
      usedBytes_ = 0;
    }
    out.buffer = chunks_.back().buffer;
    out.bufferId = chunks_.back().bufferId;
    out.offset = usedBytes_;
    // Indices are GLOBAL across chunks (slotAt walks cumulative chunk
    // capacities): a per-chunk index would collide with the previous
    // chunk after growth, and a free of the collided index would hand
    // the same storage to two live slots.
    uint32_t baseIndex = 0;
    for (size_t i = 0; i + 1 < chunks_.size(); ++i) {
      baseIndex += static_cast<uint32_t>(chunks_[i].size / sizeof(InstanceRecord));
    }
    out.index = baseIndex + static_cast<uint32_t>(usedBytes_ / sizeof(InstanceRecord));
    usedBytes_ += sizeof(InstanceRecord);
    return true;
  }

  /// Defer freeing a slot until the next frame: a slot reused within the
  /// frame that freed it would overwrite a record the frame's already
  /// recorded batch draws still read.
  void freeSlot(const Slot& slot) { pendingFrees_.push_back(slot.index); }

  /// A batch-uniform buffer plus its stable identity; `buffer` is null when
  /// the slab declined (uniform cache full, or buffer creation failed).
  struct BatchUniformHandle {
    gpu::BufferRef buffer;
    uint64_t bufferId = 0;
  };

  /// Include fixed uniform metadata when estimating a slab's retained bytes.
  /// @param uniformBytes Bytes of uniform buffer payload.
  /// @return Aggregate charge, or no value if addition overflows.
  static std::optional<uint64_t> batchUniformRetainedBytes(uint64_t uniformBytes) {
    constexpr uint64_t kMetadataBytes = kMaxBatchUniforms * sizeof(BatchUniform);
    if (uniformBytes > std::numeric_limits<uint64_t>::max() - kMetadataBytes) {
      return std::nullopt;
    }
    return kMetadataBytes + uniformBytes;
  }

  /**
   * Persistent buffer holding one distinct scene-batch uniform value.
   *
   * A batch uniform is frame-constant (identity mvp plus the frame's
   * viewport / clip / antialias state), so a steady frame finds its entry
   * and writes nothing at all - the per-frame arena would rewrite it every
   * frame because the arena's bytes do not survive.
   *
   * Entries are keyed by their exact bytes and are never rewritten, so a
   * differing value (a second render-target size within one frame) takes a
   * new entry and a batch already recorded this frame keeps reading its own
   * bytes at submit time. Returns a null handle once `kMaxBatchUniforms`
   * distinct values are live, and the caller falls back to its per-frame
   * arena.
   */
  BatchUniformHandle acquireBatchUniform(GeodeDevice& device, const void* bytes, uint64_t size) {
    const auto* first = static_cast<const uint8_t*>(bytes);
    for (const BatchUniform& entry : batchUniforms_) {
      if (entry.bytes.size() == size && std::memcmp(entry.bytes.data(), first, size) == 0) {
        return BatchUniformHandle{entry.buffer, entry.bufferId};
      }
    }
    if (batchUniforms_.size() >= kMaxBatchUniforms) {
      return BatchUniformHandle{};
    }
    if (budget_ && !budget_->reserveResidentBytes(size)) {
      return BatchUniformHandle{};
    }
    if (size > std::numeric_limits<uint64_t>::max() - cpuPayloadBytes_) {
      if (budget_) {
        budget_->releaseResidentBytes(size);
      }
      return BatchUniformHandle{};
    }
    const uint64_t previousCpuBytes = cpuRetainedBytes_;
    const uint64_t nextPayloadBytes = cpuPayloadBytes_ + size;
    if (batchUniforms_.capacity() < kMaxBatchUniforms) {
      const std::optional<uint64_t> projectedBytes = batchUniformRetainedBytes(nextPayloadBytes);
      if (!projectedBytes.has_value() || !replaceCpuBytes(*projectedBytes)) {
        if (budget_) {
          budget_->releaseResidentBytes(size);
        }
        return BatchUniformHandle{};
      }
      batchUniforms_.reserve(kMaxBatchUniforms);
    }
    const uint64_t metadataBytes = batchUniformMetadataBytesForTesting();
    if (nextPayloadBytes > std::numeric_limits<uint64_t>::max() - metadataBytes ||
        !replaceCpuBytes(metadataBytes + nextPayloadBytes)) {
      if (batchUniforms_.empty()) {
        std::vector<BatchUniform>().swap(batchUniforms_);
      }
      (void)replaceCpuBytes(previousCpuBytes);
      if (budget_) {
        budget_->releaseResidentBytes(size);
      }
      return BatchUniformHandle{};
    }
    std::vector<uint8_t> retainedBytes(first, first + size);
    const uint64_t retainedCapacity = retainedBytes.capacity();
    if (retainedCapacity > std::numeric_limits<uint64_t>::max() - cpuPayloadBytes_ ||
        cpuPayloadBytes_ + retainedCapacity >
            std::numeric_limits<uint64_t>::max() - metadataBytes ||
        !replaceCpuBytes(metadataBytes + cpuPayloadBytes_ + retainedCapacity)) {
      if (batchUniforms_.empty()) {
        std::vector<BatchUniform>().swap(batchUniforms_);
      }
      (void)replaceCpuBytes(previousCpuBytes);
      if (budget_) {
        budget_->releaseResidentBytes(size);
      }
      return BatchUniformHandle{};
    }
    gpu::Result<gpu::Buffer> created = device.runtimeDevice().createBuffer(gpu::BufferDescriptor{
        "GeodeSceneBatchUniform", size, gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst});
    if (created.hasError()) {
      if (budget_) {
        budget_->releaseResidentBytes(size);
      }
      (void)replaceCpuBytes(metadataBytes + cpuPayloadBytes_);
      return BatchUniformHandle{};
    }
    gpu::Buffer buffer = std::move(created).result();
    (void)device.runtimeDevice().writeBuffer(buffer, 0, std::span<const uint8_t>(first, size));
    batchUniforms_.push_back(
        BatchUniform{std::move(buffer), std::move(retainedBytes), GeodeDevice::AllocateBufferId()});
    cpuPayloadBytes_ += retainedCapacity;
    accountedBytes_ += size;
    return BatchUniformHandle{batchUniforms_.back().buffer, batchUniforms_.back().bufferId};
  }

  /// The chunk buffer carrying \p bufferId, or a null handle when this slab does not own it.
  /// Uploads need the owning handle; `Slot` and `Allocation` only carry an identity view.
  /// @param bufferId Stable buffer id stamped on the slot or allocation.
  const gpu::Buffer& bufferForId(uint64_t bufferId) const {
    static const gpu::Buffer kNone;
    for (const Chunk& chunk : chunks_) {
      if (chunk.bufferId == bufferId) {
        return chunk.buffer;
      }
    }
    return kNone;
  }

  /// Total live bytes across all buffers (resident-bytes accounting).
  uint64_t liveBytes() const {
    uint64_t total = 0;
    for (const Chunk& chunk : chunks_) {
      total += chunk.size;
    }
    return total;
  }

  /// Capacity bytes held by batch-uniform metadata, exposed to tests.
  uint64_t batchUniformMetadataBytesForTesting() const {
    return batchUniforms_.capacity() * sizeof(BatchUniform);
  }
  /// Capacity bytes held by CPU uniform payloads, exposed to tests.
  uint64_t batchUniformPayloadBytesForTesting() const { return cpuPayloadBytes_; }

private:
  struct Chunk {
    gpu::Buffer buffer;
    uint64_t size = 0;
    uint64_t bufferId = 0;
  };

  struct BatchUniform {
    gpu::Buffer buffer;
    std::vector<uint8_t> bytes;
    uint64_t bufferId = 0;
  };

  static constexpr uint64_t kInitialRecordSlabBytes = 262144u;
  /// Distinct live scene-batch uniform values before the caller falls back
  /// to its per-frame arena. One value covers a document rendered at one
  /// size; the cap bounds a pathological caller that varies the uniform per
  /// batch.
  static constexpr size_t kMaxBatchUniforms = 8u;

  bool replaceCpuBytes(uint64_t bytes) {
    if (!budget_) {
      cpuRetainedBytes_ = bytes;
      return true;
    }
    if (!cpuReservation_.replace(budget_, bytes)) {
      return false;
    }
    cpuRetainedBytes_ = bytes;
    return true;
  }

  Slot slotAt(uint32_t index) const {
    // Slots are never moved; walk the chunk sizes to find the owner.
    uint64_t offset = static_cast<uint64_t>(index) * sizeof(InstanceRecord);
    for (const Chunk& chunk : chunks_) {
      if (offset < chunk.size) {
        return Slot{chunk.buffer, offset, index, chunk.bufferId};
      }
      offset -= chunk.size;
    }
    return Slot{gpu::BufferRef(), 0, index, 0};
  }

  uint64_t owningDeviceId_;
  uint64_t lastMergedFrame_ = ~uint64_t{0};
  uint64_t usedBytes_ = 0;
  std::vector<Chunk> chunks_;
  /// Free indices in ascending order; entries before `freeHead_` are already handed out.
  std::vector<uint32_t> freeIndices_;
  size_t freeHead_ = 0;
  uint64_t freeListSteps_ = 0;
  std::vector<uint32_t> pendingFrees_;
  /// Bind groups dropped by slots this frame, released at the next \ref beginFrame.
  std::vector<gpu::BindGroup> retiredBindGroups_;
  std::vector<BatchUniform> batchUniforms_;
  std::shared_ptr<GeodeDocumentGeometryBudget> budget_;
  GeodeGeometryCacheReservation cpuReservation_;
  uint64_t cpuRetainedBytes_ = 0;
  uint64_t cpuPayloadBytes_ = 0;
  uint64_t accountedBytes_ = 0;
  /// Retirement of the owning context; see the constructor.
  std::weak_ptr<GeodeHandleRetirement> owner_;
};

class GeodeResidentSlab;

/**
 * A batch-level uniform kept across frames for one occurrence of a repeated draw (see
 * \ref GeodeOccurrenceRecords): a range of a geometry slab chunk, named by identity like a slab
 * allocation, and the bytes last written to it, so an unchanged frame writes nothing.
 */
struct GeodeClipUniform {
  gpu::BufferRef buffer;           ///< Borrowed chunk identity; the slab owns the buffer.
  uint64_t bufferId = 0;           ///< Stable identity of `buffer`.
  uint64_t offset = 0;             ///< Byte offset of the range in `buffer`.
  uint64_t size = 0;               ///< Bytes of the range; 0 until it is allocated.
  std::vector<uint8_t> lastBytes;  ///< Bytes last written to the range.
};

/**
 * Persistent per-occurrence instance records for content drawn more than once
 * over shared resident geometry: the glyph occurrences of one text element, or
 * the second and later draws of one resident slot within a frame (each marker
 * placement of a marker's content, a non-adjacent repeat of a `<use>` source).
 *
 * The geometry is shared, so the thing that has to exist once per OCCURRENCE is
 * the record: transform, colour, clip rectangle, and the shared geometry's
 * chunk-relative bases. Handing every occurrence a fresh record slot each frame
 * would work, but it would also write 256 bytes per occurrence per frame
 * forever, which keeps a static frame off the zero-bytes steady state the rest
 * of the renderer reaches.
 *
 * So the slots live on their owner (a text element, one set per device that
 * draws it, see \ref GeodeTextInstanceResidencyComponent; or a resident slot,
 * see `GeodeResidentSlot::repeatRecords`) and persist. Occurrence `i` of the
 * owner always writes slot `i`, the bytes are compared before writing, and an
 * unchanged frame writes nothing. Slots are allocated together on first use,
 * in draw order, which keeps their indices consecutive - exactly the property a
 * batch needs to cover them with one draw.
 *
 * No explicit invalidation is needed when the content changes: a changed
 * occurrence simply produces different record bytes for the affected slots, the
 * compare misses, and those slots are rewritten. A frame with fewer occurrences
 * leaves trailing slots unused (and free for the owner to grow back into); a
 * frame with more appends.
 *
 * `lastFrame` guards the one case the compare cannot: the same owner written
 * twice while an earlier writer's draws are still unsubmitted - a text element
 * drawn twice in one frame (a `<use>` of the text), or an offscreen pass
 * rendering the same document inside an outer frame that has already recorded
 * its batch. The second writer must not rewrite those records, because every
 * buffer write in a frame executes before every draw in that frame's submit.
 * `lastFrame` holds a DEVICE-scoped generation
 * (`GeodeDevice::beginFrameGeneration`) so the comparison covers renderers
 * sharing a device, and the renderer diverts the second writer to per-frame
 * temporary slots instead.
 *
 * An occurrence drawn under a clip polygon also keeps a uniform of its own
 * (\ref GeodeClipUniform): the polygon reaches the shader only through the
 * draw-level uniform, which an ordered batch otherwise shares.
 */
struct GeodeOccurrenceRecords {
  /// One occurrence's record slot plus the bytes last written to it.
  struct Occurrence {
    GeodeRecordSlab::Slot slot;
    std::vector<uint8_t> lastRecord;
    /// The batch-level uniform this occurrence's draw binds while a clip polygon is active;
    /// created the first time it draws under one and kept until its owner releases it.
    std::unique_ptr<GeodeClipUniform> clipUniform;
  };
  /// Stable owner for a separately allocated occurrence record.
  using OccurrenceOwner = std::unique_ptr<Occurrence>;
  /// Estimated retained bytes in one occurrence and its instance record.
  static constexpr uint64_t kOccurrencePayloadBytes = sizeof(Occurrence) + sizeof(InstanceRecord);
  /// Per-occurrence charge including the owner's vector entry.
  static constexpr uint64_t kProjectedBytesPerOccurrence =
      sizeof(OccurrenceOwner) + kOccurrencePayloadBytes;

  /// Occurrence slots in paint order. Index is the occurrence's ordinal
  /// among its owner's draws in one frame, so it is stable across unchanged
  /// frames.
  /// Owners may move when the vector grows, but each separately allocated
  /// occurrence stays put while a pending batch holds pointers into it.
  std::vector<OccurrenceOwner> occurrences;
  /// Slab the slots were allocated from, this device's record slab for the
  /// document; also keeps that slab alive for as long as the slots are.
  std::shared_ptr<GeodeRecordSlab> recordSlab;
  /// Device-scoped frame generation of the last draw that wrote these slots.
  /// Zero means "never written": generations start at 1, and the guard reads
  /// this as "older than every open frame", which is what untouched records
  /// are.
  uint64_t lastFrame = 0;
  std::shared_ptr<GeodeDocumentGeometryBudget> cpuBudget;  ///< Document budget for CPU records.
  GeodeGeometryCacheReservation cpuReservation;            ///< Owned CPU-byte reservation.
  uint64_t cpuRetainedBytes = 0;                           ///< Charged owner and record bytes.
  /// Geometry slab every occurrence's `clipUniform` range comes from: the slab of the slot whose
  /// draws the occurrences repeat, on the same device. Null until an occurrence first draws under
  /// a clip polygon.
  std::shared_ptr<GeodeResidentSlab> clipUniformSlab;

  GeodeOccurrenceRecords() = default;
  ~GeodeOccurrenceRecords() { freeRecordSlots(); }

  GeodeOccurrenceRecords(const GeodeOccurrenceRecords&) = delete;
  GeodeOccurrenceRecords& operator=(const GeodeOccurrenceRecords&) = delete;

  /// Transfer occurrence slots and CPU reservations without freeing live slab records.
  /// @param other Component whose records are moved into this object.
  GeodeOccurrenceRecords(GeodeOccurrenceRecords&& other) noexcept
      : occurrences(std::move(other.occurrences)),
        recordSlab(std::move(other.recordSlab)),
        lastFrame(other.lastFrame),
        cpuBudget(std::move(other.cpuBudget)),
        cpuReservation(std::move(other.cpuReservation)),
        cpuRetainedBytes(other.cpuRetainedBytes),
        clipUniformSlab(std::move(other.clipUniformSlab)),
        cpuOccurrenceBytes(other.cpuOccurrenceBytes),
        occurrenceReservationPending(other.occurrenceReservationPending) {
    std::vector<OccurrenceOwner>().swap(other.occurrences);
    other.cpuRetainedBytes = 0;
    other.cpuOccurrenceBytes = 0;
    other.occurrenceReservationPending = false;
  }

  /// Release current slots and take ownership of another component's records.
  /// @param other Component whose records are moved into this object.
  /// @return This component after the transfer.
  GeodeOccurrenceRecords& operator=(GeodeOccurrenceRecords&& other) noexcept {
    if (this != &other) {
      // Release ours first, then take the source's slots and clear the
      // source's view of them: entt's swap-and-pop removal move-assigns the
      // surviving component over the removed one, and a source that kept its
      // slots would free the SURVIVOR's records from its own destructor,
      // handing live batch bindings to whatever allocates next.
      freeRecordSlots();
      occurrences = std::move(other.occurrences);
      recordSlab = std::move(other.recordSlab);
      lastFrame = other.lastFrame;
      cpuBudget = std::move(other.cpuBudget);
      cpuReservation = std::move(other.cpuReservation);
      cpuRetainedBytes = other.cpuRetainedBytes;
      clipUniformSlab = std::move(other.clipUniformSlab);
      cpuOccurrenceBytes = other.cpuOccurrenceBytes;
      occurrenceReservationPending = other.occurrenceReservationPending;
      std::vector<OccurrenceOwner>().swap(other.occurrences);
      other.cpuRetainedBytes = 0;
      other.cpuOccurrenceBytes = 0;
      other.occurrenceReservationPending = false;
    }
    return *this;
  }

  /// Reserve CPU bytes and vector capacity before appending one occurrence.
  /// @param budget Document budget that will own the new reservation.
  /// @return False if capacity growth or byte admission fails.
  bool reserveOccurrence(std::shared_ptr<GeodeDocumentGeometryBudget> budget) {
    if (!canReserveOccurrence(budget) ||
        kOccurrencePayloadBytes > std::numeric_limits<uint64_t>::max() - cpuOccurrenceBytes) {
      return false;
    }
    const size_t nextSize = occurrences.size() + 1u;
    const uint64_t payloadBytes = cpuOccurrenceBytes + kOccurrencePayloadBytes;
    const bool charged = nextSize > occurrences.capacity()
                             ? growOwnersAndCharge(budget, nextSize, payloadBytes)
                             : chargeAtCurrentCapacity(budget, payloadBytes);
    if (!charged) {
      return false;
    }
    cpuBudget = std::move(budget);
    occurrenceReservationPending = true;
    return true;
  }

  /// Append a slab slot after reserveOccurrence has admitted its CPU payload.
  /// @param slot GPU record slab slot assigned to the new occurrence.
  /// @return False if the pending reservation cannot be completed.
  bool appendReservedOccurrence(const GeodeRecordSlab::Slot& slot) {
    if (!occurrenceReservationPending) {
      return false;
    }
    if (!cpuBudget || occurrences.size() == occurrences.capacity()) {
      rollbackOccurrence();
      return false;
    }
    auto occurrence = std::make_unique<Occurrence>();
    occurrence->slot = slot;
    occurrence->lastRecord.reserve(sizeof(InstanceRecord));
    const uint64_t lastRecordBytes = occurrence->lastRecord.capacity();
    if (lastRecordBytes > std::numeric_limits<uint64_t>::max() - sizeof(Occurrence) ||
        sizeof(Occurrence) + lastRecordBytes >
            std::numeric_limits<uint64_t>::max() - cpuOccurrenceBytes) {
      rollbackOccurrence();
      return false;
    }
    const uint64_t occurrenceBytes = sizeof(Occurrence) + lastRecordBytes;
    const uint64_t ownerBytes =
        static_cast<uint64_t>(occurrences.capacity()) * sizeof(OccurrenceOwner);
    if (cpuOccurrenceBytes + occurrenceBytes > std::numeric_limits<uint64_t>::max() - ownerBytes ||
        !cpuReservation.replace(cpuBudget, ownerBytes + cpuOccurrenceBytes + occurrenceBytes)) {
      rollbackOccurrence();
      return false;
    }
    occurrences.push_back(std::move(occurrence));
    cpuOccurrenceBytes += occurrenceBytes;
    cpuRetainedBytes = ownerBytes + cpuOccurrenceBytes;
    occurrenceReservationPending = false;
    return true;
  }

  /// Restore the CPU reservation after an occurrence append fails.
  void rollbackOccurrence() {
    if (!cpuBudget) {
      occurrenceReservationPending = false;
      return;
    }
    const uint64_t ownerBytes =
        static_cast<uint64_t>(occurrences.capacity()) * sizeof(OccurrenceOwner);
    cpuRetainedBytes = ownerBytes + cpuOccurrenceBytes;
    (void)cpuReservation.replace(cpuBudget, cpuRetainedBytes);
    occurrenceReservationPending = false;
  }

  /// CPU bytes currently charged for this component, exposed to tests.
  uint64_t cpuRetainedBytesForTesting() const { return cpuRetainedBytes; }

  /// The clip uniform of \p occurrence, created and charged with the occurrence's other CPU bytes
  /// on first use: the record of its range and a copy of \p bytes bytes of uniform.
  /// @param occurrence An occurrence of this component.
  /// @param bytes Size of the uniform it will hold.
  /// @return The occurrence's clip uniform, or null, leaving none, when the budget refuses it.
  GeodeClipUniform* reserveClipUniform(Occurrence& occurrence, size_t bytes) {
    if (occurrence.clipUniform) {
      return occurrence.clipUniform->lastBytes.capacity() >= bytes ? occurrence.clipUniform.get()
                                                                   : nullptr;
    }
    if (!cpuBudget || occurrenceReservationPending) {
      return nullptr;
    }
    auto clipUniform = std::make_unique<GeodeClipUniform>();
    clipUniform->lastBytes.reserve(bytes);
    const uint64_t added = sizeof(GeodeClipUniform) + clipUniform->lastBytes.capacity();
    const uint64_t ownerBytes =
        static_cast<uint64_t>(occurrences.capacity()) * sizeof(OccurrenceOwner);
    if (added > std::numeric_limits<uint64_t>::max() - cpuOccurrenceBytes ||
        cpuOccurrenceBytes + added > std::numeric_limits<uint64_t>::max() - ownerBytes ||
        !cpuReservation.replace(cpuBudget, ownerBytes + cpuOccurrenceBytes + added)) {
      return nullptr;
    }
    cpuOccurrenceBytes += added;
    cpuRetainedBytes = ownerBytes + cpuOccurrenceBytes;
    occurrence.clipUniform = std::move(clipUniform);
    return occurrence.clipUniform.get();
  }

  /// Return every occurrence's clip uniform range to `clipUniformSlab` (deferred to its next frame
  /// boundary, like every other slab free) and drop the uniforms and their charge.
  void releaseClipUniforms();

  /// Return every slot to the slab (deferred to the next frame's merge, like
  /// every other record free) and drop them.
  void freeRecordSlots() {
    releaseClipUniforms();
    if (recordSlab) {
      for (const OccurrenceOwner& occurrence : occurrences) {
        if (occurrence && occurrence->slot.buffer.isValid()) {
          recordSlab->freeSlot(occurrence->slot);
        }
      }
    }
    std::vector<OccurrenceOwner>().swap(occurrences);
    cpuReservation.reset();
    cpuBudget.reset();
    cpuRetainedBytes = 0;
    cpuOccurrenceBytes = 0;
    occurrenceReservationPending = false;
  }

private:
  /// Whether one more occurrence may be reserved against \p budget: no reservation is pending,
  /// the records charge no other budget, and the list can still grow.
  bool canReserveOccurrence(const std::shared_ptr<GeodeDocumentGeometryBudget>& budget) const {
    return !occurrenceReservationPending && budget &&
           (!cpuBudget || cpuBudget.get() == budget.get()) &&
           occurrences.size() != std::numeric_limits<size_t>::max();
  }

  /// Grow the owner vector to hold \p nextSize occurrences and charge its new capacity plus
  /// \p payloadBytes, restoring the previous charge when the grown vector cannot be charged.
  /// @return False, with the vector unchanged, if the growth or the charge is refused.
  bool growOwnersAndCharge(const std::shared_ptr<GeodeDocumentGeometryBudget>& budget,
                           size_t nextSize, uint64_t payloadBytes) {
    const uint64_t oldBytes = cpuRetainedBytes;
    size_t targetCapacity = occurrences.capacity() == 0u ? 1u : occurrences.capacity();
    while (targetCapacity < nextSize) {
      if (targetCapacity > std::numeric_limits<size_t>::max() / 2u) {
        return false;
      }
      targetCapacity *= 2u;
    }
    if (targetCapacity > occurrences.max_size()) {
      return false;
    }
    const uint64_t projectedOwnerBytes =
        static_cast<uint64_t>(targetCapacity) * sizeof(OccurrenceOwner);
    if (payloadBytes > std::numeric_limits<uint64_t>::max() - projectedOwnerBytes ||
        !cpuReservation.replace(budget, projectedOwnerBytes + payloadBytes)) {
      return false;
    }

    std::vector<OccurrenceOwner> replacement;
    replacement.reserve(targetCapacity);
    const uint64_t actualOwnerBytes =
        static_cast<uint64_t>(replacement.capacity()) * sizeof(OccurrenceOwner);
    if (payloadBytes > std::numeric_limits<uint64_t>::max() - actualOwnerBytes ||
        !cpuReservation.replace(budget, actualOwnerBytes + payloadBytes)) {
      (void)cpuReservation.replace(budget, oldBytes);
      return false;
    }
    for (OccurrenceOwner& occurrence : occurrences) {
      replacement.push_back(std::move(occurrence));
    }
    occurrences.swap(replacement);
    cpuRetainedBytes = actualOwnerBytes + payloadBytes;
    return true;
  }

  /// Charge the owner vector's current capacity plus \p payloadBytes.
  /// @return False if the charge is refused.
  bool chargeAtCurrentCapacity(const std::shared_ptr<GeodeDocumentGeometryBudget>& budget,
                               uint64_t payloadBytes) {
    const uint64_t ownerBytes =
        static_cast<uint64_t>(occurrences.capacity()) * sizeof(OccurrenceOwner);
    if (payloadBytes > std::numeric_limits<uint64_t>::max() - ownerBytes ||
        !cpuReservation.replace(budget, ownerBytes + payloadBytes)) {
      return false;
    }
    cpuRetainedBytes = ownerBytes + payloadBytes;
    return true;
  }

  uint64_t cpuOccurrenceBytes = 0;
  bool occurrenceReservationPending = false;
};

class GeodeDevice;
struct GeodeResidentImage;

/**
 * A gradient's stops laid out in a stop ramp texture (see `stopRamp` in
 * `donner/gpu/shader/programs/SlugGradientSource.h`). Draws and resident slots of gradients with
 * the same stops share one.
 */
struct GeodeGradientRamp {
  /// The stops it holds: per stop, the offset then straight-alpha RGBA. Draws compare their own
  /// stops with it byte for byte before they bind the ramp, so two gradients never share one
  /// unless their stops are identical.
  std::vector<float> stops;
  gpu::Texture texture;            ///< RGBA32Float ramp.
  gpu::TextureView view;           ///< View the gradient bind groups bind.
  std::uint64_t textureBytes = 0;  ///< Bytes of `texture`.

  /// The document budget a resident slot charged when it first kept this ramp across frames, and
  /// the bytes charged: the texture and the stop key, once for every slot that shares the ramp.
  /// Null while only this frame's draws hold it. Released when the last holder lets go, which for
  /// a slot is at the frame boundary after it drops the ramp.
  mutable std::shared_ptr<GeodeDocumentGeometryBudget> chargedBudget;
  mutable std::uint64_t chargedBytes = 0;  ///< Bytes charged to `chargedBudget`.

  GeodeGradientRamp() = default;
  ~GeodeGradientRamp() {
    if (chargedBudget) {
      chargedBudget->releaseRampBytes(chargedBytes);
    }
  }
  GeodeGradientRamp(const GeodeGradientRamp&) = delete;
  GeodeGradientRamp& operator=(const GeodeGradientRamp&) = delete;

  /// Bytes keeping this ramp across frames costs: the texture and the stop key's storage.
  [[nodiscard]] std::uint64_t retainedBytes() const {
    return textureBytes + static_cast<std::uint64_t>(stops.capacity()) * sizeof(float);
  }
};

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
 * rendered by several devices keeps one slab per device, so no device reuses
 * or replaces another's.
 *
 * Not thread-safe: allocate/free/beginFrame mutate the free list and bump
 * cursors without locking. Every caller holds the document, which serializes
 * them. A renderer is one of them: the slots it borrows for a frame go back
 * when that frame ends, while the document is still held, and it keeps none
 * past the frame. Destroying it releases nothing on the calling
 * thread: its buffers and bind groups go to the owning context's retirement
 * (see the constructor), because the device's own thread may be using its
 * tables.
 */
class GeodeResidentSlab {
public:
  /// The chunk buffer carrying \p bufferId, or a null handle when this slab does not own it.
  /// Uploads need the owning handle; `Slot` and `Allocation` only carry an identity view.
  /// @param bufferId Stable buffer id stamped on the slot or allocation.
  const gpu::Buffer& bufferForId(uint64_t bufferId) const {
    static const gpu::Buffer kNone;
    for (const Chunk& chunk : chunks_) {
      if (chunk.bufferId == bufferId) {
        return chunk.buffer;
      }
    }
    return kNone;
  }

  /// Byte size of the chunk carrying \p bufferId, or 0 when this slab does not own it.
  /// @param bufferId Stable buffer id stamped on the slot or allocation.
  uint64_t chunkBytesForId(uint64_t bufferId) const {
    for (const Chunk& chunk : chunks_) {
      if (chunk.bufferId == bufferId) {
        return chunk.size;
      }
    }
    return 0;
  }

  /// One sub-range of a slab chunk.
  struct Allocation {
    gpu::BufferRef buffer;  // Borrowed identity; the slab chunk owns the buffer.
    uint64_t offset = 0;
    uint64_t size = 0;
    /// Stable identity of `buffer` (see `GeodeDevice::AllocateBufferId`).
    /// Only allocation results carry it; `free` matches on the handle,
    /// which is unambiguous while the slab that owns the chunk is alive.
    uint64_t bufferId = 0;
  };

  /// Create an empty slab bound to `deviceId` (usually the current
  /// renderer's device). Chunks are allocated lazily on first use.
  /// @param deviceId Device on which this slab's buffers will be created.
  /// @param owner Retirement of the context that owns the device; must be live. See
  ///   \ref GeodeRecordSlab::GeodeRecordSlab.
  /// @param budget Document budget its bytes are charged to, if any.
  GeodeResidentSlab(uint64_t deviceId, std::weak_ptr<GeodeHandleRetirement> owner,
                    std::shared_ptr<GeodeDocumentGeometryBudget> budget = nullptr)
      : owningDeviceId_(deviceId), budget_(std::move(budget)), owner_(std::move(owner)) {
    UTILS_RELEASE_ASSERT_MSG(!owner_.expired(), "a resident slab needs its context's retirement");
  }

  ~GeodeResidentSlab() {
    if (budget_ && accountedBytes_ > 0) {
      budget_->releaseResidentBytes(static_cast<uint64_t>(accountedBytes_));
    }
    if (gauge_ && accountedBytes_ != 0) {
      gauge_->fetch_sub(accountedBytes_, std::memory_order_relaxed);
    }
    std::vector<gpu::Buffer> buffers;
    for (Chunk& chunk : chunks_) {
      buffers.push_back(std::move(chunk.buffer));
    }
    RetireSlabHandles(owner_, std::move(buffers), std::move(retiredBindGroups_));
  }

  GeodeResidentSlab(const GeodeResidentSlab&) = delete;
  GeodeResidentSlab& operator=(const GeodeResidentSlab&) = delete;

  /// Device this slab's chunks were created on.
  uint64_t owningDeviceId() const { return owningDeviceId_; }

  /// Document budget charged for resident geometry, if one was supplied.
  const std::shared_ptr<GeodeDocumentGeometryBudget>& documentBudget() const { return budget_; }

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
  /// command buffer was submitted at its endFrame. Generations are device-scoped; callers defer
  /// this merge while an older frame remains open so an offscreen draw cannot recycle ranges or
  /// retire bind groups still referenced by the enclosing frame's unsubmitted commands.
  void beginFrame(uint64_t frameIndex) {
    if (frameIndex == lastMergedFrame_) {
      return;
    }
    lastMergedFrame_ = frameIndex;
    retiredBindGroups_.clear();
    retiredRamps_.clear();
    retiredImages_.clear();
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
        out.buffer = chunks_[it->chunk].buffer;
        out.bufferId = chunks_[it->chunk].bufferId;
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
        if (newSize > std::numeric_limits<uint64_t>::max() / 2u) {
          return false;
        }
        newSize *= 2u;
      }
      if (budget_ && !budget_->reserveResidentBytes(newSize)) {
        return false;
      }
      gpu::Result<gpu::Buffer> created = device.runtimeDevice().createBuffer(gpu::BufferDescriptor{
          "GeodeResidentSlab", newSize,
          gpu::BufferUsage::Storage | gpu::BufferUsage::Uniform | gpu::BufferUsage::CopyDst});
      if (created.hasError()) {
        if (budget_) {
          budget_->releaseResidentBytes(newSize);
        }
        return false;
      }
      Chunk chunk;
      chunk.buffer = std::move(created).result();
      chunk.size = newSize;
      chunk.bufferId = GeodeDevice::AllocateBufferId();
      chunks_.push_back(std::move(chunk));
      if (!gauge_) {
        gauge_ = device.residentBytesGauge();
      }
      gauge_->fetch_add(static_cast<int64_t>(newSize), std::memory_order_relaxed);
      accountedBytes_ += static_cast<int64_t>(newSize);
    }

    Chunk& chunk = chunks_.back();
    out.buffer = chunk.buffer;
    out.bufferId = chunk.bufferId;
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
    // Locate the owning chunk by identity rather than by handle address, so
    // the match means "the chunk this range was carved out of" and cannot be
    // confused by a recycled handle. A default-constructed allocation carries
    // id 0 and matches nothing, since ids start at 1.
    size_t chunkIndex = 0;
    for (; chunkIndex < chunks_.size(); ++chunkIndex) {
      if (chunks_[chunkIndex].bufferId == alloc.bufferId) {
        break;
      }
    }
    if (chunkIndex == chunks_.size()) {
      return;  // Stale allocation (chunk no longer live); ignore.
    }
    pendingFrees_.push_back(FreeRange{chunkIndex, alloc.offset, alloc.size});
  }

  /// Park a bind group a slot is dropping until the next frame boundary.
  ///
  /// A slot's cached bind group can be dropped mid-frame (geometry re-upload, component removal,
  /// encode change) while draws recorded earlier in the frame still name it and have not reached
  /// the backend yet. Destroying it there fails those draws closed, so it is held here and
  /// released at the next \ref beginFrame, by which point the frame that recorded them has been
  /// submitted.
  ///
  /// @param bindGroup Bind group to release at the next frame boundary.
  void retireBindGroup(gpu::BindGroup bindGroup) {
    if (bindGroup.isValid()) {
      retiredBindGroups_.push_back(std::move(bindGroup));
    }
  }

  /// Park a stop ramp a slot is dropping until the next frame boundary, for the same reason as
  /// \ref retireBindGroup: draws recorded earlier in the frame may still bind it.
  /// @param ramp Ramp to release at the next frame boundary.
  void retireRamp(std::shared_ptr<const GeodeGradientRamp> ramp) {
    if (ramp != nullptr) {
      retiredRamps_.push_back(std::move(ramp));
    }
  }

  /// Park an image texture an entity is dropping until the next frame boundary, for the same
  /// reason as \ref retireBindGroup: draws recorded earlier in the frame may still sample it.
  /// @param image Image to release at the next frame boundary.
  void retireImage(std::shared_ptr<const GeodeResidentImage> image) {
    if (image != nullptr) {
      retiredImages_.push_back(std::move(image));
    }
  }

private:
  /// Bind groups dropped by slots this frame, released at the next \ref beginFrame.
  std::vector<gpu::BindGroup> retiredBindGroups_;
  /// Stop ramps dropped by slots this frame, released at the next \ref beginFrame.
  std::vector<std::shared_ptr<const GeodeGradientRamp>> retiredRamps_;
  /// Image textures dropped by entities this frame, released at the next \ref beginFrame.
  std::vector<std::shared_ptr<const GeodeResidentImage>> retiredImages_;

  static constexpr uint64_t kInitialChunkBytes = 1u << 20;  // 1 MiB.

  static uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
  }

  struct Chunk {
    gpu::Buffer buffer;
    uint64_t size = 0;
    uint64_t cursor = 0;
    uint64_t bufferId = 0;
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
  std::shared_ptr<GeodeDocumentGeometryBudget> budget_;
  int64_t accountedBytes_ = 0;
  /// Retirement of the owning context; see the constructor.
  std::weak_ptr<GeodeHandleRetirement> owner_;
};

inline void GeodeOccurrenceRecords::releaseClipUniforms() {
  uint64_t releasedBytes = 0;
  for (const OccurrenceOwner& occurrence : occurrences) {
    if (!occurrence || !occurrence->clipUniform) {
      continue;
    }
    const GeodeClipUniform& uniform = *occurrence->clipUniform;
    if (clipUniformSlab && uniform.size != 0) {
      clipUniformSlab->free(GeodeResidentSlab::Allocation{uniform.buffer, uniform.offset,
                                                          uniform.size, uniform.bufferId});
    }
    releasedBytes += sizeof(GeodeClipUniform) + uniform.lastBytes.capacity();
    occurrence->clipUniform.reset();
  }
  clipUniformSlab.reset();
  if (releasedBytes == 0) {
    return;
  }
  cpuOccurrenceBytes -= std::min(releasedBytes, cpuOccurrenceBytes);
  cpuRetainedBytes =
      static_cast<uint64_t>(occurrences.capacity()) * sizeof(OccurrenceOwner) + cpuOccurrenceBytes;
  if (cpuBudget) {
    (void)cpuReservation.replace(cpuBudget, cpuRetainedBytes);
  }
}

/// GPU-resident geometry for one cached `EncodedPath` (a fill slot or a
/// stroke slot). Owns a single combined-usage GPU buffer that holds the
/// path's eight analytic dual-ray SSBO regions and per-draw uniform block, plus the cached fill
/// bind group. Bounding vertices live in that uniform and are expanded from vertex_index, so the
/// resident geometry needs no vertex buffer. The buffer
/// and bind group are built once by `GeoEncoder` on first residence and
/// reused every subsequent unchanged frame.
///
/// Move-only (owns GPU runtime handles). Default-constructed slots are empty;
/// `GeoEncoder::fillPathResident` populates them lazily.
struct GeodeResidentSlot {
  /// Combined Storage|Uniform|CopyDst buffer, borrowed from the owning
  /// `GeodeResidentSlab` chunk. Region offsets are ABSOLUTE buffer
  /// offsets. Layout (each region offset satisfies the binding's
  /// alignment requirement):
  ///   [ bands | curves | vBands | vCurves | hGrid | vGrid | hRefs | vRefs | uniform ]
  /// The four grid classes are placed adjacently on purpose: they share one
  /// storage binding, whose range spans them. The slot is padded out to the
  /// slab's allocation alignment so consecutive slots leave no unwritten
  /// bytes between them.
  gpu::BufferRef buffer;
  /// Stable identity of `buffer` (see `GeodeDevice::AllocateBufferId`).
  /// A cross-entity batch binds the whole chunk and caches the resulting
  /// bind group past the lifetime of the document that owns it, so the
  /// cache key has to name the chunk by this id rather than by the handle
  /// address, which the allocator recycles once the slab is destroyed.
  uint64_t bufferId = 0;
  /// Slab that owns `buffer`, this device's geometry slab for the
  /// document; null until residence is established. Held by shared_ptr so
  /// the slab outlives every slot allocated from it, whichever of the two
  /// the document lets go of first.
  std::shared_ptr<GeodeResidentSlab> slab;
  /// Whole-slot allocation inside the slab, returned via `free` on reset.
  uint64_t allocationOffset = 0;
  /// Number of bytes occupied by this slot's allocation.
  uint64_t allocationSize = 0;

  /// Cached bind group. All bindings reference stable objects (this slot's `buffer` sub-ranges,
  /// device-owned dummy texture/sampler/identity-instance handles, and the views of a pattern
  /// fill's tile and of an active path-clip mask, which the renderer keeps across frames), so it
  /// survives frames and encoders. Rebuilt when the geometry buffer is re-allocated or a bound
  /// view changes. A fill or stroke slot holds a fill-pipeline group; a clip shape's slot holds a
  /// clip-mask-pipeline group.
  gpu::BindGroup bindGroup;

  /// Pattern tile view \ref bindGroup binds, or null when it binds the dummy pattern texture.
  /// A pattern fill whose tile view differs rebuilds the bind group before drawing.
  gpu::TextureViewRef boundPatternView;

  /// Clip-mask view \ref bindGroup binds: the path-clip mask the draw was clipped by (for a clip
  /// shape's slot, the nested mask it is intersected with), or the device's identity-coverage
  /// dummy. A draw under a different mask rebuilds the bind group before drawing.
  gpu::TextureViewRef boundClipMaskView;

  /// A byte sub-range of `buffer`. `size == 0` is never bound directly -
  /// empty SSBO regions reserve a zero-filled dummy slot wide enough for one
  /// element of the WGSL array they feed, so the shader's band-count gate
  /// keeps them un-dereferenced and the binding still passes WebGPU's
  /// minimum-size validation (matching the per-frame arena
  /// `allocStorageOrDummy` dummy).
  struct Region {
    uint64_t offset = 0;
    uint64_t size = 0;
  };

  Region bands;   ///< Horizontal band SSBO (binding 1).
  Region curves;  ///< Horizontal curve SSBO (binding 2).
  // The four grid classes below share binding 10, whose range covers all of
  // them; each one is reached through an element base in the uniform.
  Region hRefs;    ///< Horizontal curve-reference SSBO (binding 10).
  Region vBands;   ///< Vertical band SSBO (binding 8).
  Region vCurves;  ///< Vertical curve SSBO (binding 9).
  Region vRefs;    ///< Vertical curve-reference SSBO (binding 10).
  Region hGrid;    ///< Horizontal band grid (binding 10).
  Region vGrid;    ///< Vertical band grid (binding 10).
  /// Gradient paint block: transform rows, gradient geometry, and the stop
  /// ramp, as `vec4` rows addressed by the record's `paintBase` (binding 11).
  /// Always reserved, so a fill that changes from solid to gradient paint
  /// rewrites this region rather than re-laying-out the slot; a solid fill
  /// leaves it zero-filled and its record never reads it.
  Region paint;    ///< Gradient paint block within the resident buffer.
  Region uniform;  ///< Batch-level uniform block (binding 0, 256-aligned).

  /// Document-scoped record slab slot for this entity's instance record
  /// (binding 7). Solo draws bind the slot's own range; cross-entity
  /// batches bind a span of consecutive slots and index by slot index.
  GeodeRecordSlab::Slot recordSlot;
  /// Record slab that owns `recordSlot`, this device's record slab for the
  /// document; held by shared_ptr for the same reason as `slab`.
  std::shared_ptr<GeodeRecordSlab> recordSlab;

  uint32_t vertexCount = 0;  ///< Triangle-fan draw count generated from vertex_index.

  /// Frame index in which this slot was last drawn via the resident path.
  /// A slot's single uniform buffer + cached bind group can only serve ONE
  /// draw per frame (all draws recorded against a frame's command buffer
  /// read the buffer's final contents at submit time). When the same slot
  /// is drawn again in the same frame at a different transform/color
  /// (markers, repeated `<use>`), the second and later draws take records of
  /// their own from `repeatRecords` and join ordered batches; a repeat that
  /// cannot (a clip mask, gradient paint, a refused record) falls
  /// back to the per-frame arena path so it still gets its own uniform.
  /// Sentinel `~0` means "never drawn".
  uint64_t lastResidentFrame = ~uint64_t{0};

  /// Process-unique id (see `GeodeDevice::deviceId()`) of the device that
  /// created `buffer` / `bindGroup`. Every device keeps its own slots (see
  /// `GeodeResidentPathComponent`), so this always names the device drawing
  /// the slot. The draw path still checks it and treats a mismatch as
  /// non-resident, re-uploading, as a defensive cross-check: WebGPU rejects a
  /// bind group or buffer from one device inside another's render pass. `0`
  /// means "no device owns this slot yet".
  uint64_t owningDeviceId = 0;

  /// Frame index in which this slot's record was last referenced by a
  /// recorded scene-batch draw. A record slot holds ONE record; an entity
  /// drawn again in the same frame (markers, repeated `<use>`) needs another
  /// record so earlier recorded batches keep reading their own content at
  /// submit time. Such a repeat takes one of `repeatRecords`; a pending
  /// singleton converted into a batch after its slot's primary is claimed
  /// takes a per-frame temporary one.
  uint64_t lastSceneFrame = ~uint64_t{0};

  /// Records for the second and later draws of this slot within one frame,
  /// created on the first such repeat; occurrence `i` is the slot's `i + 1`-th
  /// draw of the frame. The slot's own uniform and primary record can carry
  /// only one draw per frame, so without these a repeat (each placement of a
  /// marker's content, a non-adjacent `<use>` of one source) would re-upload
  /// its geometry through the per-frame arena and build its own bind group
  /// every frame. The records read this slot's resident geometry and persist,
  /// so an unchanged frame writes none of them. They survive \ref reset for
  /// the same reason `recordSlot` does: a re-upload changes the bases inside
  /// the record bytes, which the compare-before-write already catches.
  std::unique_ptr<GeodeOccurrenceRecords> repeatRecords;
  /// Ordinal of the next repeat in the frame `repeatRecords->lastFrame`.
  size_t nextRepeatOccurrence = 0;

  /// True once `buffer` + `bindGroup` hold the current encode. Cleared
  /// when the slot is reset or when a re-upload is required.
  bool resident = false;

  /// Identity guard defending against any missed invalidation: the
  /// address of the `EncodedPath` last uploaded plus a cheap size
  /// fingerprint. If either differs at draw time the geometry is
  /// re-uploaded. Component removal is the primary invalidation; this is
  /// belt-and-suspenders for the in-place stroke-slot rebuild path.
  const void* encodedKey = nullptr;
  /// Size-based fingerprint of the last uploaded encoded path.
  uint64_t encodedFingerprint = 0;

  /// Bytes last written to the instance-record region. A draw whose
  /// recomputed record matches this skips the `writeBuffer` entirely
  /// (steady-state frames write zero record bytes).
  std::vector<uint8_t> lastRecord;

  /// Bytes last written to the gradient paint region, with the same
  /// skip-when-unchanged contract as the record and the uniform.
  std::vector<uint8_t> lastPaint;

  /// Paint published by the last paint write: the record's `paintMode` and
  /// the two gradient scalars that go with it. They live on the slot rather
  /// than travelling with each draw because a batch re-derives an instance's
  /// record at flush time, after the caller's resolved gradient is gone, and
  /// must reproduce byte-identical bytes.
  uint32_t paintMode = 0;
  uint32_t gradientSpread = 0;     ///< Last published gradient spread mode.
  uint32_t gradientStopCount = 0;  ///< Last published gradient stop count.

  /// Bytes last written to the uniform region. A draw whose recomputed
  /// uniform matches this skips the `writeBuffer` entirely (steady-state
  /// static frame => zero buffer writes); a camera/color change rewrites
  /// only this 448-byte region and keeps the cached bind group.
  std::vector<uint8_t> lastUniform;
  GeodeGeometryCacheReservation cpuMirrorReservation;  ///< Charged CPU mirror storage.
  uint64_t cpuUniformBytes = 0;  ///< Capacity bytes reserved for the cached uniform.
  uint64_t cpuRecordBytes = 0;   ///< Capacity bytes reserved for the cached record.
  uint64_t cpuPaintBytes = 0;    ///< Capacity bytes reserved for cached paint data.

  /// Replace the cached uniform's CPU-byte reservation.
  /// @param bytes New uniform mirror capacity in bytes.
  /// @return False if the document budget refuses the replacement charge.
  bool reserveUniformMirror(uint64_t bytes) {
    return reserveCpuMirrors(bytes, cpuRecordBytes, cpuPaintBytes);
  }
  /// Replace the cached record's CPU-byte reservation.
  /// @param bytes New record mirror capacity in bytes.
  /// @return False if the document budget refuses the replacement charge.
  bool reserveRecordMirror(uint64_t bytes) {
    return reserveCpuMirrors(cpuUniformBytes, bytes, cpuPaintBytes);
  }
  /// Replace the cached paint's CPU-byte reservation.
  /// @param bytes New paint mirror capacity in bytes.
  /// @return False if the document budget refuses the replacement charge.
  bool reservePaintMirror(uint64_t bytes) {
    return reserveCpuMirrors(cpuUniformBytes, cpuRecordBytes, bytes);
  }

  GeodeResidentSlot() = default;
  ~GeodeResidentSlot() {
    if (recordSlab && recordSlot.buffer.isValid()) {
      recordSlab->freeSlot(recordSlot);
    }
    reset();
  }

  GeodeResidentSlot(const GeodeResidentSlot&) = delete;
  GeodeResidentSlot& operator=(const GeodeResidentSlot&) = delete;
  /// Transfer slab handles, cached bindings, and CPU reservations.
  /// @param other Slot whose resource ownership is transferred.
  GeodeResidentSlot(GeodeResidentSlot&& other) noexcept = default;
  /// Release current geometry residence and mirrors, then take ownership of another slot.
  /// @param other Slot whose resources are moved into this one.
  /// @return This slot after the transfer.
  GeodeResidentSlot& operator=(GeodeResidentSlot&& other) noexcept {
    if (this != &other) {
      reset();
      buffer = other.buffer;
      bufferId = other.bufferId;
      other.buffer = gpu::BufferRef();
      other.bufferId = 0;
      slab = std::move(other.slab);
      allocationOffset = other.allocationOffset;
      allocationSize = other.allocationSize;
      other.allocationOffset = 0;
      other.allocationSize = 0;
      // The record slot and its owning slab move too: entt's swap-and-pop
      // removal move-assigns the surviving component over the removed one,
      // and a move that left the source's recordSlab set would let the
      // source's destructor free the SURVIVOR's record slot while its
      // cached bind group still binds it (rendering another entity's paint
      // once the slot is reused).
      recordSlot = other.recordSlot;
      recordSlab = std::move(other.recordSlab);
      other.recordSlot = GeodeRecordSlab::Slot{};
      lastSceneFrame = other.lastSceneFrame;
      repeatRecords = std::move(other.repeatRecords);
      nextRepeatOccurrence = other.nextRepeatOccurrence;
      bindGroup = std::move(other.bindGroup);
      boundPatternView = other.boundPatternView;
      other.boundPatternView = gpu::TextureViewRef();
      boundClipMaskView = other.boundClipMaskView;
      other.boundClipMaskView = gpu::TextureViewRef();
      bands = other.bands;
      curves = other.curves;
      hRefs = other.hRefs;
      vBands = other.vBands;
      vCurves = other.vCurves;
      vRefs = other.vRefs;
      hGrid = other.hGrid;
      vGrid = other.vGrid;
      paint = other.paint;
      uniform = other.uniform;
      vertexCount = other.vertexCount;
      lastResidentFrame = other.lastResidentFrame;
      resident = other.resident;
      encodedKey = other.encodedKey;
      encodedFingerprint = other.encodedFingerprint;
      owningDeviceId = other.owningDeviceId;
      lastUniform = std::move(other.lastUniform);
      lastRecord = std::move(other.lastRecord);
      lastPaint = std::move(other.lastPaint);
      cpuMirrorReservation = std::move(other.cpuMirrorReservation);
      cpuUniformBytes = other.cpuUniformBytes;
      cpuRecordBytes = other.cpuRecordBytes;
      cpuPaintBytes = other.cpuPaintBytes;
      paintMode = other.paintMode;
      gradientSpread = other.gradientSpread;
      gradientStopCount = other.gradientStopCount;
      other.resident = false;
      other.encodedKey = nullptr;
      other.owningDeviceId = 0;
      other.cpuUniformBytes = 0;
      other.cpuRecordBytes = 0;
      other.cpuPaintBytes = 0;
    }
    return *this;
  }

  /// Return the slab allocation and drop the bind group + borrowed handle.
  /// Safe to call on an empty slot. A geometry mutation can remove this
  /// component after a draw has referenced the buffer but before the
  /// frame's command encoder is submitted. Do not call `Buffer::destroy()`
  /// here: that makes the buffer unusable immediately, invalidating the already-recorded draw.
  /// Releasing our references is enough for the buffer, whose storage the slab owns; the bind
  /// group is handed to the slab instead of dropped, because draws recorded earlier this frame
  /// may still name it and have not reached the backend yet.
  void reset() {
    if (slab && allocationSize != 0) {
      GeodeResidentSlab::Allocation alloc{buffer, allocationOffset, allocationSize, bufferId};
      slab->free(alloc);
    }
    // The record slot survives geometry re-uploads: its painter-ordered
    // index is allocation-order data, not geometry data, and keeping it
    // preserves cross-entity batch contiguity across unchanged frames.
    // It is only freed by the destructor (component removal or the device's
    // entry being dropped), or replaced by the renderer's record-slab wiring
    // when the slot has no record in this device's slab yet.
    // The slab pointers themselves are intentionally kept: they outlive
    // any one residence (this device's residence for the document, in the
    // registry context, owns them), and the re-upload path needs them right
    // after reset().
    allocationOffset = 0;
    allocationSize = 0;
    buffer = gpu::BufferRef();
    bufferId = 0;
    if (slab) {
      slab->retireBindGroup(std::move(bindGroup));
    }
    bindGroup = gpu::BindGroup();
    boundPatternView = gpu::TextureViewRef();
    boundClipMaskView = gpu::TextureViewRef();
    resident = false;
    encodedKey = nullptr;
    encodedFingerprint = 0;
    owningDeviceId = 0;
    std::vector<uint8_t>().swap(lastUniform);
    std::vector<uint8_t>().swap(lastRecord);
    std::vector<uint8_t>().swap(lastPaint);
    cpuMirrorReservation.reset();
    cpuUniformBytes = 0;
    cpuRecordBytes = 0;
    cpuPaintBytes = 0;
    paintMode = 0;
    gradientSpread = 0;
    gradientStopCount = 0;
  }

private:
  bool reserveCpuMirrors(uint64_t uniformBytes, uint64_t recordBytes, uint64_t paintBytes) {
    if (uniformBytes > std::numeric_limits<uint64_t>::max() - recordBytes ||
        uniformBytes + recordBytes > std::numeric_limits<uint64_t>::max() - paintBytes) {
      return false;
    }
    const uint64_t total = uniformBytes + recordBytes + paintBytes;
    const std::shared_ptr<GeodeDocumentGeometryBudget> budget =
        slab ? slab->documentBudget() : nullptr;
    if (budget && !cpuMirrorReservation.replace(budget, total)) {
      return false;
    }
    cpuUniformBytes = uniformBytes;
    cpuRecordBytes = recordBytes;
    cpuPaintBytes = paintBytes;
    return true;
  }
};

/// GPU-resident geometry for one gradient-painted fill. Mirrors
/// \ref GeodeResidentSlot. A combined-usage
/// buffer holds the same eight analytic dual-ray SSBO regions, but the
/// uniform region holds the 704-byte gradient uniform block (stops inline,
/// `donner/gpu/shader/programs/SlugGradientSource.h`) and the cached bind group uses the
/// reflected gradient pipeline layout with the active clip mask's view (or the device-owned
/// placeholder) and the placeholder stop-ramp texture. Residence is not taken while a clip
/// polygon or a mask pass is active, exactly like the solid-fill residence gate.
struct GeodeResidentGradientSlot {
  /// Combined Storage|Uniform|CopyDst buffer, borrowed from the owning
  /// `GeodeResidentSlab` chunk; region offsets are ABSOLUTE buffer
  /// offsets. Region layout matches \ref GeodeResidentSlot, with the
  /// uniform region sized for `GradientUniforms` (704 bytes).
  gpu::BufferRef buffer;
  /// Stable identity of `buffer` (see `GeodeDevice::AllocateBufferId`), so
  /// returning the allocation matches the owning chunk by identity rather
  /// than by a handle address the allocator can recycle.
  uint64_t bufferId = 0;
  /// Slab that owns `buffer`, this device's geometry slab for the
  /// document; null until residence is established. Held by shared_ptr so
  /// the slab outlives every slot allocated from it, whichever of the two
  /// the document lets go of first.
  std::shared_ptr<GeodeResidentSlab> slab;
  /// Whole-slot allocation inside the slab, returned via `free` on reset.
  uint64_t allocationOffset = 0;
  /// Number of bytes occupied by this gradient slot's allocation.
  uint64_t allocationSize = 0;

  /// Cached gradient bind group. All bindings reference stable objects (this
  /// slot's buffer sub-ranges, \ref boundClipMaskView, and the device-owned
  /// placeholder stop-ramp texture), so it survives frames and encoders. A gradient with
  /// more stops than the uniform carries inline binds `rampBindGroup` instead.
  gpu::BindGroup bindGroup;

  /// Clip-mask view both cached groups bind: the active path-clip mask's view, which the renderer
  /// keeps across frames, or the device's identity-coverage dummy. A draw under a different mask
  /// drops both groups and builds them again.
  gpu::TextureViewRef boundClipMaskView;

  /// Stop ramp of the last draw whose gradient had more stops than the
  /// uniform carries inline, and the cached group that binds it with this
  /// slot's regions. Both are replaced only when the stops or the geometry
  /// change.
  std::shared_ptr<const GeodeGradientRamp> ramp;
  gpu::BindGroup rampBindGroup;  ///< Gradient bind group binding `ramp`.

  /// Replace `ramp` and `rampBindGroup`, parking the old ones in the slab until the next frame
  /// boundary.
  /// @param newRamp Ramp to keep. @param newBindGroup Group binding it with this slot's regions.
  void replaceRamp(std::shared_ptr<const GeodeGradientRamp> newRamp, gpu::BindGroup newBindGroup) {
    if (slab) {
      slab->retireRamp(std::move(ramp));
      slab->retireBindGroup(std::move(rampBindGroup));
    }
    ramp = std::move(newRamp);
    rampBindGroup = std::move(newBindGroup);
  }

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
  /// Size-based fingerprint of the last uploaded encoded gradient path.
  uint64_t encodedFingerprint = 0;

  /// Bytes last written to the uniform region; unchanged draws skip the write.
  std::vector<uint8_t> lastUniform;
  GeodeGeometryCacheReservation cpuMirrorReservation;  ///< Charged CPU uniform mirror storage.
  uint64_t cpuUniformBytes = 0;  ///< Capacity bytes reserved for the cached uniform.

  /// Replace the cached gradient uniform's CPU-byte reservation.
  /// @param bytes New uniform mirror capacity in bytes.
  /// @return False if the document budget refuses the replacement charge.
  bool reserveUniformMirror(uint64_t bytes) {
    const std::shared_ptr<GeodeDocumentGeometryBudget> budget =
        slab ? slab->documentBudget() : nullptr;
    if (budget && !cpuMirrorReservation.replace(budget, bytes)) {
      return false;
    }
    cpuUniformBytes = bytes;
    return true;
  }

  GeodeResidentGradientSlot() = default;
  ~GeodeResidentGradientSlot() { reset(); }

  GeodeResidentGradientSlot(const GeodeResidentGradientSlot&) = delete;
  GeodeResidentGradientSlot& operator=(const GeodeResidentGradientSlot&) = delete;
  /// Transfer slab handles, cached bindings, and CPU reservations.
  /// @param other Gradient slot whose resource ownership is transferred.
  GeodeResidentGradientSlot(GeodeResidentGradientSlot&& other) noexcept = default;
  /// Release the current slot and take ownership of another gradient slot's resources.
  /// @param other Slot whose resources are moved into this one.
  /// @return This gradient slot after the transfer.
  GeodeResidentGradientSlot& operator=(GeodeResidentGradientSlot&& other) noexcept {
    if (this != &other) {
      reset();
      buffer = other.buffer;
      bufferId = other.bufferId;
      other.buffer = gpu::BufferRef();
      other.bufferId = 0;
      slab = std::move(other.slab);
      allocationOffset = other.allocationOffset;
      allocationSize = other.allocationSize;
      other.allocationOffset = 0;
      other.allocationSize = 0;
      bindGroup = std::move(other.bindGroup);
      boundClipMaskView = other.boundClipMaskView;
      other.boundClipMaskView = gpu::TextureViewRef();
      ramp = std::move(other.ramp);
      rampBindGroup = std::move(other.rampBindGroup);
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
      cpuMirrorReservation = std::move(other.cpuMirrorReservation);
      cpuUniformBytes = other.cpuUniformBytes;
      other.resident = false;
      other.encodedKey = nullptr;
      other.owningDeviceId = 0;
      other.cpuUniformBytes = 0;
    }
    return *this;
  }

  /// Return the slab allocation and drop the bind group + borrowed handle.
  /// Never calls `Buffer::destroy()`; see GeodeResidentSlot::reset.
  void reset() {
    if (slab && allocationSize != 0) {
      GeodeResidentSlab::Allocation alloc{buffer, allocationOffset, allocationSize, bufferId};
      slab->free(alloc);
    }
    // The slab pointers themselves are intentionally kept: they outlive
    // any one residence (this device's residence for the document, in the
    // registry context, owns them), and the re-upload path needs them right
    // after reset().
    allocationOffset = 0;
    allocationSize = 0;
    buffer = gpu::BufferRef();
    bufferId = 0;
    if (slab) {
      slab->retireBindGroup(std::move(bindGroup));
    }
    bindGroup = gpu::BindGroup();
    boundClipMaskView = gpu::TextureViewRef();
    replaceRamp(nullptr, gpu::BindGroup());
    resident = false;
    encodedKey = nullptr;
    encodedFingerprint = 0;
    owningDeviceId = 0;
    std::vector<uint8_t>().swap(lastUniform);
    cpuMirrorReservation.reset();
    cpuUniformBytes = 0;
  }
};

/// GPU residence for one cached stroke outline (`GeodePathCacheComponent::StrokeVariant`).
struct GeodeResidentStrokeSlots {
  GeodeResidentSlot solid;             ///< Solid stroke's resident geometry and record.
  GeodeResidentGradientSlot gradient;  ///< Gradient-painted stroke's resident outline and paint.
};

/// Sibling of `GeodePathCacheComponent`: holds the GPU residence for an
/// entity's fill and stroke encodes. Installed lazily by `RendererGeode`
/// at the solid-fill draw sites; removed by the same entt listener that
/// clears `GeodePathCacheComponent` when geometry changes.
struct GeodeResidentPathSlots {
  /// Solid fill's resident geometry and record, one per cached fill encode variant.
  std::array<GeodeResidentSlot, kGeodeFillVariantCount> fillSlots;
  /// Stroke residence, one per cached stroke variant. Allocated on first use, so an entity that is
  /// never stroked keeps only the pointers, and at a stable address, so a pending batch can keep
  /// pointing at a slot.
  std::array<std::unique_ptr<GeodeResidentStrokeSlots>, kGeodeStrokeVariantCount> strokeSlots;
  /// Gradient-painted fill residence, one per cached fill encode variant.
  std::array<GeodeResidentGradientSlot, kGeodeFillVariantCount> gradientFillSlots;
};

/// One entity's GPU residence, one set of slots per device that draws it: a second device
/// neither reuses the first one's slots nor resets them.
struct GeodeResidentPathComponent {
  GeodePerDevice<GeodeResidentPathSlots> devices;  //!< Slots per drawing device.
};
static_assert(!std::is_copy_constructible_v<GeodeResidentPathComponent> &&
                  std::is_nothrow_move_constructible_v<GeodeResidentPathComponent> &&
                  std::is_nothrow_move_assignable_v<GeodeResidentPathComponent>,
              "the registry moves components when it compacts, and a component owns its slots");

}  // namespace donner::geode
