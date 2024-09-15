#include "donner/svg/SVGMaskElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"

namespace donner::svg {

SVGMaskElement SVGMaskElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::MaskComponent>();
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  return SVGMaskElement(handle);
}

MaskUnits SVGMaskElement::maskUnits() const {
  return handle_.get<components::MaskComponent>().maskUnits.value_or(MaskUnits::Default);
}

void SVGMaskElement::setMaskUnits(MaskUnits value) {
  handle_.get<components::MaskComponent>().maskUnits = value;
}

MaskContentUnits SVGMaskElement::maskContentUnits() const {
  return handle_.get<components::MaskComponent>().maskContentUnits.value_or(
      MaskContentUnits::Default);
}

void SVGMaskElement::setMaskContentUnits(MaskContentUnits value) {
  handle_.get<components::MaskComponent>().maskContentUnits = value;
}

void SVGMaskElement::setX(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.x.set(
      value, css::Specificity::Override());
}

void SVGMaskElement::setY(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.y.set(
      value, css::Specificity::Override());
}

void SVGMaskElement::setWidth(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
}

void SVGMaskElement::setHeight(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
}

std::optional<Lengthd> SVGMaskElement::x() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.x.get();
}

std::optional<Lengthd> SVGMaskElement::y() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.y.get();
}

std::optional<Lengthd> SVGMaskElement::width() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.width.get();
}

std::optional<Lengthd> SVGMaskElement::height() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.height.get();
}

}  // namespace donner::svg
