#include "src/svg/svg_use_element.h"

#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGUseElement SVGUseElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGUseElement(registry, CreateEntity(registry, RcString(Tag), Type));
}

void SVGUseElement::setHref(RcString value) {
  registry_.get().emplace_or_replace<ShadowTreeComponent>(entity_, value);
  // Force the shadow tree to be regenerated.
  registry_.get().remove<ComputedShadowTreeComponent>(entity_);
}

RcString SVGUseElement::href() const {
  if (const auto* component = registry_.get().try_get<ShadowTreeComponent>(entity_)) {
    return component->href();
  } else {
    return "";
  }
}

void SVGUseElement::setX(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).x = value;
}

void SVGUseElement::setY(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).y = value;
}

void SVGUseElement::setWidth(std::optional<Lengthd> value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).width = value;
}

void SVGUseElement::setHeight(std::optional<Lengthd> value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).height = value;
}

Lengthd SVGUseElement::x() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->x : Lengthd();
}

Lengthd SVGUseElement::y() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->y : Lengthd();
}

std::optional<Lengthd> SVGUseElement::width() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->width : std::nullopt;
}

std::optional<Lengthd> SVGUseElement::height() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->height : std::nullopt;
}

}  // namespace donner::svg
