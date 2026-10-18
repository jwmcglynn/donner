#include "donner/svg/SVGMPathElement.h"

#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"

namespace donner::svg {

SVGMPathElement SVGMPathElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::MPathComponent>();
  return SVGMPathElement(handle);
}

}  // namespace donner::svg
