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

  [[nodiscard]] const ComplexityBucketerStats& stats() const { return stats_; }

  [[nodiscard]] const ComplexityBucketerConfig& config() const { return config_; }

private:
  ComplexityBucketerConfig config_;
  ComplexityBucketerStats stats_;

  /// One scoped `ComplexityBucket` hint per currently-bucketed subtree root.
  std::unordered_map<Entity, ScopedCompositorHint> bucketHints_;
};

}  // namespace donner::svg::compositor
