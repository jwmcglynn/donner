#include "src/svg/svg_circle_element.h"

#include "src/svg/components/circle_component.h"
#include "src/svg/svg_document.h"

namespace donner {

SVGCircleElement SVGCircleElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGCircleElement(registry, CreateEntity(registry, RcString(Tag), Type));
}

void SVGCircleElement::setCx(Lengthd value) {
  registry_.get().get_or_emplace<CircleComponent>(entity_).cx = value;
}

void SVGCircleElement::setCy(Lengthd value) {
  registry_.get().get_or_emplace<CircleComponent>(entity_).cy = value;
}

void SVGCircleElement::setR(Lengthd value) {
  registry_.get().get_or_emplace<CircleComponent>(entity_).r = value;
}

Lengthd SVGCircleElement::cx() const {
  const auto* component = registry_.get().try_get<CircleComponent>(entity_);
  return component ? component->cx : Lengthd();
}

Lengthd SVGCircleElement::cy() const {
  const auto* component = registry_.get().try_get<CircleComponent>(entity_);
  return component ? component->cy : Lengthd();
}

Lengthd SVGCircleElement::r() const {
  const auto* component = registry_.get().try_get<CircleComponent>(entity_);
  return component ? component->r : Lengthd();
}

}  // namespace donner
