#include "donner/svg/SVGSetElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"

namespace donner::svg {

SVGSetElement SVGSetElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::AnimationTimingComponent>();
  handle.emplace<components::SetAnimationComponent>();
  return SVGSetElement(handle);
}

}  // namespace donner::svg
