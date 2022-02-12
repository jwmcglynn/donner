#include "src/svg/svg_rect_element.h"

#include "src/svg/components/rect_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGRectElement SVGRectElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGRectElement(CreateEntity(registry, RcString(Tag), Type));
}

void SVGRectElement::setX(Lengthd value) {
  handle_.get_or_emplace<RectComponent>().x = value;
}

void SVGRectElement::setY(Lengthd value) {
  handle_.get_or_emplace<RectComponent>().y = value;
}

void SVGRectElement::setWidth(Lengthd value) {
  handle_.get_or_emplace<RectComponent>().width = value;
}

void SVGRectElement::setHeight(Lengthd value) {
  handle_.get_or_emplace<RectComponent>().height = value;
}

void SVGRectElement::setRx(std::optional<Lengthd> value) {
  handle_.get_or_emplace<RectComponent>().rx = value;
}

void SVGRectElement::setRy(std::optional<Lengthd> value) {
  handle_.get_or_emplace<RectComponent>().ry = value;
}

Lengthd SVGRectElement::x() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->x : Lengthd();
}

Lengthd SVGRectElement::y() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->y : Lengthd();
}

Lengthd SVGRectElement::width() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->width : Lengthd();
}

Lengthd SVGRectElement::height() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->height : Lengthd();
}

std::optional<Lengthd> SVGRectElement::rx() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->rx : std::nullopt;
}

std::optional<Lengthd> SVGRectElement::ry() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->ry : std::nullopt;
}

}  // namespace donner::svg
