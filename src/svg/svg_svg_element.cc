#include "src/svg/svg_svg_element.h"

#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/svg_document.h"

namespace donner {

SVGSVGElement SVGSVGElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  Entity entity = CreateEntity(registry, RcString(Tag), Type);
  registry.emplace<ViewboxComponent>(entity);
  return SVGSVGElement(registry, entity);
}

void SVGSVGElement::setViewbox(std::optional<Boxd> viewbox) {
  registry_.get().get_or_emplace<ViewboxComponent>(entity_).viewbox = viewbox;
}

void SVGSVGElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  registry_.get().get_or_emplace<ViewboxComponent>(entity_).preserveAspectRatio =
      preserveAspectRatio;
}

void SVGSVGElement::setX(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).x = value;
}

void SVGSVGElement::setY(Lengthd value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).y = value;
}

void SVGSVGElement::setWidth(std::optional<Lengthd> value) {
  registry_.get().get_or_emplace<SizedElementComponent>(entity_).width = value;
}

void SVGSVGElement::setHeight(std::optional<Lengthd> value) {
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

std::optional<Lengthd> SVGSVGElement::width() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->width : std::nullopt;
}

std::optional<Lengthd> SVGSVGElement::height() const {
  const auto* component = registry_.get().try_get<SizedElementComponent>(entity_);
  return component ? component->height : std::nullopt;
}

}  // namespace donner
