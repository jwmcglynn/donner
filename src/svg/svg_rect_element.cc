#include "src/svg/svg_rect_element.h"

#include "src/svg/components/rect_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGRectElement SVGRectElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGRectElement(CreateEntity(registry, RcString(Tag), Type));
}

void SVGRectElement::setX(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<RectComponent>().properties;
  properties.x.set(value, css::Specificity::Override());
}

void SVGRectElement::setY(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<RectComponent>().properties;
  properties.y.set(value, css::Specificity::Override());
}

void SVGRectElement::setWidth(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<RectComponent>().properties;
  properties.width.set(value, css::Specificity::Override());
}

void SVGRectElement::setHeight(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<RectComponent>().properties;
  properties.height.set(value, css::Specificity::Override());
}

void SVGRectElement::setRx(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<RectComponent>().properties;
  properties.rx.set(value, css::Specificity::Override());
}

void SVGRectElement::setRy(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<RectComponent>().properties;
  properties.ry.set(value, css::Specificity::Override());
}

Lengthd SVGRectElement::x() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->properties.x.getRequired() : Lengthd();
}

Lengthd SVGRectElement::y() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->properties.y.getRequired() : Lengthd();
}

Lengthd SVGRectElement::width() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->properties.width.getRequired() : Lengthd();
}

Lengthd SVGRectElement::height() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->properties.height.getRequired() : Lengthd();
}

std::optional<Lengthd> SVGRectElement::rx() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->properties.rx.get() : std::nullopt;
}

std::optional<Lengthd> SVGRectElement::ry() const {
  const auto* component = handle_.try_get<RectComponent>();
  return component ? component->properties.ry.get() : std::nullopt;
}

Lengthd SVGRectElement::computedX() const {
  compute();
  return handle_.get<ComputedRectComponent>().properties.x.getRequired();
}

Lengthd SVGRectElement::computedY() const {
  compute();
  return handle_.get<ComputedRectComponent>().properties.y.getRequired();
}

Lengthd SVGRectElement::computedWidth() const {
  compute();
  return handle_.get<ComputedRectComponent>().properties.width.getRequired();
}

Lengthd SVGRectElement::computedHeight() const {
  compute();
  return handle_.get<ComputedRectComponent>().properties.height.getRequired();
}

Lengthd SVGRectElement::computedRx() const {
  compute();
  return handle_.get<ComputedRectComponent>().properties.calculateRx();
}

Lengthd SVGRectElement::computedRy() const {
  compute();
  return handle_.get<ComputedRectComponent>().properties.calculateRy();
}

std::optional<PathSpline> SVGRectElement::computedSpline() const {
  compute();
  if (const auto* computedPath = handle_.try_get<ComputedPathComponent>()) {
    return computedPath->spline;
  } else {
    return std::nullopt;
  }
}

void SVGRectElement::invalidate() const {
  handle_.remove<ComputedRectComponent>();
  handle_.remove<ComputedPathComponent>();
}

void SVGRectElement::compute() const {
  auto& rect = handle_.get_or_emplace<RectComponent>();
  rect.computePath(handle_, FontMetrics());
}

}  // namespace donner::svg
