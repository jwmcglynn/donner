#include "src/svg/svg_rect_element.h"

#include "src/svg/components/rect_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGRectElement SVGRectElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGRectElement(registry, CreateEntity(registry, RcString(Tag), Type));
}

void SVGRectElement::setX(Lengthd value) {
  registry_.get().get_or_emplace<RectComponent>(entity_).x = value;
}

void SVGRectElement::setY(Lengthd value) {
  registry_.get().get_or_emplace<RectComponent>(entity_).y = value;
}

void SVGRectElement::setWidth(Lengthd value) {
  registry_.get().get_or_emplace<RectComponent>(entity_).width = value;
}

void SVGRectElement::setHeight(Lengthd value) {
  registry_.get().get_or_emplace<RectComponent>(entity_).height = value;
}

void SVGRectElement::setRx(std::optional<Lengthd> value) {
  registry_.get().get_or_emplace<RectComponent>(entity_).rx = value;
}

void SVGRectElement::setRy(std::optional<Lengthd> value) {
  registry_.get().get_or_emplace<RectComponent>(entity_).ry = value;
}

Lengthd SVGRectElement::x() const {
  const auto* component = registry_.get().try_get<RectComponent>(entity_);
  return component ? component->x : Lengthd();
}

Lengthd SVGRectElement::y() const {
  const auto* component = registry_.get().try_get<RectComponent>(entity_);
  return component ? component->y : Lengthd();
}

Lengthd SVGRectElement::width() const {
  const auto* component = registry_.get().try_get<RectComponent>(entity_);
  return component ? component->width : Lengthd();
}

Lengthd SVGRectElement::height() const {
  const auto* component = registry_.get().try_get<RectComponent>(entity_);
  return component ? component->height : Lengthd();
}

std::optional<Lengthd> SVGRectElement::rx() const {
  const auto* component = registry_.get().try_get<RectComponent>(entity_);
  return component ? component->rx : std::nullopt;
}

std::optional<Lengthd> SVGRectElement::ry() const {
  const auto* component = registry_.get().try_get<RectComponent>(entity_);
  return component ? component->ry : std::nullopt;
}

}  // namespace donner::svg
