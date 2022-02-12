#include "src/svg/svg_svg_element.h"

#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGSVGElement SVGSVGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, RcString(Tag), Type);
  handle.emplace<ViewboxComponent>();
  return SVGSVGElement(handle);
}

void SVGSVGElement::setViewbox(std::optional<Boxd> viewbox) {
  handle_.get_or_emplace<ViewboxComponent>().viewbox = viewbox;
}

void SVGSVGElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get_or_emplace<ViewboxComponent>().preserveAspectRatio = preserveAspectRatio;
}

void SVGSVGElement::setX(Lengthd value) {
  handle_.get_or_emplace<SizedElementComponent>().x = value;
}

void SVGSVGElement::setY(Lengthd value) {
  handle_.get_or_emplace<SizedElementComponent>().y = value;
}

void SVGSVGElement::setWidth(std::optional<Lengthd> value) {
  handle_.get_or_emplace<SizedElementComponent>().width = value;
}

void SVGSVGElement::setHeight(std::optional<Lengthd> value) {
  handle_.get_or_emplace<SizedElementComponent>().height = value;
}

std::optional<Boxd> SVGSVGElement::viewbox() const {
  if (const auto* component = handle_.try_get<ViewboxComponent>()) {
    return component->viewbox;
  }

  return std::nullopt;
}

std::optional<PreserveAspectRatio> SVGSVGElement::preserveAspectRatio() const {
  if (const auto* component = handle_.try_get<ViewboxComponent>()) {
    return component->preserveAspectRatio;
  }

  return std::nullopt;
}

Lengthd SVGSVGElement::x() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->x : Lengthd();
}

Lengthd SVGSVGElement::y() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->y : Lengthd();
}

std::optional<Lengthd> SVGSVGElement::width() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->width : std::nullopt;
}

std::optional<Lengthd> SVGSVGElement::height() const {
  const auto* component = handle_.try_get<SizedElementComponent>();
  return component ? component->height : std::nullopt;
}

}  // namespace donner::svg
