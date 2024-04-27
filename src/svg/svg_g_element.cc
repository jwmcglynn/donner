#include "src/svg/svg_g_element.h"

#include "src/svg/svg_document.h"

namespace donner::svg {

SVGGElement SVGGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGGElement(CreateEntity(registry, Tag, Type));
}

}  // namespace donner::svg
