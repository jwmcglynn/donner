#include "donner/svg/SVGSVGElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/ViewboxComponent.h"

namespace donner::svg {

SVGSVGElement SVGSVGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::ViewboxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  handle.emplace<components::SizedElementComponent>();
  return SVGSVGElement(handle);
}

std::optional<Boxd> SVGSVGElement::viewbox() const {
  return handle_.get<components::ViewboxComponent>().viewbox;
}

PreserveAspectRatio SVGSVGElement::preserveAspectRatio() const {
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
}

Lengthd SVGSVGElement::x() const {
  return handle_.get<components::SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGSVGElement::y() const {
  return handle_.get<components::SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGSVGElement::width() const {
  return handle_.get<components::SizedElementComponent>().properties.width.getRequired();
}

std::optional<Lengthd> SVGSVGElement::height() const {
  return handle_.get<components::SizedElementComponent>().properties.height.getRequired();
}

void SVGSVGElement::setViewbox(std::optional<Boxd> viewbox) {
  handle_.get<components::ViewboxComponent>().viewbox = viewbox;
}

void SVGSVGElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio = preserveAspectRatio;
}

void SVGSVGElement::setX(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.x.set(value,
                                                                    css::Specificity::Override());
}

void SVGSVGElement::setY(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.y.set(value,
                                                                    css::Specificity::Override());
}

void SVGSVGElement::setWidth(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
}

void SVGSVGElement::setHeight(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
}

}  // namespace donner::svg
