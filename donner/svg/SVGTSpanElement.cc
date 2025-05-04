#include "donner/svg/SVGTSpanElement.h"

#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {

SVGTSpanElement SVGTSpanElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);

  return SVGTSpanElement(handle);
}

}  // namespace donner::svg
