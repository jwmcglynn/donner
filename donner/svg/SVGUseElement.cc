#include "donner/svg/SVGUseElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"

namespace donner::svg {

SVGUseElement SVGUseElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::SizedElementComponent>().applyTranslationForUseElement = true;
  return SVGUseElement(handle);
}

void SVGUseElement::setHref(const RcString& value) {
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
