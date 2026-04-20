#pragma once
/// @file

#include <cstdint>

#include "donner/base/EcsRegistry.h"

namespace donner::svg::compositor {

/**
 * Per-call policy for `LayerResolver::resolve()`. Lets the caller disable
 * specific hint sources without mutating the registry. Disabled-source hints
 * are still collected into the candidate list (for stats) but their
 * contribution to the weight sum is zeroed — so an entity whose only hint
 * is from a disabled source ends up at layer 0 (root).
 */
struct ResolveOptions {
  bool enableInteractionHints = true;
  bool enableAnimationHints = true;
  bool enableComplexityBucketHints = true;
  // Mandatory and Explicit are always honored — they represent SVG semantics
  // and the explicit escape-hatch API, not optional optimizations.
};

/// Stats produced by `LayerResolver::resolve()`. Tests inspect these.
struct LayerResolverStats {
  /// Total number of entities with a non-empty `CompositorHintComponent` seen this pass.
  uint32_t candidatesEvaluated = 0;
  /// Number of entities that received a non-zero `ComputedLayerAssignmentComponent` this pass.
  uint32_t layersAssigned = 0;
  /// Count of non-mandatory candidates that lost their slot to the budget cap.
  /// Mandatory candidates that exceed the budget are not counted here (they
  /// always take precedence); each such eviction logs to stderr and increments
  /// this counter too so tests can catch over-budget mandatory sets.
  uint32_t budgetExhaustions = 0;
};

/**
 * Stateless resolver that collapses `CompositorHintComponent` entries into
 * `ComputedLayerAssignmentComponent` assignments, subject to a layer budget.
 *
 * Algorithm (see design doc § Layer Promotion Cascade):
 *   1. Collect candidates — entities with at least one hint.
 *   2. Mandatory hints are non-contestable; they always take a layer.
 *   3. Rank remaining candidates by total weight descending, ties broken by
 *      numeric entity id ascending (deterministic; draw-order tie-breaking
 *      is a Phase 2 follow-up).
 *   4. Assign consecutive `layerId`s starting at 1, up to `maxLayers`.
 *      Losers have their `ComputedLayerAssignmentComponent` removed (or
 *      never attached).
 *
 * Determinism: repeated calls on unchanged registry state produce identical
 * assignments. `ComputedLayerAssignmentComponent` is only written when the
 * assigned `layerId` changes, reducing ECS churn.
 */
class LayerResolver {
public:
  /**
   * Run one resolution pass over `registry`.
   *
   * @param registry Registry to inspect and update.
   * @param maxLayers Maximum number of non-root layers to assign. Defaults to
   *                  `kMaxCompositorLayers` (see `CompositorController.h`). The
   *                  parameter is a bare integer here to keep `LayerResolver.h`
   *                  independent of the controller.
   * @param options   Per-source enable/disable flags (default: all sources on).
   */
  void resolve(Registry& registry, uint32_t maxLayers, const ResolveOptions& options = {});

  /// Stats from the most recent `resolve()` call. Zeroed before each run.
  [[nodiscard]] const LayerResolverStats& stats() const { return stats_; }

private:
  LayerResolverStats stats_;
};

}  // namespace donner::svg::compositor
