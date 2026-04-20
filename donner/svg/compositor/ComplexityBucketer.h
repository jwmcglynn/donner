#pragma once
/// @file

#include <cstdint>
#include <unordered_map>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"

namespace donner::svg::compositor {

/**
 * Runtime configuration for `ComplexityBucketer`.
 *
 * Defaults match the design doc's illustrative constants (see
 * § Complexity Bucketing in 0025-composited_rendering.md). They are hand-tuned
 * per Non-Goal 2 ("no ML or user-history heuristics") — adjust values based
 * on benchmark results as the system matures.
 */
struct ComplexityBucketerConfig {
  /// Total number of layer slots the bucketer is allowed to produce, including
  /// reserved slots. Design-doc target is K=4 across all backends for v1.
  uint32_t targetBucketCount = 4;

  /// Slots reserved for non-bucket hint sources (interactive layer, animation,
  /// etc.). Subtracted from `targetBucketCount` to yield the usable bucket
  /// count. Default `1` leaves room for the interactive layer.
  uint32_t reservedSlots = 1;

  /// Per-subtree cost adder when any entity in the subtree has a filter
  /// (`RenderingInstanceComponent::resolvedFilter.has_value()`).
  uint32_t filterPenalty = 16;

  /// Per-subtree cost adder when any entity in the subtree has a mask
  /// (`RenderingInstanceComponent::mask.has_value()`).
  uint32_t maskPenalty = 8;

  /// Minimum subtree cost to be worth bucketing. Subtrees with cost below this
  /// threshold stay in the root. Default `1` (bucket every top-level child)
  /// for parity with the initial unit-test behavior; production callers should
  /// raise this to avoid carving cheap leaf elements into their own layers,
  /// which costs more than it saves and exposes correctness edge cases in
  /// `RendererDriver::drawEntityRange` for standalone top-level elements.
  uint32_t minCostToBucket = 1;
};

/**
 * Stats produced by `ComplexityBucketer::reconcile()`. Tests inspect these to
 * verify the bucketer considered the expected candidates, published the
 * expected hints, and dropped stale ones.
 */
struct ComplexityBucketerStats {
  /// Entities visited during the cost walk (root plus all descendants reached
  /// through the top-level children iteration).
  uint32_t entitiesEvaluated = 0;
  /// Number of top-level-root-child subtrees evaluated as candidates.
  uint32_t candidatesConsidered = 0;
  /// Hints newly published this call.
  uint32_t bucketsPublished = 0;
  /// Hints removed this call because the candidate no longer qualifies, the
  /// entity was destroyed, or the ranking pushed it off the budget.
  uint32_t bucketsDropped = 0;
  /// Post-reconcile size of the held hints map. Reflects steady state.
  uint32_t bucketsActive = 0;
};

/**
 * Phase 2.5 producer that pre-chunks a document into a small number of layers
 * based on per-subtree rasterization cost. Runs at document load and on
 * structural rebuild (both callers control when; this class is stateless
 * between calls other than its held hints).
 *
 * Algorithm (see 0025-composited_rendering.md § Complexity Bucketing):
 *
 * 1. Walk `RenderingInstanceView` in draw order. The first entity is the
 *    document root; skip it as a candidate (we never bucket the whole
 *    document).
 * 2. Identify **direct children of the root** by advancing past each child's
 *    subtree (`subtreeInfo.lastRenderedEntity`). Each direct child is a
 *    candidate; its cost is computed from the subtree walk:
 *    `cost = 1 per entity + filterPenalty * has_filter + maskPenalty * has_mask`
 *    summed across the subtree.
 * 3. Sort candidates by cost descending, ties broken by entity id ascending
 *    (deterministic).
 * 4. Publish `ComplexityBucket` hints for the top
 *    `targetBucketCount - reservedSlots` candidates.
 *
 * Restricting candidates to top-level root children sidesteps the deferred-pop
 * correctness concern: by construction, top-level children have no isolation
 * / clip / filter / mask ancestor (other than the root itself), so bucketing
 * them cannot split a group that must stay atomic.
 *
 * **v1 simplifications (tracked for Phase 2.5 followups):**
 *
 * - Cost walk does NOT perform bbox overlap rejection. Two candidates with
 *   visually overlapping subtrees may both be bucketed. Correctness is
 *   preserved (the resolver composes layers in order); optimality may suffer
 *   for pathological documents.
 * - Candidates are limited to top-level root children. Deeper cost peaks
 *   (e.g., a deeply-nested `<g>` with many filter children) are not promoted
 *   in v1 — they'd require a deferred-pop walk we're deferring.
 * - Each reconcile recomputes from scratch. No incremental update on partial
 *   mutations.
 *
 * Held hints map (`bucketHints_`) is non-copyable, movable. The RAII pattern
 * ensures dropped entries clean up their `CompositorHintComponent` entries.
 */
class ComplexityBucketer {
public:
  explicit ComplexityBucketer(ComplexityBucketerConfig config = {}) : config_(config) {}

  ~ComplexityBucketer() = default;
  ComplexityBucketer(const ComplexityBucketer&) = delete;
  ComplexityBucketer& operator=(const ComplexityBucketer&) = delete;
  ComplexityBucketer(ComplexityBucketer&&) noexcept = default;
  ComplexityBucketer& operator=(ComplexityBucketer&&) noexcept = default;

  /**
   * Recompute the bucket partition for the current document state. Intended to
   * be called on document load and when `RenderTreeState::needsFullRebuild` is
   * true. Publishes new `ComplexityBucket` hints, drops stale ones, and updates
   * stats. Idempotent on unchanged input.
   */
  void reconcile(Registry& registry);

  /// Drop all held bucket hints. Callers use this when the registry is being
  /// reset (e.g. `CompositorController::resetAllLayers`) so stale bucket
  /// assignments don't linger into the rebuilt document.
  void clear() { bucketHints_.clear(); }

  /// Like `clear()`, but releases each hint's registry pointer first so
  /// the dtor becomes a no-op. Used from `CompositorController::resetAll
  /// Layers` after `setDocument` has replaced the entity space — the old
  /// `CompositorHintComponent`s are already gone, and `registry.valid()`
  /// on the rebuilt registry would SIGSEGV against the stale entity IDs.
  void releaseAllHintsNoClean() {
    for (auto& [entity, hint] : bucketHints_) {
      hint.release();
    }
    bucketHints_.clear();
  }

  /// Rebuild the bucket hint set against a new entity space after a
  /// structurally-identical `setDocument`. Mirror of `MandatoryHint
  /// Detector::rebuildForReplacedDocument` — release the stale hints
  /// without touching the old entity ids, then run `reconcile` on the
  /// new registry. Because the bucketer's decisions are a pure function
  /// of the render-tree shape + complexity costs, a structurally-equal
  /// document produces the identical bucket set keyed on the new ids.
  void rebuildForReplacedDocument(Registry& newRegistry) {
    for (auto& [entity, hint] : bucketHints_) {
      hint.release();
    }
    bucketHints_.clear();
    reconcile(newRegistry);
  }

  [[nodiscard]] const ComplexityBucketerStats& stats() const { return stats_; }

  [[nodiscard]] const ComplexityBucketerConfig& config() const { return config_; }

private:
  ComplexityBucketerConfig config_;
  ComplexityBucketerStats stats_;

  /// One scoped `ComplexityBucket` hint per currently-bucketed subtree root.
  std::unordered_map<Entity, ScopedCompositorHint> bucketHints_;
};

}  // namespace donner::svg::compositor
