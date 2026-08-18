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
#include <memory>
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
 * own font-size multiplier), and the stretch factors are the `lengthAdjust`
 * scale baked into the outline before placement. Position and rotation are
 * deliberately absent: they are the placement transform, which rides in the
 * per-occurrence record.
 *
 * The floating-point fields are compared and hashed BITWISE, matching how they
 * reach the font backend: two scales that differ in the last bit produce
 * different outlines, so they must not share an entry.
 */
struct GlyphGeometryKey {
  uint64_t fontId = 0;
  uint32_t glyphIndex = 0;
  float outlineScale = 0.0f;
  float stretchScaleX = 1.0f;
  float stretchScaleY = 1.0f;

  bool operator==(const GlyphGeometryKey& other) const {
    return fontId == other.fontId && glyphIndex == other.glyphIndex &&
           bits(outlineScale) == bits(other.outlineScale) &&
           bits(stretchScaleX) == bits(other.stretchScaleX) &&
           bits(stretchScaleY) == bits(other.stretchScaleY);
  }

  /// Raw bit pattern of a float, so `-0.0f` and NaN payloads compare and hash
  /// as the distinct inputs they are rather than by numeric equality.
  static uint32_t bits(float value) {
    uint32_t out = 0;
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
  /// Renderer frame index of the last occurrence that used this entry. The
  /// eviction pass never touches an entry used in the current frame, because
  /// this frame's recorded draws still read its geometry.
  uint64_t lastUsedFrame = ~uint64_t{0};
  /// Approximate CPU footprint of the encode, used as the residence budget's
  /// unit. Slab capacity is not usable here: the slab reports whole chunks.
  uint64_t encodedBytes = 0;
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
  explicit GeodeGlyphCache(uint64_t deviceId) : owningDeviceId_(deviceId) {}

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
    auto entry = std::make_unique<GeodeGlyphResidentEntry>();
    entry->outline = std::move(outline);
    entry->encoded = std::move(encoded);
    entry->encodedBytes = EncodedBytes(entry->encoded);
    encodedBytes_ += entry->encodedBytes;
    GeodeGlyphResidentEntry* raw = entry.get();
    entries_.emplace(key, std::move(entry));
    return raw;
  }

  /// Number of live entries.
  size_t size() const { return entries_.size(); }

  /// Summed \ref GeodeGlyphResidentEntry::encodedBytes over live entries.
  uint64_t encodedBytes() const { return encodedBytes_; }

  /// Trim to budget at most once per frame. The frame-index guard makes the
  /// call idempotent no matter how many times a frame touches the cache, so
  /// the caller can put it on the accessor and cover every draw entry point.
  /// Returns the number of entries dropped.
  size_t beginFrame(uint64_t frameIndex, size_t maxEntries, uint64_t maxEncodedBytes) {
    if (frameIndex == lastEvictedFrame_) {
      return 0;
    }
    lastEvictedFrame_ = frameIndex;
    return evictToBudget(frameIndex, maxEntries, maxEncodedBytes);
  }

  /// Drop entries until the cache fits the budget, oldest-unused first.
  /// Entries whose `lastUsedFrame` equals `currentFrame` are never dropped:
  /// this frame's already-recorded draws still read their geometry, and the
  /// slab would hand the range to a later allocation in the same frame.
  /// Returns the number of entries dropped.
  size_t evictToBudget(uint64_t currentFrame, size_t maxEntries, uint64_t maxEncodedBytes) {
    if (entries_.size() <= maxEntries && encodedBytes_ <= maxEncodedBytes) {
      return 0;
    }

    // Gather the droppable entries and order them oldest-use-first. The cache
    // holds one entry per distinct glyph outline, so this list stays small
    // enough that a sort per over-budget frame is cheaper than maintaining an
    // intrusive LRU list through every lookup.
    std::vector<std::pair<uint64_t, const GlyphGeometryKey*>> candidates;
    candidates.reserve(entries_.size());
    for (const auto& [key, entry] : entries_) {
      if (entry->lastUsedFrame != currentFrame) {
        candidates.emplace_back(entry->lastUsedFrame, &key);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    size_t evicted = 0;
    for (const auto& [unusedFrame, keyPtr] : candidates) {
      if (entries_.size() <= maxEntries && encodedBytes_ <= maxEncodedBytes) {
        break;
      }
      auto it = entries_.find(*keyPtr);
      if (it == entries_.end()) {
        continue;
      }
      encodedBytes_ -= it->second->encodedBytes;
      entries_.erase(it);
      ++evicted;
    }
    return evicted;
  }

  /// Default cap on distinct cached glyph outlines. One document at one size
  /// in one font needs a few hundred; the cap bounds a document that animates
  /// font-size continuously, where every frame mints new keys.
  static constexpr size_t kDefaultMaxEntries = 1024u;
  /// Default cap on summed encode bytes.
  static constexpr uint64_t kDefaultMaxEncodedBytes = 8u << 20;

private:
  /// CPU size of an encode. Used as the budget unit because it tracks the GPU
  /// residence byte-for-byte (the resident slot uploads exactly these arrays).
  static uint64_t EncodedBytes(const EncodedPath& encoded) {
    return encoded.bands.size() * sizeof(EncodedPath::Band) +
           encoded.curves.size() * 6u * sizeof(float) +
           encoded.curveIndices.size() * sizeof(uint32_t) +
           encoded.vBands.size() * sizeof(EncodedPath::Band) +
           encoded.vCurves.size() * 6u * sizeof(float) +
           encoded.vCurveIndices.size() * sizeof(uint32_t) +
           encoded.hBandGrid.size() * sizeof(uint32_t) +
           encoded.vBandGrid.size() * sizeof(uint32_t);
  }

  uint64_t owningDeviceId_ = 0;
  uint64_t encodedBytes_ = 0;
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
 * `lastFrame` guards the one case the compare cannot: the SAME element drawn
 * twice in one frame (a `<use>` of the text, a multi-document tile pass). The
 * second pass must not rewrite records the first pass's already-recorded draw
 * reads, because every buffer write in a frame executes before every draw in
 * that frame's submit. The renderer diverts the repeat to per-frame temporary
 * slots instead.
 */
struct GeodeTextInstanceRecordComponent {
  /// One occurrence's record slot plus the bytes last written to it.
  struct Occurrence {
    GeodeRecordSlab::Slot slot;
    std::vector<uint8_t> lastRecord;
  };

  /// Occurrence slots in paint order. Index is the occurrence's ordinal
  /// within the element's draw, so it is stable across unchanged frames. A
  /// deque, not a vector: a pending batch holds pointers into these entries
  /// while later occurrences are still appending, and a reallocating growth
  /// would leave the batch writing through dangling pointers at flush.
  std::deque<Occurrence> occurrences;
  /// Slab the slots were allocated from; also keeps that slab alive so a
  /// device change cannot leave the slots pointing at a destroyed slab.
  std::shared_ptr<GeodeRecordSlab> recordSlab;
  /// Renderer frame index of the last draw that wrote these slots. Sentinel
  /// `~0` means "never drawn".
  uint64_t lastFrame = ~uint64_t{0};

  GeodeTextInstanceRecordComponent() = default;
  ~GeodeTextInstanceRecordComponent() { freeRecordSlots(); }

  GeodeTextInstanceRecordComponent(const GeodeTextInstanceRecordComponent&) = delete;
  GeodeTextInstanceRecordComponent& operator=(const GeodeTextInstanceRecordComponent&) = delete;

  GeodeTextInstanceRecordComponent(GeodeTextInstanceRecordComponent&& other) noexcept
      : occurrences(std::move(other.occurrences)),
        recordSlab(std::move(other.recordSlab)),
        lastFrame(other.lastFrame) {
    other.occurrences.clear();
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
      other.occurrences.clear();
    }
    return *this;
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
  }
};

}  // namespace donner::geode
