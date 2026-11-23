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
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "donner/base/FillRule.h"
#include "donner/base/Path.h"
#include "donner/base/Vector2.h"
#include "donner/svg/renderer/geode/GeodePathEncoder.h"
#include "donner/svg/renderer/geode/GeodePerDevice.h"
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
 * stable when a different face is selected. An outline-only synthesis change
 * can keep the same face, so its per-glyph permission is also part of the key.
 * Superseded entries age out through the residency budget.
 */
struct GlyphGeometryKey {
  uint64_t fontId = 0;                ///< Versioned font handle identity.
  uint32_t glyphIndex = 0;            ///< Glyph index within the selected font.
  float outlineScale = 0.0f;          ///< Scale applied while extracting the outline.
  float stretchScaleX = 1.0f;         ///< Horizontal stretch applied to the outline.
  float stretchScaleY = 1.0f;         ///< Vertical stretch applied to the outline.
  double rotateDegrees = 0.0;         ///< Rotation applied to the outline, in degrees.
  double fillTolerance = 0.1;         ///< Device-derived cubic approximation tolerance.
  bool allowSyntheticOblique = true;  ///< Whether the outline may be synthetically obliqued.

  /// Compare every key field by raw bits, including signed zero and NaN payloads.
  /// @param other Geometry key to compare.
  bool operator==(const GlyphGeometryKey& other) const {
    return fontId == other.fontId && glyphIndex == other.glyphIndex &&
           bits(outlineScale) == bits(other.outlineScale) &&
           bits(stretchScaleX) == bits(other.stretchScaleX) &&
           bits(stretchScaleY) == bits(other.stretchScaleY) &&
           bits(rotateDegrees) == bits(other.rotateDegrees) &&
           bits(fillTolerance) == bits(other.fillTolerance) &&
           allowSyntheticOblique == other.allowSyntheticOblique;
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
  /// Hash every bitwise field of a glyph geometry key.
  /// @param key Key to hash.
  size_t operator()(const GlyphGeometryKey& key) const {
    uint64_t h = key.fontId;
    h = mix(h ^ key.glyphIndex);
    h = mix(h ^ GlyphGeometryKey::bits(key.outlineScale));
    h = mix(h ^ GlyphGeometryKey::bits(key.stretchScaleX));
    h = mix(h ^ GlyphGeometryKey::bits(key.stretchScaleY));
    h = mix(h ^ GlyphGeometryKey::bits(key.rotateDegrees));
    h = mix(h ^ GlyphGeometryKey::bits(key.fillTolerance));
    h = mix(h ^ static_cast<uint64_t>(key.allowSyntheticOblique));
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
  /// Total retained CPU bytes for the entry, its outline, and its encode.
  uint64_t retainedBytes = 0;
};

/**
 * Document-scoped cache of unique glyph outlines and their GPU residence.
 *
 * Bound to one device exactly like `GeodeResidentSlab`: a document rendered by
 * several devices keeps one cache per device, so no entry can hand one device's
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
  /// the aggregate cross-document admission gate. Closes admission for the
  /// rest of the frame when eviction cannot make room.
  GeodeGlyphResidentEntry* insertWithinBudget(const GlyphGeometryKey& key, Path&& outline,
                                              EncodedPath&& encoded, uint64_t oldestOpenFrame,
                                              size_t maxEntries, uint64_t maxRetainedBytes,
                                              size_t* evictedOut = nullptr) {
    if (GeodeGlyphResidentEntry* existing = find(key)) {
      return existing;
    }
    const std::optional<uint64_t> entryBytes = EntryRetainedBytes(outline, encoded);
    if (maxEntries == 0u || !entryBytes.has_value() || *entryBytes > maxRetainedBytes ||
        !admissionOpen_) {
      return nullptr;
    }
    const size_t evicted =
        evictToBudget(oldestOpenFrame, maxEntries - 1u, maxRetainedBytes - *entryBytes);
    if (evictedOut != nullptr) {
      *evictedOut += evicted;
    }
    if (entries_.size() >= maxEntries || retainedBytes_ > maxRetainedBytes - *entryBytes) {
      admissionOpen_ = false;
      return nullptr;
    }
    return insert(key, std::move(outline), std::move(encoded));
  }

  /// Whether this frame may still add entries. See \ref closeAdmission.
  bool admissionOpen() const { return admissionOpen_; }

  /// Stop admitting entries until the next \ref beginFrame. Called once an eviction pass leaves
  /// only entries an open frame still uses: nothing more can be evicted this frame, so every
  /// further admission would rescan the whole cache and fail.
  void closeAdmission() { admissionOpen_ = false; }

  /// Number of live entries.
  size_t size() const { return entries_.size(); }

  /// Summed \ref GeodeGlyphResidentEntry::encodedBytes over live entries.
  uint64_t encodedBytes() const { return encodedBytes_; }

  /// Total CPU bytes retained by cached entries, outlines, and encodes.
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
    admissionOpen_ = true;
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

  /// Default cap on summed retained entry bytes. The entry count is capped by the renderer's glyph
  /// cap; this cap bounds a document that animates font-size continuously, where every frame mints
  /// new keys.
  static constexpr uint64_t kDefaultMaxRetainedBytes = 8u << 20;

  /// Bytes one entry retains besides its outline and encode vectors: the entry, and the map node
  /// holding its key, owning pointer, cached hash, and bucket link. Charging it keeps entries with
  /// empty outlines inside the byte cap.
  static constexpr uint64_t kEntryOverheadBytes =
      sizeof(GeodeGlyphResidentEntry) + sizeof(GlyphGeometryKey) + 4u * sizeof(void*);

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
        !std::in_range<uint64_t>(*outlineBytes)) {
      return std::nullopt;
    }
    const uint64_t outlineSize = static_cast<uint64_t>(*outlineBytes);
    if (outlineSize > std::numeric_limits<uint64_t>::max() - kEntryOverheadBytes ||
        *encodedBytes > std::numeric_limits<uint64_t>::max() - kEntryOverheadBytes - outlineSize) {
      return std::nullopt;
    }
    return kEntryOverheadBytes + outlineSize + *encodedBytes;
  }

  uint64_t owningDeviceId_ = 0;
  std::shared_ptr<GeodeDocumentGeometryBudget> budget_;
  GeodeGeometryCacheReservation budgetReservation_;
  uint64_t encodedBytes_ = 0;
  uint64_t retainedBytes_ = 0;
  /// Frame index of the last trim; `~0` = never trimmed. See beginFrame().
  uint64_t lastEvictedFrame_ = ~uint64_t{0};
  /// Cleared by \ref closeAdmission, set again by \ref beginFrame.
  bool admissionOpen_ = true;
  std::unordered_map<GlyphGeometryKey, std::unique_ptr<GeodeGlyphResidentEntry>,
                     GlyphGeometryKeyHash>
      entries_;
};

/// One text element's persistent occurrence records, one \ref GeodeOccurrenceRecords per
/// device that draws it: records hold slots in that device's record slab, and a device's frame
/// generation means nothing to another device.
struct GeodeTextInstanceResidencyComponent {
  GeodePerDevice<GeodeOccurrenceRecords> devices;  //!< Records per drawing device.
};
static_assert(!std::is_copy_constructible_v<GeodeTextInstanceResidencyComponent> &&
                  std::is_nothrow_move_constructible_v<GeodeTextInstanceResidencyComponent> &&
                  std::is_nothrow_move_assignable_v<GeodeTextInstanceResidencyComponent>,
              "the registry moves components when it compacts, and a component owns its records");

/**
 * Retained encodes and GPU residence of the shapes one text element paints outside the glyph
 * cache's solid fills: placed glyph outlines filled with a gradient or a pattern, or with a solid
 * along a text path; the joined stroke outlines of its glyphs; and its decoration lines and their
 * strokes. These shapes are built in the element's own space every frame, so the draw that paints
 * them is the same draw a per-frame encode would feed, and keeping them lets an unchanged frame
 * encode, upload and bind nothing for them.
 *
 * Shape `i` is the element's `i`th such draw in paint order. A draw reuses shape `i` only when the
 * path it is about to paint, its fill rule and the encode tolerance are bitwise identical to what
 * the shape holds; any difference replaces the shape, with fresh residence. Because the key is the
 * geometry itself, no edit can leave a shape stale: an edit that changes what is painted changes
 * the path, and an edit that only changes the paint reaches the draw's uniform as it does for any
 * resident fill.
 *
 * Bounded by the renderer's shape cap (\ref kDefaultMaximumShapes) per element and device; draws
 * past it, or one the document's cache budget refuses, encode for the frame as before, and a
 * refused draw leaves its ordinal empty, so the shapes after it are still kept. Every shape's
 * retained bytes (\ref kShapeOverheadBytes plus its path and encode capacities) and the owner
 * vector's capacity are charged to the document's cache budget, and its GPU geometry to the
 * document's resident budget through the slab.
 *
 * A shape is replaced or dropped only while no frame that has not submitted drew the element
 * (\ref lastFrame, a device-scoped generation, as for \ref GeodeOccurrenceRecords). Its
 * residence then returns through the slab's deferred free and bind-group retirement, which also
 * covers an element destroyed while such a frame is open.
 */
class GeodeTextShapeCache {
public:
  /// One retained shape: its geometry key, encode and residence.
  struct Shape {
    Path path;                               //!< Exact geometry the encode was built from.
    uint64_t toleranceBits = 0;              //!< Raw bits of the encode tolerance.
    FillRule rule = FillRule::NonZero;       //!< Fill rule the encode was built for.
    EncodedPath encoded;                     //!< Encode of \ref path; its address is stable.
    GeodeResidentSlot slot;                  //!< Residence for solid and pattern paint.
    GeodeResidentGradientSlot gradientSlot;  //!< Residence for gradient paint.
    uint64_t retainedBytes = 0;              //!< Bytes charged for this shape.
  };
  /// Owner of one shape, so its address stays put while the owner vector grows. An ordinal whose
  /// draw could not be kept holds a null owner, so the shapes after it keep their own ordinals.
  using ShapeOwner = std::unique_ptr<Shape>;
  static_assert(sizeof(Shape) <= 2048,
                "a shape's fixed record is charged per shape; keep it small");

  /// Shapes one element keeps per device unless a test lowers it: more than the glyph occurrences
  /// of a long paragraph painted with a gradient or pattern, each with a stroke group.
  static constexpr size_t kDefaultMaximumShapes = 4096;
  /// Bytes a shape retains besides its path and encode vectors.
  static constexpr uint64_t kShapeOverheadBytes = sizeof(Shape);

  GeodeTextShapeCache() = default;
  ~GeodeTextShapeCache() = default;
  GeodeTextShapeCache(const GeodeTextShapeCache&) = delete;
  GeodeTextShapeCache& operator=(const GeodeTextShapeCache&) = delete;
  GeodeTextShapeCache(GeodeTextShapeCache&&) noexcept = default;
  GeodeTextShapeCache& operator=(GeodeTextShapeCache&&) noexcept = default;

  /// Device-scoped frame generation of the last draw that used these shapes; zero means never.
  uint64_t lastFrame = 0;

  /// Shape \p index when it holds exactly \p path, \p rule and \p tolerance, else null.
  /// @param index Ordinal of the draw within the element.
  /// @param path Geometry about to be painted.
  /// @param rule Fill rule of the draw.
  /// @param tolerance Encode tolerance of the draw.
  Shape* find(size_t index, const Path& path, FillRule rule, double tolerance) {
    if (index >= shapes_.size() || !shapes_[index]) {
      return nullptr;
    }
    Shape& shape = *shapes_[index];
    if (shape.rule != rule || shape.toleranceBits != GlyphGeometryKey::bits(tolerance) ||
        !SameGeometry(shape.path, path)) {
      return nullptr;
    }
    return &shape;
  }

  /**
   * Keeps \p encoded as shape \p index, replacing whatever the shape held, and returns it. Ordinals
   * between the last held shape and \p index stay empty. Fails without consuming \p encoded when
   * \p index is at or past \p maximumShapes, a size overflows, or \p budget refuses the charge.
   *
   * @param index Ordinal of the draw within the element.
   * @param path Geometry \p encoded was built from.
   * @param rule Fill rule of \p encoded.
   * @param tolerance Encode tolerance of \p encoded.
   * @param encoded Encode to keep; moved from only on success.
   * @param budget Document budget charged for the cache.
   * @param maximumShapes Most shapes this cache may hold.
   */
  Shape* store(size_t index, const Path& path, FillRule rule, double tolerance,
               EncodedPath& encoded, const std::shared_ptr<GeodeDocumentGeometryBudget>& budget,
               size_t maximumShapes) {
    if (!budget || index >= maximumShapes) {
      return nullptr;
    }
    const std::optional<uint64_t> shapeBytes = ShapeBytes(path, encoded);
    if (!shapeBytes.has_value()) {
      return nullptr;
    }
    const uint64_t replacedBytes =
        index < shapes_.size() && shapes_[index] ? shapes_[index]->retainedBytes : 0u;
    const size_t capacity = ownerCapacityFor(index, maximumShapes);
    const uint64_t keptBytes = shapeBytes_ - replacedBytes;
    if (!charge(budget, capacity, keptBytes, *shapeBytes)) {
      return nullptr;
    }
    budget_ = budget;
    if (index >= shapes_.size()) {
      shapes_.reserve(capacity);
      shapes_.resize(index + 1u);
    }
    // A fresh shape: the old one's residence goes back through the slab's deferred free, and the
    // new encode can never be mistaken for the old one by the slot's upload check.
    shapes_[index] = std::make_unique<Shape>();
    Shape& shape = *shapes_[index];
    shape.path = path;
    shape.toleranceBits = GlyphGeometryKey::bits(tolerance);
    shape.rule = rule;
    shape.encoded = std::move(encoded);
    shape.retainedBytes = *shapeBytes;
    shapeBytes_ = keptBytes + *shapeBytes;
    return &shape;
  }

  /// Drops shape \p index and its charge, leaving its ordinal empty: a draw whose new geometry
  /// could not be kept does not keep the old geometry it replaced either.
  /// @param index Ordinal of the shape.
  void release(size_t index) {
    if (index >= shapes_.size() || !shapes_[index]) {
      return;
    }
    shapeBytes_ -= shapes_[index]->retainedBytes;
    shapes_[index].reset();
    // Shrinking lowers the charge, which a budget never refuses.
    (void)reservation_.replace(
        budget_, static_cast<uint64_t>(shapes_.capacity()) * sizeof(ShapeOwner) + shapeBytes_);
  }

  /// Drops the shapes from \p count on, which the element's last draw did not reach.
  /// @param count Number of shapes to keep.
  void trim(size_t count) {
    if (count >= shapes_.size()) {
      return;
    }
    for (size_t index = count; index < shapes_.size(); ++index) {
      if (shapes_[index]) {
        shapeBytes_ -= shapes_[index]->retainedBytes;
      }
    }
    shapes_.resize(count);
    if (shapes_.empty()) {
      std::vector<ShapeOwner>().swap(shapes_);
      reservation_.reset();
      budget_.reset();
      shapeBytes_ = 0;
      return;
    }
    // Shrinking lowers the charge, which a budget never refuses.
    (void)reservation_.replace(
        budget_, static_cast<uint64_t>(shapes_.capacity()) * sizeof(ShapeOwner) + shapeBytes_);
  }

  /// Number of ordinals held, empty ones included, until the next draw trims those it did not
  /// reach.
  size_t size() const { return shapes_.size(); }

  /// Number of ordinals that hold a kept shape.
  size_t keptShapes() const {
    return static_cast<size_t>(std::count_if(
        shapes_.begin(), shapes_.end(), [](const ShapeOwner& shape) { return shape != nullptr; }));
  }

  /// Bytes charged to the document's cache budget for this cache.
  uint64_t retainedBytes() const { return reservation_.bytes(); }

private:
  /// Bytes one shape holding \p path and \p encoded retains, or nullopt on overflow.
  static std::optional<uint64_t> ShapeBytes(const Path& path, const EncodedPath& encoded) {
    const std::optional<std::size_t> pathBytes = path.retainedBytes();
    const std::size_t encodedBytes = encoded.retainedBytes();
    if (!pathBytes.has_value() || encodedBytes == std::numeric_limits<std::size_t>::max() ||
        *pathBytes > std::numeric_limits<uint64_t>::max() - kShapeOverheadBytes - encodedBytes) {
      return std::nullopt;
    }
    return kShapeOverheadBytes + *pathBytes + encodedBytes;
  }

  /// Owner vector capacity after storing shape \p index (below \p maximumShapes): a vector too
  /// small for it grows to twice its size or to the index, whichever is larger, never past
  /// \p maximumShapes.
  size_t ownerCapacityFor(size_t index, size_t maximumShapes) const {
    if (index < shapes_.capacity()) {
      return shapes_.capacity();
    }
    return std::min(maximumShapes, std::max(index + 1u, 2u * shapes_.size()));
  }

  /// Charges \p budget for an owner vector of \p capacity, the \p keptBytes of the other shapes
  /// and a stored shape of \p shapeBytes. @return False, keeping the old charge, on refusal or
  /// overflow.
  bool charge(const std::shared_ptr<GeodeDocumentGeometryBudget>& budget, size_t capacity,
              uint64_t keptBytes, uint64_t shapeBytes) {
    const uint64_t ownerBytes = static_cast<uint64_t>(capacity) * sizeof(ShapeOwner);
    if (keptBytes > std::numeric_limits<uint64_t>::max() - shapeBytes ||
        keptBytes + shapeBytes > std::numeric_limits<uint64_t>::max() - ownerBytes) {
      return false;
    }
    return reservation_.replace(budget, ownerBytes + keptBytes + shapeBytes);
  }

  /// Whether \p a and \p b have bitwise identical commands and points.
  static bool SameGeometry(const Path& a, const Path& b) {
    const std::span<const Path::Command> aCommands = a.commands();
    const std::span<const Path::Command> bCommands = b.commands();
    const std::span<const Vector2d> aPoints = a.points();
    const std::span<const Vector2d> bPoints = b.points();
    if (aCommands.size() != bCommands.size() || aPoints.size() != bPoints.size()) {
      return false;
    }
    for (size_t i = 0; i < aCommands.size(); ++i) {
      if (aCommands[i].verb != bCommands[i].verb ||
          aCommands[i].pointIndex != bCommands[i].pointIndex ||
          aCommands[i].isInternal != bCommands[i].isInternal) {
        return false;
      }
    }
    for (size_t i = 0; i < aPoints.size(); ++i) {
      if (GlyphGeometryKey::bits(aPoints[i].x) != GlyphGeometryKey::bits(bPoints[i].x) ||
          GlyphGeometryKey::bits(aPoints[i].y) != GlyphGeometryKey::bits(bPoints[i].y)) {
        return false;
      }
    }
    return true;
  }

  std::vector<ShapeOwner> shapes_;
  uint64_t shapeBytes_ = 0;
  std::shared_ptr<GeodeDocumentGeometryBudget> budget_;
  GeodeGeometryCacheReservation reservation_;
};

/// One text element's retained shapes, one \ref GeodeTextShapeCache per device that draws it: the
/// shapes' residence lives in that device's slab.
struct GeodeTextShapeResidencyComponent {
  GeodePerDevice<GeodeTextShapeCache> devices;  //!< Shapes per drawing device.
};
static_assert(!std::is_copy_constructible_v<GeodeTextShapeResidencyComponent> &&
                  std::is_nothrow_move_constructible_v<GeodeTextShapeResidencyComponent> &&
                  std::is_nothrow_move_assignable_v<GeodeTextShapeResidencyComponent>,
              "the registry moves components when it compacts, and a component owns its shapes");

}  // namespace donner::geode
