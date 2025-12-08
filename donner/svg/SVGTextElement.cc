#include "donner/svg/SVGTextElement.h"

#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/text/TextFlowComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"

namespace donner::svg {

SVGTextElement SVGTextElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  handle.emplace<components::TextFlowComponent>();
  handle.emplace<components::TextRootComponent>();

  return SVGTextElement(handle);
}

}  // namespace donner::svg
