#include "donner/svg/SVGUnknownElement.h"

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

SVGUnknownElement SVGUnknownElement::Create(SVGDocument& document,
                                            const XMLQualifiedNameRef& xmlTypeName) {
  Registry& registry = document.registry();
  return SVGUnknownElement(CreateEntity(registry, xmlTypeName, Type));
}

}  // namespace donner::svg
