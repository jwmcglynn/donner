#include "donner/svg/SVGUnknownElement.h"

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

SVGUnknownElement SVGUnknownElement::Create(SVGDocument& document,
                                            const XMLQualifiedNameRef& tagName) {
  Registry& registry = document.registry();
  return SVGUnknownElement(CreateEntity(registry, tagName, Type));
}

}  // namespace donner::svg
