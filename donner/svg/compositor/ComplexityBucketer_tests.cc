#include "donner/svg/compositor/ComplexityBucketer.h"

#include <gtest/gtest.h>

#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/compositor/CompositorHintComponent.h"

namespace donner::svg::compositor {

namespace {

using components::RenderingInstanceComponent;
using components::SubtreeInfo;

class ComplexityBucketerTest : public ::testing::Test {
protected:
  /// Create an entity and attach a `RenderingInstanceComponent`. By default the
  /// instance has no subtree info (cost-walk treats it as a leaf).
  Entity makeInstance() {
    const Entity e = registry_.create();
    registry_.emplace<RenderingInstanceComponent>(e);
    return e;
  }

  /// Set `subtreeInfo.lastRenderedEntity` on an entity so the cost walker knows
  /// how far to advance the view when treating this entity as a subtree root.
  void setSubtreeEnd(Entity root, Entity end) {
    auto& inst = registry_.get<RenderingInstanceComponent>(root);
    SubtreeInfo info;
    info.lastRenderedEntity = end;
    inst.subtreeInfo = info;
  }

  /// Count how many `CompositorHintComponent` entries of the given source are
  /// attached to @p entity. Used to assert hint lifecycle.
  size_t countHints(Entity entity, HintSource source) const {
    const auto* component = registry_.try_get<CompositorHintComponent>(entity);
    if (component == nullptr) {
      return 0;
    }
    size_t count = 0;
    for (const auto& entry : component->entries) {
      if (entry.source == source) {
        ++count;
      }
    }
    return count;
  }

  Registry registry_;
};

}  // namespace

TEST_F(ComplexityBucketerTest, EmptyRegistryProducesNothing) {
  ComplexityBucketer bucketer;
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().entitiesEvaluated, 0u);
  EXPECT_EQ(bucketer.stats().candidatesConsidered, 0u);
  EXPECT_EQ(bucketer.stats().bucketsPublished, 0u);
  EXPECT_EQ(bucketer.stats().bucketsActive, 0u);
}

TEST_F(ComplexityBucketerTest, RootOnlyProducesNoBuckets) {
  // Only the root exists; no children to bucket.
  const Entity root = makeInstance();
  static_cast<void>(root);

  ComplexityBucketer bucketer;
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().entitiesEvaluated, 1u)
      << "root is visited but not a candidate";
  EXPECT_EQ(bucketer.stats().candidatesConsidered, 0u);
  EXPECT_EQ(bucketer.stats().bucketsPublished, 0u);
}

TEST_F(ComplexityBucketerTest, FlatChildrenWithinBudgetAllBucketed) {
  // root with 3 leaf children. Budget = K(4) - reserved(1) = 3.
  const Entity root = makeInstance();
  const Entity c1 = makeInstance();
  const Entity c2 = makeInstance();
  const Entity c3 = makeInstance();
  setSubtreeEnd(root, c3);

  ComplexityBucketer bucketer;
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().candidatesConsidered, 3u);
  EXPECT_EQ(bucketer.stats().bucketsPublished, 3u);
  EXPECT_EQ(bucketer.stats().bucketsActive, 3u);
  EXPECT_EQ(countHints(c1, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c2, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c3, HintSource::ComplexityBucket), 1u);
}

TEST_F(ComplexityBucketerTest, BudgetCapClipsLowerRanked) {
  // 5 equal-cost children, but budget is only 3. The 3 earliest (by entity id
  // — ties broken ascending) win.
  const Entity root = makeInstance();
  const Entity c1 = makeInstance();
  const Entity c2 = makeInstance();
  const Entity c3 = makeInstance();
  const Entity c4 = makeInstance();
  const Entity c5 = makeInstance();
  setSubtreeEnd(root, c5);
  static_cast<void>(c4);

  ComplexityBucketer bucketer;
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().candidatesConsidered, 5u);
  EXPECT_EQ(bucketer.stats().bucketsPublished, 3u);

  EXPECT_EQ(countHints(c1, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c2, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c3, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c4, HintSource::ComplexityBucket), 0u) << "lower-ranked entity is evicted";
  EXPECT_EQ(countHints(c5, HintSource::ComplexityBucket), 0u);
}

TEST_F(ComplexityBucketerTest, FilterCostDominatesPlainChild) {
  // Two children: one with a filter, one plain. The filter child wins.
  // Budget = 1 (K=2, reserved=1).
  const Entity root = makeInstance();
  const Entity plain = makeInstance();
  const Entity filtered = makeInstance();
  setSubtreeEnd(root, filtered);

  // Attach a filter to the second child.
  auto& filteredInstance = registry_.get<RenderingInstanceComponent>(filtered);
  filteredInstance.resolvedFilter = components::ResolvedFilterEffect{std::vector<FilterEffect>{}};

  ComplexityBucketer bucketer(ComplexityBucketerConfig{.targetBucketCount = 2});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsPublished, 1u);
  EXPECT_EQ(countHints(filtered, HintSource::ComplexityBucket), 1u)
      << "filter penalty makes filtered subtree win";
  EXPECT_EQ(countHints(plain, HintSource::ComplexityBucket), 0u);
}

TEST_F(ComplexityBucketerTest, MaskAddsCostBelowFilter) {
  // Three children: plain, masked, filtered. Budget = 2 (K=3, reserved=1).
  // Expected: filtered > masked > plain (filterPenalty=16 > maskPenalty=8).
  const Entity root = makeInstance();
  const Entity plain = makeInstance();
  const Entity masked = makeInstance();
  const Entity filtered = makeInstance();
  setSubtreeEnd(root, filtered);

  auto& maskedInstance = registry_.get<RenderingInstanceComponent>(masked);
  maskedInstance.mask = components::ResolvedMask{ResolvedReference{EntityHandle()}, std::nullopt,
                                                   MaskContentUnits::Default};

  auto& filteredInstance = registry_.get<RenderingInstanceComponent>(filtered);
  filteredInstance.resolvedFilter = components::ResolvedFilterEffect{std::vector<FilterEffect>{}};

  ComplexityBucketer bucketer(ComplexityBucketerConfig{.targetBucketCount = 3});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsPublished, 2u);
  EXPECT_EQ(countHints(filtered, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(masked, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(plain, HintSource::ComplexityBucket), 0u)
      << "plain subtree loses to both filtered and masked";
}

TEST_F(ComplexityBucketerTest, ReconcileIsIdempotent) {
  // 3 children; first reconcile publishes 3. Second reconcile on unchanged
  // registry should publish 0 and drop 0.
  const Entity root = makeInstance();
  const Entity c1 = makeInstance();
  const Entity c2 = makeInstance();
  const Entity c3 = makeInstance();
  setSubtreeEnd(root, c3);

  ComplexityBucketer bucketer;
  bucketer.reconcile(registry_);
  ASSERT_EQ(bucketer.stats().bucketsPublished, 3u);

  bucketer.reconcile(registry_);
  EXPECT_EQ(bucketer.stats().bucketsPublished, 0u) << "steady state publishes nothing";
  EXPECT_EQ(bucketer.stats().bucketsDropped, 0u);
  EXPECT_EQ(bucketer.stats().bucketsActive, 3u);
  EXPECT_EQ(countHints(c1, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c2, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c3, HintSource::ComplexityBucket), 1u);
}

TEST_F(ComplexityBucketerTest, StaleHintDroppedWhenEntityDestroyed) {
  const Entity root = makeInstance();
  const Entity c1 = makeInstance();
  const Entity c2 = makeInstance();
  setSubtreeEnd(root, c2);

  ComplexityBucketer bucketer;
  bucketer.reconcile(registry_);
  ASSERT_EQ(bucketer.stats().bucketsPublished, 2u);

  registry_.destroy(c1);
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsDropped, 1u)
      << "destroyed entity's hint is removed on reconcile";
  EXPECT_EQ(bucketer.stats().bucketsActive, 1u);
}

TEST_F(ComplexityBucketerTest, Deterministic) {
  // Three children with equal cost. Ties break by entity id ascending.
  // Running reconcile 10 times on an unchanged registry produces the same
  // winning set each run (no churn beyond the first pass).
  const Entity root = makeInstance();
  const Entity c1 = makeInstance();
  const Entity c2 = makeInstance();
  const Entity c3 = makeInstance();
  setSubtreeEnd(root, c3);

  ComplexityBucketer bucketer;
  bucketer.reconcile(registry_);
  const auto after1 = std::vector<bool>{
      countHints(c1, HintSource::ComplexityBucket) == 1u,
      countHints(c2, HintSource::ComplexityBucket) == 1u,
      countHints(c3, HintSource::ComplexityBucket) == 1u,
  };

  for (int i = 0; i < 9; ++i) {
    bucketer.reconcile(registry_);
  }

  EXPECT_EQ(countHints(c1, HintSource::ComplexityBucket) == 1u, after1[0]);
  EXPECT_EQ(countHints(c2, HintSource::ComplexityBucket) == 1u, after1[1]);
  EXPECT_EQ(countHints(c3, HintSource::ComplexityBucket) == 1u, after1[2]);
}

TEST_F(ComplexityBucketerTest, ReservedSlotsReduceBudget) {
  // 5 children; K=5, reserved=3 → budget=2. Top 2 by entity id win.
  const Entity root = makeInstance();
  const Entity c1 = makeInstance();
  const Entity c2 = makeInstance();
  const Entity c3 = makeInstance();
  const Entity c4 = makeInstance();
  const Entity c5 = makeInstance();
  setSubtreeEnd(root, c5);
  static_cast<void>(c1);
  static_cast<void>(c4);

  ComplexityBucketer bucketer(
      ComplexityBucketerConfig{.targetBucketCount = 5, .reservedSlots = 3});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsPublished, 2u);
  EXPECT_EQ(countHints(c1, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c2, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(c3, HintSource::ComplexityBucket), 0u);
}

// === Bucket boundary respect ================================================
// These tests document the v1 invariant: the bucketer never splits a
// clip-path / mask / filter / isolation group across buckets. v1 sidesteps
// the problem by limiting candidates to top-level root children (whose
// subtrees are atomic by construction). The tests below verify this holds
// even when the subtree *internally* contains compositing features — the
// whole subtree still goes in one bucket.
// ============================================================================

TEST_F(ComplexityBucketerTest, SubtreeWithInternalFilterStaysIntactInOneBucket) {
  // root with a child that has an internal filter. Since v1 only considers
  // top-level root children as candidates, the child with a filter is the
  // subtree root — it becomes one bucket, filter and all. The filter
  // doesn't split the subtree.
  const Entity root = makeInstance();
  const Entity child = makeInstance();
  setSubtreeEnd(root, child);

  // Attach a filter to the child (which IS the subtree root in this case).
  auto& childInstance = registry_.get<RenderingInstanceComponent>(child);
  childInstance.resolvedFilter = components::ResolvedFilterEffect{std::vector<FilterEffect>{}};

  ComplexityBucketer bucketer(ComplexityBucketerConfig{.targetBucketCount = 2});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsPublished, 1u);
  EXPECT_EQ(countHints(child, HintSource::ComplexityBucket), 1u)
      << "subtree with internal filter is bucketed atomically — filter stays with its subtree";
}

TEST_F(ComplexityBucketerTest, SubtreeWithInternalMaskStaysIntactInOneBucket) {
  const Entity root = makeInstance();
  const Entity child = makeInstance();
  setSubtreeEnd(root, child);

  auto& childInstance = registry_.get<RenderingInstanceComponent>(child);
  childInstance.mask = components::ResolvedMask{ResolvedReference{EntityHandle()}, std::nullopt,
                                                  MaskContentUnits::Default};

  ComplexityBucketer bucketer(ComplexityBucketerConfig{.targetBucketCount = 2});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsPublished, 1u);
  EXPECT_EQ(countHints(child, HintSource::ComplexityBucket), 1u)
      << "subtree with internal mask is bucketed atomically";
}

TEST_F(ComplexityBucketerTest, SubtreeWithInternalIsolatedLayerStaysIntactInOneBucket) {
  // A child with `isolatedLayer = true` (opacity<1 / blend-mode / isolation)
  // is still a valid bucket candidate. The isolation boundary is the subtree
  // root, so the whole subtree ends up in one bucket. No splitting.
  const Entity root = makeInstance();
  const Entity child = makeInstance();
  setSubtreeEnd(root, child);

  auto& childInstance = registry_.get<RenderingInstanceComponent>(child);
  childInstance.isolatedLayer = true;

  ComplexityBucketer bucketer(ComplexityBucketerConfig{.targetBucketCount = 2});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsPublished, 1u);
  EXPECT_EQ(countHints(child, HintSource::ComplexityBucket), 1u);
}

TEST_F(ComplexityBucketerTest, MultipleTopLevelChildrenEachBucketedAtomically) {
  // Three top-level children, each with different compositing features.
  // They all become separate buckets; none is split.
  const Entity root = makeInstance();
  const Entity filtered = makeInstance();
  const Entity masked = makeInstance();
  const Entity isolated = makeInstance();
  setSubtreeEnd(root, isolated);

  auto& filteredInstance = registry_.get<RenderingInstanceComponent>(filtered);
  filteredInstance.resolvedFilter = components::ResolvedFilterEffect{std::vector<FilterEffect>{}};

  auto& maskedInstance = registry_.get<RenderingInstanceComponent>(masked);
  maskedInstance.mask = components::ResolvedMask{ResolvedReference{EntityHandle()}, std::nullopt,
                                                   MaskContentUnits::Default};

  auto& isolatedInstance = registry_.get<RenderingInstanceComponent>(isolated);
  isolatedInstance.isolatedLayer = true;

  ComplexityBucketer bucketer(ComplexityBucketerConfig{.targetBucketCount = 4});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().bucketsPublished, 3u);
  EXPECT_EQ(countHints(filtered, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(masked, HintSource::ComplexityBucket), 1u);
  EXPECT_EQ(countHints(isolated, HintSource::ComplexityBucket), 1u);
}

TEST_F(ComplexityBucketerTest, BudgetZeroProducesNothing) {
  // K=1, reserved=1 → budget=0. No buckets published even with candidates.
  const Entity root = makeInstance();
  const Entity c1 = makeInstance();
  const Entity c2 = makeInstance();
  setSubtreeEnd(root, c2);
  static_cast<void>(c1);

  ComplexityBucketer bucketer(
      ComplexityBucketerConfig{.targetBucketCount = 1, .reservedSlots = 1});
  bucketer.reconcile(registry_);

  EXPECT_EQ(bucketer.stats().candidatesConsidered, 2u) << "candidates still evaluated";
  EXPECT_EQ(bucketer.stats().bucketsPublished, 0u) << "but zero published under zero budget";
}

}  // namespace donner::svg::compositor
