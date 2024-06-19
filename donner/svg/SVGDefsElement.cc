#include "donner/svg/SVGDefsElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {

SVGDefsElement SVGDefsElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  return SVGDefsElement(handle);
}

}  // namespace donner::svg
