#include "src/svg/svg_defs_element.h"

#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

namespace donner {

SVGDefsElement SVGDefsElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  Entity entity = CreateEntity(registry, RcString(Tag), Type);
  registry.emplace<RenderingBehaviorComponent>(entity, true);
  return SVGDefsElement(registry, entity);
}

}  // namespace donner
