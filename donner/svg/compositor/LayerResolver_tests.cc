#include "donner/svg/compositor/LayerResolver.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/CompositorHintComponent.h"
#include "donner/svg/compositor/ComputedLayerAssignmentComponent.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"

namespace donner::svg::compositor {

namespace {

constexpr uint32_t kDefaultBudget = 32;

class LayerResolverTest : public ::testing::Test {
protected:
  Registry registry_;
  LayerResolver resolver_;

  uint32_t layerIdOf(Entity entity) const {
    const auto* comp = registry_.try_get<ComputedLayerAssignmentComponent>(entity);
    return comp == nullptr ? 0u : comp->layerId;
  }

  bool hasAssignment(Entity entity) const {
    return registry_.all_of<ComputedLayerAssignmentComponent>(entity);
  }
};

}  // namespace

TEST_F(LayerResolverTest, NoHintsProducesNoAssignments) {
  const Entity a = registry_.create();
  const Entity b = registry_.create();
  (void)a;
  (void)b;

  resolver_.resolve(registry_, kDefaultBudget);

  EXPECT_EQ(resolver_.stats().candidatesEvaluated, 0u);
  EXPECT_EQ(resolver_.stats().layersAssigned, 0u);
  EXPECT_EQ(resolver_.stats().budgetExhaustions, 0u);
  EXPECT_FALSE(hasAssignment(a));
  EXPECT_FALSE(hasAssignment(b));
}

TEST_F(LayerResolverTest, SingleMandatoryHintGetsLayer1) {
  const Entity entity = registry_.create();
  ScopedCompositorHint hint = ScopedCompositorHint::Mandatory(registry_, entity);

  resolver_.resolve(registry_, kDefaultBudget);

  EXPECT_EQ(resolver_.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(resolver_.stats().layersAssigned, 1u);
  EXPECT_EQ(resolver_.stats().budgetExhaustions, 0u);
  EXPECT_EQ(layerIdOf(entity), 1u) << "single mandatory should win layer 1";
}

TEST_F(LayerResolverTest, SingleExplicitHintGetsLayer1) {
  const Entity entity = registry_.create();
  ScopedCompositorHint hint = ScopedCompositorHint::Explicit(registry_, entity, 0x4000);

  resolver_.resolve(registry_, kDefaultBudget);

  EXPECT_EQ(resolver_.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(resolver_.stats().layersAssigned, 1u);
  EXPECT_EQ(layerIdOf(entity), 1u) << "single explicit hint should win layer 1";
}

TEST_F(LayerResolverTest, MandatoryAndExplicitOnSameEntityProducesOneAssignment) {
  const Entity entity = registry_.create();
  ScopedCompositorHint mandatory = ScopedCompositorHint::Mandatory(registry_, entity);
  ScopedCompositorHint explicit_ = ScopedCompositorHint::Explicit(registry_, entity, 0x2000);

  resolver_.resolve(registry_, kDefaultBudget);

  EXPECT_EQ(resolver_.stats().candidatesEvaluated, 1u)
      << "same entity with multiple hints is still one candidate";
  EXPECT_EQ(resolver_.stats().layersAssigned, 1u);
  EXPECT_EQ(layerIdOf(entity), 1u);
}

TEST_F(LayerResolverTest, ExplicitHintsRankedByWeightDescending) {
  const Entity low = registry_.create();
  const Entity high = registry_.create();

  ScopedCompositorHint lowHint = ScopedCompositorHint::Explicit(registry_, low, 0x0100);
  ScopedCompositorHint highHint = ScopedCompositorHint::Explicit(registry_, high, 0x8000);

  resolver_.resolve(registry_, kDefaultBudget);

  EXPECT_EQ(layerIdOf(high), 1u) << "higher weight must win layer 1";
  EXPECT_EQ(layerIdOf(low), 2u) << "lower weight takes the next slot when budget permits";
  EXPECT_EQ(resolver_.stats().layersAssigned, 2u);
}

TEST_F(LayerResolverTest, TieInWeightsBrokenByLowerEntityId) {
  // Create several entities and tie their weights.
  std::vector<Entity> entities;
  for (int i = 0; i < 4; ++i) {
    entities.push_back(registry_.create());
  }
  std::vector<ScopedCompositorHint> hints;
  hints.reserve(entities.size());
  for (Entity e : entities) {
    hints.push_back(ScopedCompositorHint::Explicit(registry_, e, 0x4000));
  }

  resolver_.resolve(registry_, kDefaultBudget);

  // Lower entity id should get layer 1, etc.
  std::vector<Entity> sorted = entities;
  std::sort(sorted.begin(), sorted.end(), [](Entity a, Entity b) {
    return static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b);
  });
  for (size_t i = 0; i < sorted.size(); ++i) {
    EXPECT_EQ(layerIdOf(sorted[i]), static_cast<uint32_t>(i + 1u))
        << "tie-break must be by ascending entity id; mismatch at index " << i;
  }
}

TEST_F(LayerResolverTest, MandatoryAlwaysWinsUnderBudgetPressure) {
  constexpr uint32_t kBudget = 2;

  std::vector<Entity> mandatoryEntities;
  std::vector<ScopedCompositorHint> mandatoryHints;
  for (int i = 0; i < 3; ++i) {
    Entity e = registry_.create();
    mandatoryEntities.push_back(e);
    mandatoryHints.push_back(ScopedCompositorHint::Mandatory(registry_, e));
  }
  std::vector<Entity> explicitEntities;
  std::vector<ScopedCompositorHint> explicitHints;
  for (int i = 0; i < 3; ++i) {
    Entity e = registry_.create();
    explicitEntities.push_back(e);
    explicitHints.push_back(ScopedCompositorHint::Explicit(registry_, e, 0x4000));
  }

  resolver_.resolve(registry_, kBudget);

  // Budget is 2. First two mandatory get layers (by entity id). Third mandatory
  // is evicted and logs; all explicit candidates are evicted.
  EXPECT_EQ(layerIdOf(mandatoryEntities[0]), 1u);
  EXPECT_EQ(layerIdOf(mandatoryEntities[1]), 2u);
  EXPECT_FALSE(hasAssignment(mandatoryEntities[2]))
      << "over-budget mandatory should have no assignment";
  for (Entity e : explicitEntities) {
    EXPECT_FALSE(hasAssignment(e)) << "explicit candidate " << e
                                   << " should lose to mandatory under budget pressure";
  }

  EXPECT_EQ(resolver_.stats().candidatesEvaluated, 6u);
  EXPECT_EQ(resolver_.stats().layersAssigned, 2u);
  // 1 over-budget mandatory + 3 non-mandatory denials.
  EXPECT_EQ(resolver_.stats().budgetExhaustions, 4u)
      << "budgetExhaustions should count both the over-budget mandatory eviction and the "
         "non-mandatory denials";
}

TEST_F(LayerResolverTest, DeterministicAcrossRepeatedCalls) {
  std::vector<Entity> entities;
  std::vector<ScopedCompositorHint> hints;
  for (int i = 0; i < 6; ++i) {
    Entity e = registry_.create();
    entities.push_back(e);
    // Alternating weights.
    hints.push_back(ScopedCompositorHint::Explicit(registry_, e,
                                                   static_cast<uint16_t>(0x1000 + i * 0x0400)));
  }

  resolver_.resolve(registry_, kDefaultBudget);

  std::vector<uint32_t> firstAssignment;
  firstAssignment.reserve(entities.size());
  for (Entity e : entities) {
    firstAssignment.push_back(layerIdOf(e));
  }

  for (int run = 0; run < 10; ++run) {
    resolver_.resolve(registry_, kDefaultBudget);
    for (size_t i = 0; i < entities.size(); ++i) {
      EXPECT_EQ(layerIdOf(entities[i]), firstAssignment[i])
          << "assignment diverged on run " << run << " at index " << i;
    }
  }
}

TEST_F(LayerResolverTest, ReResolveAfterHintDropClearsAssignment) {
  const Entity a = registry_.create();
  const Entity b = registry_.create();
  std::optional<ScopedCompositorHint> hintA(
      ScopedCompositorHint::Explicit(registry_, a, 0x4000));
  std::optional<ScopedCompositorHint> hintB(
      ScopedCompositorHint::Explicit(registry_, b, 0x2000));

  resolver_.resolve(registry_, kDefaultBudget);
  ASSERT_EQ(layerIdOf(a), 1u);
  ASSERT_EQ(layerIdOf(b), 2u);

  // Drop the higher-weight hint; only b remains.
  hintA.reset();

  resolver_.resolve(registry_, kDefaultBudget);

  EXPECT_FALSE(hasAssignment(a)) << "dropped hint must clear its ComputedLayerAssignmentComponent";
  EXPECT_EQ(layerIdOf(b), 1u) << "remaining entity should be reassigned to layer 1";
  EXPECT_EQ(resolver_.stats().candidatesEvaluated, 1u);
  EXPECT_EQ(resolver_.stats().layersAssigned, 1u);
}

TEST_F(LayerResolverTest, WriteOnlyOnDiffAvoidsChurn) {
  const Entity e = registry_.create();
  ScopedCompositorHint hint = ScopedCompositorHint::Explicit(registry_, e, 0x4000);

  resolver_.resolve(registry_, kDefaultBudget);
  ASSERT_TRUE(hasAssignment(e));
  const auto* before = registry_.try_get<ComputedLayerAssignmentComponent>(e);
  const uint32_t layerBefore = before->layerId;

  resolver_.resolve(registry_, kDefaultBudget);
  const auto* after = registry_.try_get<ComputedLayerAssignmentComponent>(e);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after->layerId, layerBefore)
      << "stable inputs should yield the same layer id without churn";
}

TEST_F(LayerResolverTest, BudgetCapWithoutMandatoryEvictsLowestWeight) {
  constexpr uint32_t kBudget = 2;

  const Entity low = registry_.create();
  const Entity mid = registry_.create();
  const Entity high = registry_.create();
  ScopedCompositorHint lowHint = ScopedCompositorHint::Explicit(registry_, low, 0x0100);
  ScopedCompositorHint midHint = ScopedCompositorHint::Explicit(registry_, mid, 0x4000);
  ScopedCompositorHint highHint = ScopedCompositorHint::Explicit(registry_, high, 0x8000);

  resolver_.resolve(registry_, kBudget);

  EXPECT_EQ(layerIdOf(high), 1u);
  EXPECT_EQ(layerIdOf(mid), 2u);
  EXPECT_FALSE(hasAssignment(low)) << "lowest-weight candidate must lose to the budget cap";
  EXPECT_EQ(resolver_.stats().budgetExhaustions, 1u);
}

}  // namespace donner::svg::compositor
