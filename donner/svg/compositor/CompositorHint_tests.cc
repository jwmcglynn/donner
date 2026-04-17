#include "donner/svg/compositor/ScopedCompositorHint.h"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/CompositorHintComponent.h"

namespace donner::svg::compositor {

namespace {

class CompositorHintTest : public ::testing::Test {
protected:
  Registry registry_;
};

}  // namespace

TEST_F(CompositorHintTest, ConstructingScopedHintAttachesComponent) {
  const Entity entity = registry_.create();
  {
    ScopedCompositorHint hint = ScopedCompositorHint::Explicit(registry_, entity, 0x2000);
    ASSERT_TRUE(registry_.all_of<CompositorHintComponent>(entity))
        << "component should appear once a scoped hint is constructed";
    const auto& component = registry_.get<CompositorHintComponent>(entity);
    ASSERT_EQ(component.entries.size(), 1u) << "exactly one entry expected";
    EXPECT_EQ(component.entries[0].source, HintSource::Explicit);
    EXPECT_EQ(component.entries[0].weight, 0x2000);
  }
  EXPECT_FALSE(registry_.all_of<CompositorHintComponent>(entity))
      << "destructor should remove the component when the last entry drops";
}

TEST_F(CompositorHintTest, MandatoryFactoryUsesInfiniteSentinelWeight) {
  const Entity entity = registry_.create();
  ScopedCompositorHint hint = ScopedCompositorHint::Mandatory(registry_, entity);
  const auto& component = registry_.get<CompositorHintComponent>(entity);
  ASSERT_EQ(component.entries.size(), 1u);
  EXPECT_EQ(component.entries[0].source, HintSource::Mandatory);
  EXPECT_EQ(component.entries[0].weight, 0xFFFF);
  EXPECT_EQ(component.totalWeight(), std::numeric_limits<uint32_t>::max())
      << "Mandatory hint must short-circuit totalWeight to UINT32_MAX";
}

TEST_F(CompositorHintTest, InteractionFactoryDefaultsToMediumWeight) {
  const Entity entity = registry_.create();
  ScopedCompositorHint hint =
      ScopedCompositorHint::Interaction(registry_, entity, InteractionHint::Selection);
  const auto& component = registry_.get<CompositorHintComponent>(entity);
  ASSERT_EQ(component.entries.size(), 1u);
  EXPECT_EQ(component.entries[0].source, HintSource::Interaction);
  EXPECT_EQ(component.entries[0].weight, 0x8000)
      << "Interaction default weight matches the design-doc Medium slot";
  ASSERT_TRUE(hint.interactionKind().has_value());
  EXPECT_EQ(*hint.interactionKind(), InteractionHint::Selection);
}

TEST_F(CompositorHintTest, InteractionFactoryPreservesActiveDragKind) {
  const Entity entity = registry_.create();
  ScopedCompositorHint hint =
      ScopedCompositorHint::Interaction(registry_, entity, InteractionHint::ActiveDrag);
  ASSERT_TRUE(hint.interactionKind().has_value());
  EXPECT_EQ(*hint.interactionKind(), InteractionHint::ActiveDrag);
}

TEST_F(CompositorHintTest, NonInteractionHintsReportNoInteractionKind) {
  const Entity entity = registry_.create();
  ScopedCompositorHint mandatory = ScopedCompositorHint::Mandatory(registry_, entity);
  EXPECT_FALSE(mandatory.interactionKind().has_value())
      << "Mandatory hint must not report an InteractionHint kind";

  const Entity entity2 = registry_.create();
  ScopedCompositorHint animation = ScopedCompositorHint::Animation(registry_, entity2);
  EXPECT_FALSE(animation.interactionKind().has_value())
      << "Animation hint must not report an InteractionHint kind";
}

TEST_F(CompositorHintTest, AnimationFactoryDefaultsToHighWeight) {
  const Entity entity = registry_.create();
  ScopedCompositorHint hint = ScopedCompositorHint::Animation(registry_, entity);
  const auto& component = registry_.get<CompositorHintComponent>(entity);
  ASSERT_EQ(component.entries.size(), 1u);
  EXPECT_EQ(component.entries[0].source, HintSource::Animation);
  EXPECT_EQ(component.entries[0].weight, 0xC000)
      << "Animation default weight matches the design-doc High slot (above Interaction)";
}

TEST_F(CompositorHintTest, InteractionKindSurvivesMove) {
  const Entity entity = registry_.create();
  ScopedCompositorHint original =
      ScopedCompositorHint::Interaction(registry_, entity, InteractionHint::ActiveDrag);
  ScopedCompositorHint moved = std::move(original);
  ASSERT_TRUE(moved.interactionKind().has_value());
  EXPECT_EQ(*moved.interactionKind(), InteractionHint::ActiveDrag)
      << "move must preserve the InteractionHint kind on the destination handle";
  EXPECT_FALSE(original.active()) << "moved-from handle should be inert";
}

TEST_F(CompositorHintTest, MultipleScopedHintsAccumulate) {
  const Entity entity = registry_.create();
  ScopedCompositorHint a = ScopedCompositorHint::Explicit(registry_, entity, 0x1000);
  ScopedCompositorHint b = ScopedCompositorHint::Explicit(registry_, entity, 0x2000);
  ScopedCompositorHint c = ScopedCompositorHint::Mandatory(registry_, entity);

  const auto& component = registry_.get<CompositorHintComponent>(entity);
  EXPECT_EQ(component.entries.size(), 3u) << "all three entries should coexist";
  EXPECT_EQ(component.totalWeight(), std::numeric_limits<uint32_t>::max())
      << "mandatory among entries should still short-circuit to UINT32_MAX";
}

TEST_F(CompositorHintTest, ScopedHintsRemoveIndependently) {
  const Entity entity = registry_.create();
  std::optional<ScopedCompositorHint> a(ScopedCompositorHint::Explicit(registry_, entity, 0x1000));
  std::optional<ScopedCompositorHint> b(ScopedCompositorHint::Explicit(registry_, entity, 0x2000));

  ASSERT_EQ(registry_.get<CompositorHintComponent>(entity).entries.size(), 2u);

  a.reset();
  ASSERT_TRUE(registry_.all_of<CompositorHintComponent>(entity))
      << "one live hint should keep the component attached";
  const auto& after = registry_.get<CompositorHintComponent>(entity);
  ASSERT_EQ(after.entries.size(), 1u);
  EXPECT_EQ(after.entries[0].weight, 0x2000) << "the 0x1000 hint should be the one removed";

  b.reset();
  EXPECT_FALSE(registry_.all_of<CompositorHintComponent>(entity))
      << "dropping the last scoped hint must remove the component entirely";
}

TEST_F(CompositorHintTest, MoveConstructTransfersOwnership) {
  const Entity entity = registry_.create();
  std::optional<ScopedCompositorHint> src(
      ScopedCompositorHint::Explicit(registry_, entity, 0x4000));
  ASSERT_EQ(registry_.get<CompositorHintComponent>(entity).entries.size(), 1u);

  ScopedCompositorHint dst = std::move(*src);
  EXPECT_FALSE(src->active()) << "moved-from handle must be inert";

  src.reset();
  EXPECT_TRUE(registry_.all_of<CompositorHintComponent>(entity))
      << "destroying moved-from handle should NOT remove the hint entry";
  EXPECT_EQ(registry_.get<CompositorHintComponent>(entity).entries.size(), 1u);

  {
    ScopedCompositorHint local = std::move(dst);
    EXPECT_FALSE(dst.active()) << "second moved-from handle must also be inert";
    EXPECT_TRUE(registry_.all_of<CompositorHintComponent>(entity))
        << "moving the handle must not drop the entry";
  }

  EXPECT_FALSE(registry_.all_of<CompositorHintComponent>(entity))
      << "only the final moved-to handle's destructor removes the entry";
}

TEST_F(CompositorHintTest, MoveAssignmentReleasesPriorEntry) {
  const Entity first = registry_.create();
  const Entity second = registry_.create();
  ScopedCompositorHint a = ScopedCompositorHint::Explicit(registry_, first, 0x1000);
  ScopedCompositorHint b = ScopedCompositorHint::Explicit(registry_, second, 0x2000);
  ASSERT_TRUE(registry_.all_of<CompositorHintComponent>(first));
  ASSERT_TRUE(registry_.all_of<CompositorHintComponent>(second));

  a = std::move(b);

  EXPECT_FALSE(registry_.all_of<CompositorHintComponent>(first))
      << "move-assign should release the pre-existing entry on the destination";
  ASSERT_TRUE(registry_.all_of<CompositorHintComponent>(second))
      << "the moved-in entry should remain on the new target";
  EXPECT_EQ(registry_.get<CompositorHintComponent>(second).entries.size(), 1u);
}

TEST_F(CompositorHintTest, RemoveFirstMatchingRemovesExactlyOneDuplicate) {
  CompositorHintComponent component;
  component.addHint(HintSource::Explicit, 0x4000);
  component.addHint(HintSource::Explicit, 0x4000);
  component.addHint(HintSource::Explicit, 0x4000);

  EXPECT_TRUE(component.removeFirstMatching(HintSource::Explicit, 0x4000));
  EXPECT_EQ(component.entries.size(), 2u) << "only one duplicate should be removed";

  EXPECT_FALSE(component.removeFirstMatching(HintSource::Explicit, 0x9999))
      << "unmatched remove returns false and does not alter storage";
  EXPECT_EQ(component.entries.size(), 2u);
}

TEST_F(CompositorHintTest, TotalWeightSumsNonMandatoryEntries) {
  CompositorHintComponent component;
  component.addHint(HintSource::Explicit, 0x1000);
  component.addHint(HintSource::Animation, 0x0200);
  component.addHint(HintSource::Interaction, 0x0040);

  EXPECT_EQ(component.totalWeight(), 0x1000u + 0x0200u + 0x0040u);

  component.addHint(HintSource::Mandatory, 0xFFFF);
  EXPECT_EQ(component.totalWeight(), std::numeric_limits<uint32_t>::max())
      << "any mandatory entry must short-circuit the weight";
}

}  // namespace donner::svg::compositor
