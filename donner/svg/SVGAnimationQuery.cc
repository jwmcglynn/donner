#include "donner/svg/SVGAnimationQuery.h"

#include "donner/svg/SVGElement.h"
#include "donner/svg/components/animation/AnimationSystem.h"

namespace donner::svg {

bool IsAnimationTarget(const SVGElement& element) {
  return element.withReadAccess([](DocumentReadAccess&, EntityHandle handle) {
    return handle &&
           components::AnimationSystem().isAnimationTarget(*handle.registry(), handle.entity());
  });
}

}  // namespace donner::svg
