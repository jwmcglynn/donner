#include "donner/svg/SVGAnimateTransformElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"

namespace donner::svg {

SVGAnimateTransformElement SVGAnimateTransformElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::AnimationTimingComponent>();
  handle.emplace<components::AnimateTransformComponent>();
  return SVGAnimateTransformElement(handle);
}

}  // namespace donner::svg
