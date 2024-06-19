#include "donner/svg/SVGGElement.h"

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

SVGGElement SVGGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGGElement(CreateEntity(registry, Tag, Type));
}

}  // namespace donner::svg
