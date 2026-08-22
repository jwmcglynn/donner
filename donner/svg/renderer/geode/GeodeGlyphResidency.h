#pragma once
/// @file
/// Glyph-identity-keyed CPU encode + GPU residence for Geode text rendering.
///
/// A text run draws the same handful of outlines over and over: the letter "e"
/// in a paragraph is one outline placed at fifty positions. Encoding and
/// uploading that outline once per occurrence per frame is the dominant cost of
/// a text frame - the outline has to be fetched from the font backend,
/// transformed into place, banded into analytic dual-ray data, and pushed to
/// the GPU, every time.
///
/// This cache breaks the occurrence apart: everything that depends only on
/// glyph identity (the outline, its encode, and its GPU residence) is kept
/// once, and everything that depends on the occurrence (position, rotation,
/// colour) rides in a per-occurrence instance record. A repeated glyph then
/// costs one record instead of a re-encode plus a re-upload.
///
/// Lives in `donner::geode` (not `donner::svg::geode`) to match the other
/// Geode types referenced unqualified via `geode::` inside `donner::svg`.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "donner/base/Path.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodeResidentPathComponent.h"

namespace donner::geode {

/**
 * Identity of one unique glyph outline: every input that can change the
 * encoded geometry, and nothing that cannot.
 *
 * `fontId` is the font handle's opaque identifier, `outlineScale` is the
 * effective scale handed to the font backend (the run scale times the glyph's
 * own font-size multiplier), the stretch factors are the `lengthAdjust` scale,
 * and `rotateDegrees` is the per-glyph rotation - all four are baked into the
 * outline before placement. Position is deliberately absent: it is the
 * placement transform, which rides in the per-occurrence record.
 *
 * Rotation belongs here rather than in the placement because a resident
 * outline is also the space its rasteriser works in, and that space has to stay
 * aligned with the pixel grid. The cost is one cached outline per distinct
 * angle: a whole run rotated by one angle still collapses to one entry, and a
 * `textPath`, which rotates every glyph differently, degrades to about one
 * entry per glyph - which is what an uncached renderer pays anyway.
 *
 * The numeric fields are compared and hashed BITWISE, matching how they reach
 * the font backend: two scales or angles that differ in the last bit produce
 * different outlines, so they must not share an entry.
 *
 * `fontId` carries the font handle's version bits, so an unloaded font whose
 * slot is later reused cannot be mistaken for the original. It is also not
 * stable across a style mutation that makes the document re-resolve its fonts:
 * the re-resolved font is a new identity and its glyphs are rebuilt. That costs
 * a rebuild on a mutating document rather than serving stale geometry, and the
 * superseded entries age out through the residency budget.
 */
struct GlyphGeometryKey {
  uint64_t fontId = 0;
  uint32_t glyphIndex = 0;
  float outlineScale = 0.0f;
  float stretchScaleX = 1.0f;
  float stretchScaleY = 1.0f;
  double rotateDegrees = 0.0;

  bool operator==(const GlyphGeometryKey& other) const {
    return fontId == other.fontId && glyphIndex == other.glyphIndex &&
           bits(outlineScale) == bits(other.outlineScale) &&
           bits(stretchScaleX) == bits(other.stretchScaleX) &&
           bits(stretchScaleY) == bits(other.stretchScaleY) &&
           bits(rotateDegrees) == bits(other.rotateDegrees);
  }

  /// Raw bit pattern of a float, so `-0.0f` and NaN payloads compare and hash
  /// as the distinct inputs they are rather than by numeric equality.
  static uint32_t bits(float value) {
    uint32_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
  }

  /// Raw bit pattern of a double; same policy as the float overload.
  static uint64_t bits(double value) {
    uint64_t out = 0;
    std::memcpy(&out, &value, sizeof(out));
    return out;
  }
};

/// Hash for \ref GlyphGeometryKey. Mixes with the 64-bit splitmix finalizer so
/// the low-entropy fields (small glyph indices, a handful of scales) still
/// spread across buckets.
struct GlyphGeometryKeyHash {
  size_t operator()(const GlyphGeometryKey& key) const {
    uint64_t h = key.fontId;
    h = mix(h ^ key.glyphIndex);
    h = mix(h ^ GlyphGeometryKey::bits(key.outlineScale));
    h = mix(h ^ GlyphGeometryKey::bits(key.stretchScaleX));
    h = mix(h ^ GlyphGeometryKey::bits(key.stretchScaleY));
    h = mix(h ^ GlyphGeometryKey::bits(key.rotateDegrees));
    return static_cast<size_t>(h);
  }

private:
  static uint64_t mix(uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
  }
};

/**
 * One unique glyph outline: the CPU outline, its analytic encode, and its GPU
 * residence.
 *
 * The entry owns the outline and the encode so their addresses are stable for
 * as long as the entry lives; `slot.encodedKey` points at `encoded`, and the
 * pattern / gradient / stroke text paths borrow `outline` to build a placed
 * path without going back to the font backend.
 */
struct GeodeGlyphResidentEntry {
  /// Unplaced outline (stretch baked in). Empty for glyphs the font has no
  /// vector outline for; the entry is still cached so the miss is not repaid
  /// every frame.
  Path outline;
  /// Analytic dual-ray encode of `outline`. Empty when `outline` is.
  EncodedPath encoded;
  /// Persistent GPU residence for `encoded`.
  GeodeResidentSlot slot;
  /// Device-scoped frame generation of the last occurrence that used this
  /// entry. The eviction pass never drops an entry a frame that has not
  /// submitted touched, because that frame's recorded draws still read its
  /// geometry. Zero means "never used"; generations start at 1, so an entry
  /// that somehow never reached a draw is evictable immediately.
  uint64_t lastUsedFrame = 0;
  /// Approximate CPU footprint of the encode, used as the residence budget's
  /// unit. Slab capacity is not usable here: the slab reports whole chunks.
  uint64_t encodedBytes = 0;
  /// Total retained CPU bytes for the outline and encode.
  uint64_t retainedBytes = 0;
};

/**
 * Document-scoped cache of unique glyph outlines and their GPU residence.
 *
 * Bound to one device exactly like `GeodeResidentSlab`: a document rendered by
 * a second device gets a fresh cache, so no entry can hand a stale device's
 * buffer or bind group to another device's render pass.
 *
 * Eviction is generation-based rather than strictly ordered: entries carry the
 * frame index of their last use, and when the cache is over budget the oldest
 * unused entries are dropped first. Dropping an entry destroys its
 * `GeodeResidentSlot`, which returns the slab range through the slab's
 * deferred free list, so the range only becomes reusable at the NEXT frame's
 * merge - never inside the frame whose recorded draws still reference it.
 * Entries used in the current frame are never candidates for the same reason.
 *
 * Not thread-safe; the renderer serializes one frame per device at a time.
 */
class GeodeGlyphCache {
public:
  /// Create an empty cache bound to `deviceId`.
  explicit GeodeGlyphCache(uint64_t deviceId,
                           std::shared_ptr<GeodeDocumentGeometryBudget> budget = nullptr)
      : owningDeviceId_(deviceId), budget_(std::move(budget)) {}

  GeodeGlyphCache(const GeodeGlyphCache&) = delete;
  GeodeGlyphCache& operator=(const GeodeGlyphCache&) = delete;

  /// Device this cache's residence was built on.
  uint64_t owningDeviceId() const { return owningDeviceId_; }

  /// Look up `key`, returning null on a miss. Does not mark the entry used;
  /// the caller does that once it commits to drawing.
  GeodeGlyphResidentEntry* find(const GlyphGeometryKey& key) {
    auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : it->second.get();
  }

  /// Insert an entry for `key`, taking ownership of `outline` and its encode.
  /// The returned address is stable for the entry's lifetime.
  GeodeGlyphResidentEntry* insert(const GlyphGeometryKey& key, Path&& outline,
                                  EncodedPath&& encoded) {
    if (GeodeGlyphResidentEntry* existing = find(key)) {
      return existing;
    }
    const std::optional<uint64_t> entryBytes = EntryRetainedBytes(outline, encoded);
    if (!entryBytes.has_value() ||
        *entryBytes > std::numeric_limits<uint64_t>::max() - retainedBytes_) {
      return nullptr;
    }
    const std::optional<uint64_t> encodedBytes = EncodedBytes(encoded);
    if (!encodedBytes.has_value() ||
        *encodedBytes > std::numeric_limits<uint64_t>::max() - encodedBytes_) {
      return nullptr;
    }
    if (budget_ && !budgetReservation_.replace(budget_, retainedBytes_ + *entryBytes)) {
      return nullptr;
    }
    // try_emplace, not emplace: emplace constructs the node before it checks
    // for a duplicate key and destroys it on collision, which would hand the
    // caller a pointer into freed storage and leave the byte total crediting an
    // entry that is not in the map.
    auto [it, inserted] = entries_.try_emplace(key);
    if (!inserted) {
      if (budget_) {
        (void)budgetReservation_.replace(budget_, retainedBytes_);
      }
      return it->second.get();
    }
    it->second = std::make_unique<GeodeGlyphResidentEntry>();
    GeodeGlyphResidentEntry* entry = it->second.get();
    entry->outline = std::move(outline);
    entry->encoded = std::move(encoded);
    entry->encodedBytes = *encodedBytes;
    entry->retainedBytes = *entryBytes;
    encodedBytes_ += entry->encodedBytes;
    retainedBytes_ += entry->retainedBytes;
    return entry;
  }

  /// Insert through the per-cache entry/byte envelope after evicting entries
  /// old enough to be safe. The family reservation in @ref insert remains
  /// the aggregate cross-document admission gate.
  GeodeGlyphResidentEntry* insertWithinBudget(const GlyphGeometryKey& key, Path&& outline,
                                              EncodedPath&& encoded, uint64_t oldestOpenFrame,
                                              size_t maxEntries, uint64_t maxRetainedBytes,
                                              size_t* evictedOut = nullptr) {
    if (GeodeGlyphResidentEntry* existing = find(key)) {
      return existing;
    }
    const std::optional<uint64_t> entryBytes = EntryRetainedBytes(outline, encoded);
    if (maxEntries == 0u || !entryBytes.has_value() || *entryBytes > maxRetainedBytes) {
      return nullptr;
    }
    const size_t evicted =
        evictToBudget(oldestOpenFrame, maxEntries - 1u, maxRetainedBytes - *entryBytes);
    if (evictedOut != nullptr) {
      *evictedOut += evicted;
    }
    if (entries_.size() >= maxEntries || retainedBytes_ > maxRetainedBytes - *entryBytes) {
      return nullptr;
    }
    return insert(key, std::move(outline), std::move(encoded));
  }

  /// Number of live entries.
  size_t size() const { return entries_.size(); }

  /// Summed \ref GeodeGlyphResidentEntry::encodedBytes over live entries.
  uint64_t encodedBytes() const { return encodedBytes_; }

  /// Total CPU bytes retained by cached outlines and encodes.
  uint64_t retainedBytes() const { return retainedBytes_; }

  /// Trim to budget at most once per frame. The frame-index guard makes the
  /// call idempotent no matter how many times a frame touches the cache, so
  /// the caller can put it on the accessor and cover every draw entry point.
  /// Returns the number of entries dropped.
  size_t beginFrame(uint64_t frameIndex, uint64_t oldestOpenFrame, size_t maxEntries,
                    uint64_t maxRetainedBytes) {
    if (frameIndex == lastEvictedFrame_) {
      return 0;
    }
    lastEvictedFrame_ = frameIndex;
    return evictToBudget(oldestOpenFrame, maxEntries, maxRetainedBytes);
  }

  /// Drop entries until the cache fits the budget, oldest-unused first.
  ///
  /// An entry last used at or after `oldestOpenFrame` is never dropped: some
  /// frame that touched it has not submitted, its recorded draws still read
  /// that geometry, and dropping the entry would return the slab range for a
  /// later allocation to overwrite. `oldestOpenFrame` is device-scoped, so an
  /// offscreen pass nested inside an outer frame cannot free the outer frame's
  /// geometry out from under it. Returns the number of entries dropped.
  size_t evictToBudget(uint64_t oldestOpenFrame, size_t maxEntries, uint64_t maxRetainedBytes) {
    if (entries_.size() <= maxEntries && retainedBytes_ <= maxRetainedBytes) {
      return 0;
    }

    // Gather the droppable entries and order them oldest-use-first. The cache
    // holds one entry per distinct glyph outline, so this list stays small
    // enough that a sort per over-budget frame is cheaper than maintaining an
    // intrusive LRU list through every lookup.
    std::vector<std::pair<uint64_t, const GlyphGeometryKey*>> candidates;
    candidates.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
      if (entry->lastUsedFrame < oldestOpenFrame) {
        candidates.emplace_back(entry->lastUsedFrame, &key);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    size_t evicted = 0;
    for (const auto& [unusedFrame, keyPtr] : candidates) {
      if (entries_.size() <= maxEntries && retainedBytes_ <= maxRetainedBytes) {
        break;
      }
      auto it = entries_.find(*keyPtr);
      if (it == entries_.end()) {
        continue;
      }
      encodedBytes_ -= it->second->encodedBytes;
      retainedBytes_ -= it->second->retainedBytes;
      if (budget_) {
        (void)budgetReservation_.replace(budget_, retainedBytes_);
      }
      entries_.erase(it);
      ++evicted;
    }
    return evicted;
  }

  /// Default cap on distinct cached glyph outlines. One document at one size
  /// in one font needs a few hundred; the cap bounds a document that animates
  /// font-size continuously, where every frame mints new keys.
  static constexpr size_t kDefaultMaxEntries = 1024u;
  /// Default cap on summed retained outline and encode bytes.
  static constexpr uint64_t kDefaultMaxRetainedBytes = 8u << 20;

private:
  template <typename T>
  static bool AddCapacityBytes(uint64_t& total, const std::vector<T>& values) {
    if (values.capacity() > std::numeric_limits<uint64_t>::max() / sizeof(T)) {
      return false;
    }
    const uint64_t bytes = static_cast<uint64_t>(values.capacity()) * sizeof(T);
    if (bytes > std::numeric_limits<uint64_t>::max() - total) {
      return false;
    }
    total += bytes;
    return true;
  }

  /// CPU vector capacities retained by one encoded path.
  static std::optional<uint64_t> EncodedBytes(const EncodedPath& encoded) {
    uint64_t total = 0;
    if (!AddCapacityBytes(total, encoded.bands) || !AddCapacityBytes(total, encoded.curves) ||
        !AddCapacityBytes(total, encoded.curveIndices) ||
        !AddCapacityBytes(total, encoded.vBands) || !AddCapacityBytes(total, encoded.vCurves) ||
        !AddCapacityBytes(total, encoded.vCurveIndices) ||
        !AddCapacityBytes(total, encoded.hBandGrid) ||
        !AddCapacityBytes(total, encoded.vBandGrid)) {
      return std::nullopt;
    }
    return total;
  }

  static std::optional<uint64_t> EntryRetainedBytes(const Path& outline,
                                                    const EncodedPath& encoded) {
    const std::optional<std::size_t> outlineBytes = outline.retainedBytes();
    const std::optional<uint64_t> encodedBytes = EncodedBytes(encoded);
    if (!outlineBytes.has_value() || !encodedBytes.has_value() ||
        *outlineBytes > std::numeric_limits<uint64_t>::max() - *encodedBytes) {
      return std::nullopt;
    }
    return static_cast<uint64_t>(*outlineBytes) + *encodedBytes;
  }

  uint64_t owningDeviceId_ = 0;
  std::shared_ptr<GeodeDocumentGeometryBudget> budget_;
  GeodeGeometryCacheReservation budgetReservation_;
  uint64_t encodedBytes_ = 0;
  uint64_t retainedBytes_ = 0;
  /// Frame index of the last trim; `~0` = never trimmed. See beginFrame().
  uint64_t lastEvictedFrame_ = ~uint64_t{0};
  std::unordered_map<GlyphGeometryKey, std::unique_ptr<GeodeGlyphResidentEntry>,
                     GlyphGeometryKeyHash>
      entries_;
};

/**
 * Persistent per-occurrence instance records for one text element.
 *
 * Glyph geometry is shared, so the thing that has to exist once per OCCURRENCE
 * is the record: transform, colour, and the shared outline's chunk-relative
 * geometry bases. Handing every occurrence a fresh record slot each frame would
 * work, but it would also write 256 bytes per glyph per frame forever, which
 * keeps a static text frame off the zero-bytes steady state the rest of the
 * renderer reaches.
 *
 * So the slots live on the text element and persist. Occurrence `i` of the
 * element always writes slot `i`, the bytes are compared before writing, and an
 * unchanged frame writes nothing. Slots are bump-allocated together on first
 * use, which keeps their indices consecutive - exactly the property a batch
 * needs to cover them with one draw.
 *
 * No explicit invalidation is needed when the text changes: a re-laid-out
 * element simply produces different record bytes for the affected slots, the
 * compare misses, and those slots are rewritten. A shorter run leaves trailing
 * slots unused (and free for the element to grow back into); a longer one
 * appends.
 *
 * `lastFrame` guards the one case the compare cannot: the same element written
 * twice while an earlier writer's draws are still unsubmitted - the same
 * element drawn twice in one frame (a `<use>` of the text), or an offscreen
 * pass rendering the same document inside an outer frame that has already
 * recorded its glyph batch. The second writer must not rewrite those records,
 * because every buffer write in a frame executes before every draw in that
 * frame's submit. `lastFrame` holds a DEVICE-scoped generation
 * (`GeodeDevice::beginFrameGeneration`) so the comparison covers renderers
 * sharing a device, and the renderer diverts the second writer to per-frame
 * temporary slots instead.
 */
struct GeodeTextInstanceRecordComponent {
  /// One occurrence's record slot plus the bytes last written to it.
  struct Occurrence {
    GeodeRecordSlab::Slot slot;
    std::vector<uint8_t> lastRecord;
  };
  static constexpr uint64_t kProjectedBytesPerOccurrence =
      sizeof(Occurrence) + sizeof(InstanceRecord);

  /// Occurrence slots in paint order. Index is the occurrence's ordinal
  /// within the element's draw, so it is stable across unchanged frames. A
  /// deque, not a vector: a pending batch holds pointers into these entries
  /// while later occurrences are still appending, and a reallocating growth
  /// would leave the batch writing through dangling pointers at flush.
  std::deque<Occurrence> occurrences;
  /// Slab the slots were allocated from; also keeps that slab alive so a
  /// device change cannot leave the slots pointing at a destroyed slab.
  std::shared_ptr<GeodeRecordSlab> recordSlab;
  /// Device-scoped frame generation of the last draw that wrote these slots.
  /// Zero means "never written": generations start at 1, and the guard reads
  /// this as "older than every open frame", which is what an untouched
  /// component is.
  uint64_t lastFrame = 0;
  std::shared_ptr<GeodeDocumentGeometryBudget> cpuBudget;
  GeodeGeometryCacheReservation cpuReservation;
  uint64_t cpuRetainedBytes = 0;

  GeodeTextInstanceRecordComponent() = default;
  ~GeodeTextInstanceRecordComponent() { freeRecordSlots(); }

  GeodeTextInstanceRecordComponent(const GeodeTextInstanceRecordComponent&) = delete;
  GeodeTextInstanceRecordComponent& operator=(const GeodeTextInstanceRecordComponent&) = delete;

  GeodeTextInstanceRecordComponent(GeodeTextInstanceRecordComponent&& other) noexcept
      : occurrences(std::move(other.occurrences)),
        recordSlab(std::move(other.recordSlab)),
        lastFrame(other.lastFrame),
        cpuBudget(std::move(other.cpuBudget)),
        cpuReservation(std::move(other.cpuReservation)),
        cpuRetainedBytes(other.cpuRetainedBytes) {
    other.occurrences.clear();
    other.cpuRetainedBytes = 0;
  }

  GeodeTextInstanceRecordComponent& operator=(GeodeTextInstanceRecordComponent&& other) noexcept {
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
      other.occurrences.clear();
      other.cpuRetainedBytes = 0;
    }
    return *this;
  }

  bool reserveOccurrence(std::shared_ptr<GeodeDocumentGeometryBudget> budget) {
    if (cpuRetainedBytes > std::numeric_limits<uint64_t>::max() - kProjectedBytesPerOccurrence ||
        !budget ||
        !cpuReservation.replace(budget, cpuRetainedBytes + kProjectedBytesPerOccurrence)) {
      return false;
    }
    cpuBudget = std::move(budget);
    cpuRetainedBytes += kProjectedBytesPerOccurrence;
    return true;
  }

  void rollbackOccurrence() {
    if (cpuRetainedBytes < kProjectedBytesPerOccurrence || !cpuBudget) {
      return;
    }
    cpuRetainedBytes -= kProjectedBytesPerOccurrence;
    (void)cpuReservation.replace(cpuBudget, cpuRetainedBytes);
  }

  /// Return every slot to the slab (deferred to the next frame's merge, like
  /// every other record free) and drop them.
  void freeRecordSlots() {
    if (recordSlab) {
      for (const Occurrence& occurrence : occurrences) {
        if (occurrence.slot.buffer) {
          recordSlab->freeSlot(occurrence.slot);
        }
      }
    }
    occurrences.clear();
    cpuReservation.reset();
    cpuBudget.reset();
    cpuRetainedBytes = 0;
  }
};

}  // namespace donner::geode
