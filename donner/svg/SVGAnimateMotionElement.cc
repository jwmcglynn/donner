#include "donner/svg/SVGAnimateMotionElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"

namespace donner::svg {

SVGAnimateMotionElement SVGAnimateMotionElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::AnimationTimingComponent>();
  handle.emplace<components::AnimateMotionComponent>();
  return SVGAnimateMotionElement(handle);
}

}  // namespace donner::svg
