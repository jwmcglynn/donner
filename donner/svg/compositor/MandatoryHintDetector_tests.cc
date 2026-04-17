#include "donner/svg/compositor/MandatoryHintDetector.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/compositor/CompositorHintComponent.h"

namespace donner::svg::compositor {

namespace {

using components::RenderingInstanceComponent;

/// Returns the single `Mandatory` entry on the entity's `CompositorHintComponent`
/// if exactly one exists. `nullptr` otherwise (test helper).
const HintEntry* getSingleMandatoryEntry(const Registry& registry, Entity entity) {
  const auto* component = registry.try_get<CompositorHintComponent>(entity);
  if (component == nullptr) {
    return nullptr;
  }
  if (component->entries.size() != 1) {
    return nullptr;
  }
  if (component->entries[0].source != HintSource::Mandatory) {
    return nullptr;
  }
  return &component->entries[0];
}

}  // namespace

TEST(MandatoryHintDetectorTest, EmptyRegistryProducesNoHints) {
  Registry registry;
  MandatoryHintDetector detector;

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 0u);
  EXPECT_EQ(detector.stats().hintsPublished, 0u);
  EXPECT_EQ(detector.stats().hintsDropped, 0u);
  EXPECT_EQ(detector.stats().hintsActive, 0u);
}

TEST(MandatoryHintDetectorTest, NonQualifyingEntityGetsNoHint) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  registry.emplace<RenderingInstanceComponent>(entity);  // all defaults — nothing qualifies.

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(detector.stats().hintsPublished, 0u) << "no qualifying signal, no hint";
  EXPECT_EQ(detector.stats().hintsDropped, 0u);
  EXPECT_EQ(detector.stats().hintsActive, 0u);
  EXPECT_FALSE(registry.all_of<CompositorHintComponent>(entity))
      << "non-qualifying entity must not receive a CompositorHintComponent";
}

TEST(MandatoryHintDetectorTest, IsolatedLayerQualifies) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  instance.isolatedLayer = true;

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(detector.stats().hintsPublished, 1u);
  EXPECT_EQ(detector.stats().hintsDropped, 0u);
  EXPECT_EQ(detector.stats().hintsActive, 1u);

  const HintEntry* entry = getSingleMandatoryEntry(registry, entity);
  ASSERT_NE(entry, nullptr) << "entity should have exactly one Mandatory hint entry";
  EXPECT_EQ(entry->source, HintSource::Mandatory);
  EXPECT_EQ(entry->weight, 0xFFFF) << "Mandatory hints use the infinite-weight sentinel";
}

TEST(MandatoryHintDetectorTest, ResolvedFilterQualifies) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  // A default-constructed empty vector still makes the optional engaged,
  // which is what the detector checks.
  instance.resolvedFilter.emplace(std::vector<FilterEffect>{});

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(detector.stats().hintsPublished, 1u);
  EXPECT_EQ(detector.stats().hintsActive, 1u);

  const HintEntry* entry = getSingleMandatoryEntry(registry, entity);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->source, HintSource::Mandatory);
  EXPECT_EQ(entry->weight, 0xFFFF);
}

TEST(MandatoryHintDetectorTest, MaskQualifies) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  instance.mask = components::ResolvedMask{ResolvedReference{EntityHandle()}, std::nullopt,
                                           MaskContentUnits::Default};

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(detector.stats().hintsPublished, 1u);
  EXPECT_EQ(detector.stats().hintsActive, 1u);

  const HintEntry* entry = getSingleMandatoryEntry(registry, entity);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->source, HintSource::Mandatory);
  EXPECT_EQ(entry->weight, 0xFFFF);
}

TEST(MandatoryHintDetectorTest, ReconcileIsIdempotent) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  instance.isolatedLayer = true;

  detector.reconcile(registry);
  ASSERT_EQ(detector.stats().hintsPublished, 1u);
  ASSERT_EQ(detector.stats().hintsActive, 1u);
  ASSERT_EQ(registry.get<CompositorHintComponent>(entity).entries.size(), 1u);

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(detector.stats().hintsPublished, 0u) << "no churn on unchanged registry";
  EXPECT_EQ(detector.stats().hintsDropped, 0u);
  EXPECT_EQ(detector.stats().hintsActive, 1u);

  EXPECT_EQ(registry.get<CompositorHintComponent>(entity).entries.size(), 1u)
      << "idempotent reconcile must not duplicate hint entries";
}

TEST(MandatoryHintDetectorTest, DemoteWhenSignalClears) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  instance.isolatedLayer = true;

  detector.reconcile(registry);
  ASSERT_TRUE(registry.all_of<CompositorHintComponent>(entity));
  ASSERT_EQ(detector.stats().hintsActive, 1u);

  // Clear the signal and reconcile again.
  registry.get<RenderingInstanceComponent>(entity).isolatedLayer = false;
  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(detector.stats().hintsPublished, 0u);
  EXPECT_EQ(detector.stats().hintsDropped, 1u);
  EXPECT_EQ(detector.stats().hintsActive, 0u);
  EXPECT_FALSE(registry.all_of<CompositorHintComponent>(entity))
      << "demoted entity should have its CompositorHintComponent removed entirely";
}

TEST(MandatoryHintDetectorTest, MoveConstructPreservesHint) {
  Registry registry;
  MandatoryHintDetector source;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  instance.isolatedLayer = true;

  source.reconcile(registry);
  ASSERT_TRUE(registry.all_of<CompositorHintComponent>(entity));
  ASSERT_EQ(registry.get<CompositorHintComponent>(entity).entries.size(), 1u);

  MandatoryHintDetector moved(std::move(source));

  EXPECT_EQ(moved.stats().hintsActive, 1u)
      << "stats should be carried across the move (same snapshot)";

  // The hint entry must remain attached to the entity.
  EXPECT_TRUE(registry.all_of<CompositorHintComponent>(entity))
      << "the hint should survive the move";
  EXPECT_EQ(registry.get<CompositorHintComponent>(entity).entries.size(), 1u)
      << "exactly one hint entry still present after move";

  // A reconcile on the moved-from detector should see an empty internal map,
  // re-publish the hint from scratch — but because the moved-to detector still
  // holds the active ScopedCompositorHint, publishing from the moved-from
  // detector would add a second entry. The real invariant we care about is
  // that the moved-from detector has an empty hints_ map. We can verify that
  // by running reconcile against a registry with no qualifying entities: no
  // drops should occur.
  Registry emptyRegistry;
  source.reconcile(emptyRegistry);  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(source.stats().hintsDropped, 0u)  // NOLINT(bugprone-use-after-move)
      << "moved-from detector's hints_ map must be empty — no hints to drop";
  EXPECT_EQ(source.stats().hintsActive, 0u);  // NOLINT(bugprone-use-after-move)
}

TEST(MandatoryHintDetectorTest, EntityDestroyedBetweenReconciles) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  instance.isolatedLayer = true;

  detector.reconcile(registry);
  ASSERT_EQ(detector.stats().hintsActive, 1u);

  registry.destroy(entity);

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 0u)
      << "destroyed entity is no longer in the RenderingInstanceComponent view";
  EXPECT_EQ(detector.stats().hintsPublished, 0u);
  EXPECT_EQ(detector.stats().hintsDropped, 1u);
  EXPECT_EQ(detector.stats().hintsActive, 0u);
}

TEST(MandatoryHintDetectorTest, MultipleSignalsOnSameEntityProduceOneHint) {
  Registry registry;
  MandatoryHintDetector detector;

  const Entity entity = registry.create();
  auto& instance = registry.emplace<RenderingInstanceComponent>(entity);
  instance.isolatedLayer = true;
  instance.resolvedFilter.emplace(std::vector<FilterEffect>{});

  detector.reconcile(registry);

  EXPECT_EQ(detector.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(detector.stats().hintsPublished, 1u) << "one entity, one hint regardless of signal count";
  EXPECT_EQ(detector.stats().hintsActive, 1u);

  const auto& component = registry.get<CompositorHintComponent>(entity);
  ASSERT_EQ(component.entries.size(), 1u)
      << "multiple qualifying signals must still produce only a single Mandatory entry";
  EXPECT_EQ(component.entries[0].source, HintSource::Mandatory);
  EXPECT_EQ(component.entries[0].weight, 0xFFFF);
}

}  // namespace donner::svg::compositor
