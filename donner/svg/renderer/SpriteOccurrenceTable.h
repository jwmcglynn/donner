#pragma once
/// @file
/// Host inputs of external `<use>` occurrences, identified so a sprite cache can key on them.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/EcsRegistry.h"
#include "donner/base/Length.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg {

/**
 * What an external `<use>` occurrence contributes to the rendering of the subtree it references:
 * the referenced root, the style the root inherits from the `<use>`, its fill and stroke (for
 * context paint), and the `x`, `y`, `width` and `height` the `<use>` places and sizes it with.
 *
 * The inherited style holds only inherited properties, so a change to a property that does not
 * inherit, such as the `<use>` element's own `opacity` or `transform`, leaves the inputs equal.
 */
struct SpriteOccurrenceInputs {
  Entity root = entt::null;    ///< Referenced root in the sprite sheet.
  PropertyRegistry inherited;  ///< Style the root inherits from the `<use>`.
  std::optional<components::ComputedResourceOpacities> resourceOpacities;  ///< Resource opacities.
  components::ResolvedPaintServer fill;    ///< The `<use>` element's fill, none or a solid color.
  components::ResolvedPaintServer stroke;  ///< The `<use>` element's stroke, none or a solid color.
  std::array<std::optional<Lengthd>, 4> geometry;  ///< The `<use>` `x`, `y`, `width`, `height`.

  /// Whether \p other describes the same occurrence inputs.
  [[nodiscard]] bool matches(const SpriteOccurrenceInputs& other) const {
    return root == other.root && geometry == other.geometry &&
           resourceOpacities == other.resourceOpacities &&
           components::IsProvablySamePaint(fill, other.fill) &&
           components::IsProvablySamePaint(stroke, other.stroke) && inherited == other.inherited;
  }
};

/**
 * Recently seen occurrence inputs of one sprite sheet, kept in the sheet's registry context.
 *
 * Each distinct set of inputs gets an id no other set in the process has had, which a sprite cache
 * key uses in place of the inputs themselves. An entry also remembers what the driver learned the
 * last time it drew the occurrence: whether its content may be cached and the bounds of what it
 * paints, valid for the sheet revisions it was measured at. Bounded to \ref kMaximumEntries, least
 * recently used first out.
 */
class SpriteOccurrenceTable {
public:
  /// Most occurrence inputs remembered per sheet.
  static constexpr std::size_t kMaximumEntries = 64;

  /// One remembered set of occurrence inputs.
  struct Entry {
    SpriteOccurrenceInputs inputs;  ///< The occurrence inputs.
    std::uint64_t id = 0;           ///< Process-unique id of \ref inputs.
    std::uint64_t lastUse = 0;      ///< Use sequence of the last lookup.

    /// Whether \ref bounds were measured at these sheet revisions.
    bool measured = false;
    std::uint64_t measuredRevision = 0;      ///< Sheet mutation revision measured at.
    std::uint64_t measuredFontRevision = 0;  ///< Sheet font resource revision measured at.
    /// Bounds of what the occurrence paints, in the `<use>` element's user space; unset when its
    /// content may not be cached.
    std::optional<Box2d> bounds;
    /// When everything the occurrence paints is one filtered element: its filter stage, which
    /// decides whether the rasterization may be resampled at any sub-pixel offset.
    std::optional<components::FilterGraph> resamplingGraph;
    /// Maps the filtered element's user space into the `<use>` element's.
    Transform2d spriteFromFilter;
    /// Painted bounds of the filtered element in its user space, when known.
    std::optional<Box2d> filteredContentBounds;

    /// Whether the measurement is current for the sheet at \p revision and \p fontRevision.
    [[nodiscard]] bool measuredAt(std::uint64_t revision, std::uint64_t fontRevision) const {
      return measured && measuredRevision == revision && measuredFontRevision == fontRevision;
    }
  };

  SpriteOccurrenceTable() = default;
  SpriteOccurrenceTable(SpriteOccurrenceTable&&) noexcept = default;
  SpriteOccurrenceTable& operator=(SpriteOccurrenceTable&&) noexcept = default;
  // Declared explicitly: the registry context copies any value whose type reports itself copyable.
  SpriteOccurrenceTable(const SpriteOccurrenceTable&) = delete;
  SpriteOccurrenceTable& operator=(const SpriteOccurrenceTable&) = delete;

  /**
   * The entry for \p inputs, created when none matches. Its address stays valid until a later
   * call evicts it, which only happens to the least recently used entry when the table is full.
   */
  Entry& findOrInsert(SpriteOccurrenceInputs inputs) {
    ++useSequence_;
    for (const std::unique_ptr<Entry>& entry : entries_) {
      if (entry->inputs.matches(inputs)) {
        entry->lastUse = useSequence_;
        return *entry;
      }
    }
    if (entries_.size() >= kMaximumEntries) {
      auto oldest = entries_.begin();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if ((*it)->lastUse < (*oldest)->lastUse) {
          oldest = it;
        }
      }
      entries_.erase(oldest);
    }
    auto entry = std::make_unique<Entry>();
    entry->inputs = std::move(inputs);
    entry->id = NextId();
    entry->lastUse = useSequence_;
    entries_.push_back(std::move(entry));
    return *entries_.back();
  }

  /// Number of remembered inputs.
  [[nodiscard]] std::size_t size() const { return entries_.size(); }

private:
  static std::uint64_t NextId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }

  std::vector<std::unique_ptr<Entry>> entries_;
  std::uint64_t useSequence_ = 0;
};

}  // namespace donner::svg
