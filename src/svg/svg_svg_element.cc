#include "src/svg/svg_svg_element.h"

#include "src/svg/components/preserve_aspect_ratio_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGSVGElement SVGSVGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, RcString(Tag), Type);
  handle.emplace<ViewboxComponent>();
  handle.emplace<PreserveAspectRatioComponent>();
  handle.emplace<SizedElementComponent>();
  return SVGSVGElement(handle);
}

std::optional<Boxd> SVGSVGElement::viewbox() const {
  return handle_.get<ViewboxComponent>().viewbox;
}

PreserveAspectRatio SVGSVGElement::preserveAspectRatio() const {
  return handle_.get<PreserveAspectRatioComponent>().preserveAspectRatio;
}

Lengthd SVGSVGElement::x() const {
  return handle_.get<SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGSVGElement::y() const {
  return handle_.get<SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGSVGElement::width() const {
  return handle_.get<SizedElementComponent>().properties.width.getRequired();
}

std::optional<Lengthd> SVGSVGElement::height() const {
  return handle_.get<SizedElementComponent>().properties.height.getRequired();
}

void SVGSVGElement::setViewbox(std::optional<Boxd> viewbox) {
  handle_.get<ViewboxComponent>().viewbox = viewbox;
}

void SVGSVGElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get<PreserveAspectRatioComponent>().preserveAspectRatio = preserveAspectRatio;
}

void SVGSVGElement::setX(Lengthd value) {
  handle_.get<SizedElementComponent>().properties.x.set(value, css::Specificity::Override());
}

void SVGSVGElement::setY(Lengthd value) {
  handle_.get<SizedElementComponent>().properties.y.set(value, css::Specificity::Override());
}

void SVGSVGElement::setWidth(std::optional<Lengthd> value) {
  handle_.get<SizedElementComponent>().properties.width.set(value, css::Specificity::Override());
}

void SVGSVGElement::setHeight(std::optional<Lengthd> value) {
  handle_.get<SizedElementComponent>().properties.height.set(value, css::Specificity::Override());
}

}  // namespace donner::svg
