#include "src/svg/svg_use_element.h"

#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGUseElement SVGUseElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGUseElement(CreateEntity(registry, RcString(Tag), Type));
}

void SVGUseElement::setHref(RcString value) {
  handle_.emplace_or_replace<ShadowTreeComponent>(value);
  // Force the shadow tree to be regenerated.
  handle_.remove<ComputedShadowTreeComponent>();
}

RcString SVGUseElement::href() const {
  if (const auto* component = handle_.try_get<ShadowTreeComponent>()) {
    return component->href();
  } else {
    return "";
  }
}

void SVGUseElement::setX(Lengthd value) {
  handle_.get_or_emplace<SizedElementComponent>().x = value;
}

void SVGUseElement::setY(Lengthd value) {
  handle_.get_or_emplace<SizedElementComponent>().y = value;
}

void SVGUseElement::setWidth(std::optional<Lengthd> value) {
  handle_.get_or_emplace<SizedElementComponent>().width = value;
}

void SVGUseElement::setHeight(std::optional<Lengthd> value) {
  handle_.get_or_emplace<SizedElementComponent>().height = value;
}

Lengthd SVGUseElement::x() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->x : Lengthd();
}

Lengthd SVGUseElement::y() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->y : Lengthd();
}

std::optional<Lengthd> SVGUseElement::width() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->width : std::nullopt;
}

std::optional<Lengthd> SVGUseElement::height() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->height : std::nullopt;
}

}  // namespace donner::svg
