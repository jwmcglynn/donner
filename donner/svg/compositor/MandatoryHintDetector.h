#pragma once
/// @file

#include <cstdint>
#include <unordered_map>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"

namespace donner::svg::components {
struct RenderingInstanceComponent;
}  // namespace donner::svg::components

namespace donner::svg::compositor {

/**
 * Stats produced by `MandatoryHintDetector::reconcile()`. Tests inspect these to
 * verify that the detector published / dropped the right number of hints on a
 * given pass.
 */
struct MandatoryHintDetectorStats {
  /// Number of entities visited with a `RenderingInstanceComponent` this call.
  uint32_t candidatesEvaluated = 0;
  /// Number of `Mandatory` hints newly published this call.
  uint32_t hintsPublished = 0;
  /// Number of hints removed this call because the entity no longer qualifies
  /// (signal cleared) or was destroyed.
  uint32_t hintsDropped = 0;
  /// Post-reconcile size of the internal hint map: the number of `Mandatory`
  /// hints currently held by the detector after this call. Reflects the
  /// steady-state, not the delta.
  uint32_t hintsActive = 0;
};

/**
 * Subsystem that publishes `Mandatory` `CompositorHint` entries for SVG
 * features that force isolated compositing.
 *
 * A `RenderingInstanceComponent` qualifies for a mandatory hint when any of the
 * following is true (per the design doc § Mandatory hints):
 *   - `isolatedLayer == true` (covers `opacity < 1`, `mix-blend-mode`,
 *     `isolation: isolate`).
 *   - `resolvedFilter.has_value()` (SVG `filter`).
 *   - `mask.has_value()` (SVG `mask`).
 *
 * Other fallback-inducing features (`clipPath`, markers, external paint
 * servers) intentionally do NOT force a mandatory promotion — that's the
 * narrower classification handled by `CompositorController::detectFallbackReasons`.
 *
 * The detector owns one `ScopedCompositorHint` per currently-qualifying entity.
 * `reconcile()` diffs the current qualifying set against the held hints so that
 * running it twice in a row on an unchanged registry is a no-op.
 *
 * Non-copyable; movable. The internal hint map is movable and moves preserve
 * the scoped hints (moved-from handles are inert per `ScopedCompositorHint`'s
 * RAII contract).
 */
class MandatoryHintDetector {
public:
  /// Default constructor.
  MandatoryHintDetector() = default;

  /// Destructor. Drops all outstanding hints via `ScopedCompositorHint` RAII.
  ~MandatoryHintDetector() = default;

  MandatoryHintDetector(const MandatoryHintDetector&) = delete;
  MandatoryHintDetector& operator=(const MandatoryHintDetector&) = delete;

  MandatoryHintDetector(MandatoryHintDetector&&) noexcept = default;
  MandatoryHintDetector& operator=(MandatoryHintDetector&&) noexcept = default;

  /**
   * Walk all entities with `RenderingInstanceComponent`; publish a `Mandatory`
   * hint on each entity whose instance exhibits a mandatory-compositing feature
   * (opacity < 1, filter, mask, mix-blend-mode, isolation) and drop hints for
   * entities that no longer qualify. Idempotent: running `reconcile` twice in
   * a row with no ECS changes produces no hint churn.
   *
   * @param registry Registry to inspect.
   */
  void reconcile(Registry& registry);

  /// Drop all held hints. Callers use this when the registry is being reset
  /// (e.g. `CompositorController::resetAllLayers`) so stale hints don't
  /// linger into the rebuilt document.
  void clear() { hints_.clear(); }

  /// Defuse every outstanding hint's registry pointer, then drop them.
  /// Used from `CompositorController::resetAllLayers` when the underlying
  /// document has been replaced in place — the old Registry was destroyed
  /// and a new one constructed at the same storage address, so the hints'
  /// cached `Registry*` now points at a live object that knows nothing
  /// about the old entity IDs. Running `~ScopedCompositorHint` normally
  /// would call `registry.valid(old_entity)` and SIGSEGV inside entt's
  /// sparse-set lookup. The old `CompositorHintComponent`s went down with
  /// the old entity space, so there's nothing to clean up; just stop the
  /// dtors from touching the rebuilt registry.
  void releaseAllHintsNoClean() {
    for (auto& [entity, hint] : hints_) {
      hint.release();
    }
    hints_.clear();
  }

  /// Rebuild the hint set against a new entity space after a structurally-
  /// identical `setDocument`. The detector's hints are derived from
  /// `RenderingInstanceComponent` features (opacity < 1, filter, mask,
  /// isolation…) — rather than surgical-remap the old hint set, we release
  /// the stale hints (they point into a destroyed entt sparse set) and
  /// run `reconcile()` against the new registry. The detector's `reconcile`
  /// is deterministic on the ECS state, so a structurally-identical
  /// re-parse yields the identical hint set, keyed on the new entity ids.
  void rebuildForReplacedDocument(Registry& newRegistry) {
    for (auto& [entity, hint] : hints_) {
      hint.release();
    }
    hints_.clear();
    reconcile(newRegistry);
  }

  /// Stats from the most recent `reconcile()` call. Zeroed on entry.
  [[nodiscard]] const MandatoryHintDetectorStats& stats() const { return stats_; }

private:
  /// Returns true if the given rendering instance exhibits any of the three
  /// mandatory-compositing signals.
  [[nodiscard]] static bool qualifies(const components::RenderingInstanceComponent& instance);

  /// One scoped `Mandatory` hint per currently-qualifying entity.
  std::unordered_map<Entity, ScopedCompositorHint> hints_;

  MandatoryHintDetectorStats stats_;
};

}  // namespace donner::svg::compositor
