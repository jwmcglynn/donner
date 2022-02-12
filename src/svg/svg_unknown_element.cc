#include "src/svg/svg_unknown_element.h"

#include "src/svg/svg_document.h"

namespace donner {

SVGUnknownElement SVGUnknownElement::Create(SVGDocument& document, RcString typeString) {
  Registry& registry = document.registry();
  return SVGUnknownElement(registry, CreateEntity(registry, std::move(typeString), Type));
}

}  // namespace donner
