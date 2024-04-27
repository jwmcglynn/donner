#include "src/svg/svg_unknown_element.h"

#include "src/svg/svg_document.h"

namespace donner::svg {

SVGUnknownElement SVGUnknownElement::Create(SVGDocument& document,
                                            const XMLQualifiedNameRef& xmlTypeName) {
  Registry& registry = document.registry();
  return SVGUnknownElement(CreateEntity(registry, xmlTypeName, Type));
}

}  // namespace donner::svg
