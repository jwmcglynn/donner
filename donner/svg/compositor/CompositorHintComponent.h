#pragma once
/// @file

#include <algorithm>
#include <climits>
#include <cstdint>
#include <limits>
#include <vector>

namespace donner::svg::compositor {

/**
 * Enumerates the subsystems that can publish a compositor hint for an entity.
 *
 * Hints are weighted inputs to the `LayerResolver`. Multiple sources may
 * simultaneously hint the same entity (for example, a selected entity that
 * also has `opacity < 1` receives both a `Mandatory` and an `Interaction`
 * or `Explicit` hint).
 *
 * All five values are defined; Phase 1 produces only `Mandatory` and
 * `Explicit`. `Animation`, `Interaction`, and `ComplexityBucket` are
 * reserved for Phase 2+ and must compile/resolve cleanly even though no
 * source publishes them yet.
 */
enum class HintSource : uint8_t {
  /// SVG semantics force isolation (opacity < 1, filter, mask, etc.). Infinite weight.
  Mandatory,
  /// Published by the animation system on animated subtrees. Reserved for Phase 2.
  Animation,
  /// Published by the editor selection/drag subsystem. Reserved for Phase 2.
  Interaction,
  /// Published by the complexity bucketer at load / structural rebuild. Reserved for Phase 2.5.
  ComplexityBucket,
  /// Escape hatch for explicit callers (tests, `CompositorController::promoteEntity`).
  Explicit,
};

/// A single `{source, weight}` entry stored by `CompositorHintComponent`.
struct HintEntry {
  /// Source subsystem that published this hint.
  HintSource source;
  /// Weight (0..65535). `0xFFFF` combined with `HintSource::Mandatory` is the infinite-weight sentinel.
  uint16_t weight;
};

/**
 * Author-layer ECS component carrying the set of weighted hints published
 * against an entity.
 *
 * Multiple sources may hint the same entity at the same time. The resolver
 * sums weights to rank candidates for layer assignment, except that any
 * `Mandatory` hint short-circuits the calculation to infinite weight
 * (`UINT32_MAX`) — mandatory hints are non-contestable.
 *
 * Storage is a plain `std::vector<HintEntry>` — see Non-Goal 3 (sequential,
 * single-threaded) in the design doc. Hints are typically 1–3 per entity.
 *
 * This component mirrors `StyleComponent` in the CSS engine: it is the
 * author-layer input to a resolver that produces a `ComputedLayerAssignmentComponent`.
 *
 * @ingroup ecs_components
 */
struct CompositorHintComponent {
  /// All hint entries currently attached to this entity.
  std::vector<HintEntry> entries;

  /// Append a hint. Does not deduplicate — the same `(source, weight)` pair may appear more than once.
  void addHint(HintSource source, uint16_t weight) {
    entries.push_back(HintEntry{source, weight});
  }

  /**
   * Remove exactly one entry whose `source` and `weight` both match. Used
   * by `ScopedCompositorHint` on destruction. If duplicate entries exist,
   * only the first matching one is removed — callers that stacked duplicates
   * should drop their handles in LIFO order.
   *
   * @returns true if an entry was removed, false if no match was found.
   */
  bool removeFirstMatching(HintSource source, uint16_t weight) {
    auto it = std::find_if(entries.begin(), entries.end(), [&](const HintEntry& e) {
      return e.source == source && e.weight == weight;
    });
    if (it == entries.end()) {
      return false;
    }
    entries.erase(it);
    return true;
  }

  /**
   * Saturating sum of all entry weights.
   *
   * Any `Mandatory` hint short-circuits the result to `UINT32_MAX` — the
   * infinite-weight sentinel. Otherwise returns the clamped sum of
   * `weight` fields (clamped at `UINT32_MAX - 1` so `Mandatory` remains
   * distinguishable). With a `std::vector` of `uint16_t` weights the sum
   * cannot realistically overflow `uint32_t`, but we clamp defensively.
   */
  [[nodiscard]] uint32_t totalWeight() const {
    uint32_t sum = 0;
    for (const auto& entry : entries) {
      if (entry.source == HintSource::Mandatory) {
        return std::numeric_limits<uint32_t>::max();
      }
      const uint32_t next = sum + static_cast<uint32_t>(entry.weight);
      // Defensive clamp; keep below UINT32_MAX so Mandatory remains distinguishable.
      if (next < sum) {
        sum = std::numeric_limits<uint32_t>::max() - 1;
      } else {
        sum = next;
      }
    }
    return sum;
  }

  /// Returns true if the hint list is empty.
  [[nodiscard]] bool empty() const { return entries.empty(); }
};

}  // namespace donner::svg::compositor
