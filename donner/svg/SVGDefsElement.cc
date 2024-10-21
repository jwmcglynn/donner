#include "donner/svg/SVGDefsElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {

SVGDefsElement SVGDefsElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  return SVGDefsElement(handle);
}

}  // namespace donner::svg
