#include "donner/svg/SVGAnimateElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"

namespace donner::svg {

SVGAnimateElement SVGAnimateElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::AnimationTimingComponent>();
  handle.emplace<components::AnimateValueComponent>();
  return SVGAnimateElement(handle);
}

}  // namespace donner::svg
