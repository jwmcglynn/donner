#include "donner/svg/SVGImageElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"

namespace donner::svg {

SVGImageElement SVGImageElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  handle.emplace<components::SizedElementComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  return SVGImageElement(handle);
}

void SVGImageElement::setHref(RcStringOrRef value) {
  handle_.get_or_emplace<components::ImageComponent>().href = RcString(value);
}

RcString SVGImageElement::href() const {
  return handle_.get_or_emplace<components::ImageComponent>().href;
}

void SVGImageElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>().preserveAspectRatio =
      preserveAspectRatio;
}

PreserveAspectRatio SVGImageElement::preserveAspectRatio() const {
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
}

void SVGImageElement::setX(Lengthd value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.x.set(
      value, css::Specificity::Override());
}

void SVGImageElement::setY(Lengthd value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.y.set(
      value, css::Specificity::Override());
}

void SVGImageElement::setWidth(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
}

void SVGImageElement::setHeight(std::optional<Lengthd> value) {
  handle_.get_or_emplace<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
}

Lengthd SVGImageElement::x() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGImageElement::y() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGImageElement::width() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.width.get();
}

std::optional<Lengthd> SVGImageElement::height() const {
  return handle_.get_or_emplace<components::SizedElementComponent>().properties.height.get();
}

}  // namespace donner::svg
