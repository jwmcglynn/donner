#include "donner/svg/SVGTextElement.h"

#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {

SVGTextElement SVGTextElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);

  return SVGTextElement(handle);
}

}  // namespace donner::svg
