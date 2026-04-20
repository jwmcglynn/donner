#include <gtest/gtest.h>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/CompositorHintComponent.h"
#include "donner/svg/compositor/ComputedLayerAssignmentComponent.h"
#include "donner/svg/compositor/LayerResolver.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"

namespace donner::svg::compositor {

namespace {

class AnimationIsolationTest : public ::testing::Test {
protected:
  Registry registry_;
  LayerResolver resolver_;

  uint32_t layerIdOf(Entity entity) const {
    const auto* assignment = registry_.try_get<ComputedLayerAssignmentComponent>(entity);
    return assignment != nullptr ? assignment->layerId : 0;
  }
};

}  // namespace

// Proves that a future animation system can publish `Animation` hints through
// `ScopedCompositorHint::Animation` and the resolver will promote the
// animated entity to its own layer — the Phase 2 "animation unblocked" gate.
// The animation system itself doesn't exist yet; this exercises the seam.
TEST_F(AnimationIsolationTest, AnimationHintPromotesEntityToLayer) {
  const Entity animated = registry_.create();

  ScopedCompositorHint hint = ScopedCompositorHint::Animation(registry_, animated);
  resolver_.resolve(registry_, /*maxLayers=*/8);

  EXPECT_NE(layerIdOf(animated), 0u) << "Animation hint should promote entity to a non-root layer";
}

// An `Animation` hint outweighs an `Interaction` hint: animation is High
// weight, Interaction is Medium. Under budget pressure, the animated entity
// wins.
TEST_F(AnimationIsolationTest, AnimationOutweighsInteractionUnderBudget) {
  const Entity animated = registry_.create();
  const Entity interactive = registry_.create();

  ScopedCompositorHint animHint = ScopedCompositorHint::Animation(registry_, animated);
  ScopedCompositorHint interHint =
      ScopedCompositorHint::Interaction(registry_, interactive, InteractionHint::ActiveDrag);

  // Budget = 1 so exactly one of the two candidates can win.
  resolver_.resolve(registry_, /*maxLayers=*/1);

  EXPECT_NE(layerIdOf(animated), 0u) << "Animation (High weight) wins under budget pressure";
  EXPECT_EQ(layerIdOf(interactive), 0u)
      << "Interaction (Medium weight) loses to Animation when only one slot is available";
}

// When the `autoPromoteAnimations` gate is off in `ResolveOptions`, Animation
// hints produce no layer. This is the runtime kill-switch the design doc calls
// out in Goal 7 / § Reversibility — animations fall back to whole-document
// re-rendering without ABI fragmentation.
TEST_F(AnimationIsolationTest, AnimationGateDisablesPromotion) {
  const Entity animated = registry_.create();

  ScopedCompositorHint hint = ScopedCompositorHint::Animation(registry_, animated);

  ResolveOptions options;
  options.enableAnimationHints = false;
  resolver_.resolve(registry_, /*maxLayers=*/8, options);

  EXPECT_EQ(layerIdOf(animated), 0u)
      << "Animation hint should be ignored when the gate is off";
}

// An entity with both a Mandatory hint AND an Animation hint still gets a
// layer even when the Animation gate is off — Mandatory hints represent SVG
// semantics (opacity < 1, filter, mask) and are always honored.
TEST_F(AnimationIsolationTest, MandatoryHintOverridesAnimationGate) {
  const Entity e = registry_.create();

  ScopedCompositorHint mandatory = ScopedCompositorHint::Mandatory(registry_, e);
  ScopedCompositorHint animation = ScopedCompositorHint::Animation(registry_, e);

  ResolveOptions options;
  options.enableAnimationHints = false;
  resolver_.resolve(registry_, /*maxLayers=*/8, options);

  EXPECT_NE(layerIdOf(e), 0u)
      << "Mandatory hint promotes the entity regardless of the Animation gate";
}

// Toggling the gate on a live registry drops the assignment when the gate
// flips off and re-publishes it when the gate flips back on. The RAII hint
// handle itself persists — only the resolver's output changes.
TEST_F(AnimationIsolationTest, GateFlipDemotesAndRepromotes) {
  const Entity animated = registry_.create();
  ScopedCompositorHint hint = ScopedCompositorHint::Animation(registry_, animated);

  resolver_.resolve(registry_, /*maxLayers=*/8);
  ASSERT_NE(layerIdOf(animated), 0u);

  ResolveOptions off;
  off.enableAnimationHints = false;
  resolver_.resolve(registry_, /*maxLayers=*/8, off);
  EXPECT_EQ(layerIdOf(animated), 0u) << "gate flip off demotes";

  resolver_.resolve(registry_, /*maxLayers=*/8);
  EXPECT_NE(layerIdOf(animated), 0u) << "gate flip back on re-promotes the still-live hint";
}

}  // namespace donner::svg::compositor
