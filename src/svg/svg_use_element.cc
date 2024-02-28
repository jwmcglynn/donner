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
  auto& shadowTree = handle_.emplace_or_replace<components::ShadowTreeComponent>();
  shadowTree.setMainHref(value);
  shadowTree.setsContextColors = true;

  // Force the shadow tree to be regenerated.
  handle_.remove<components::ComputedShadowTreeComponent>();
}

RcString SVGUseElement::href() const {
  if (const auto* component = handle_.try_get<components::ShadowTreeComponent>()) {
    if (auto maybeMainHref = component->mainHref()) {
      return maybeMainHref.value();
    }
  }

  return "";
}

void SVGUseElement::setX(Lengthd value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.x.set(
      value, css::Specificity::Override());
}

void SVGUseElement::setY(Lengthd value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.y.set(
      value, css::Specificity::Override());
}

void SVGUseElement::setWidth(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
}

void SVGUseElement::setHeight(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
}

Lengthd SVGUseElement::x() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGUseElement::y() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGUseElement::width() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.width.get();
}

std::optional<Lengthd> SVGUseElement::height() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.height.get();
}

}  // namespace donner::svg
