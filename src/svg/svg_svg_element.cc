#include "src/svg/svg_svg_element.h"

#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/svg_document.h"

namespace donner {

SVGSVGElement SVGSVGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGSVGElement(registry, CreateEntity(registry, Type));
}

void SVGSVGElement::setViewbox(Boxd viewbox) {
  registry_.get().get_or_emplace<ViewboxComponent>(entity_).viewbox = viewbox;
}

void SVGSVGElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  registry_.get().get_or_emplace<ViewboxComponent>(entity_).preserveAspectRatio =
      preserveAspectRatio;
}

void SVGSVGElement::clearViewbox() {
  registry_.get().remove_if_exists<ViewboxComponent>(entity_);
}

void SVGSVGElement::setX(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).x = value;
}

void SVGSVGElement::setY(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).y = value;
}

void SVGSVGElement::setWidth(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).width = value;
}

void SVGSVGElement::setHeight(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).height = value;
}

std::optional<Boxd> SVGSVGElement::viewbox() const {
  if (const auto* component = registry_.get().try_get<ViewboxComponent>(entity_)) {
    return component->viewbox;
  }

  return std::nullopt;
}

std::optional<PreserveAspectRatio> SVGSVGElement::preserveAspectRatio() const {
  if (const auto* component = registry_.get().try_get<ViewboxComponent>(entity_)) {
    return component->preserveAspectRatio;
  }

  return std::nullopt;
}

Lengthd SVGSVGElement::x() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->x : Lengthd();
}

Lengthd SVGSVGElement::y() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->y : Lengthd();
}

Lengthd SVGSVGElement::width() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->width : Lengthd();
}

Lengthd SVGSVGElement::height() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->height : Lengthd();
}

}  // namespace donner