#include "src/svg/svg_rect_element.h"

#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGRectElement SVGRectElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGRectElement(handle);
}

void SVGRectElement::setX(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>().properties;
  properties.x.set(value, css::Specificity::Override());
}

void SVGRectElement::setY(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>().properties;
  properties.y.set(value, css::Specificity::Override());
}

void SVGRectElement::setWidth(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>().properties;
  properties.width.set(value, css::Specificity::Override());
}

void SVGRectElement::setHeight(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>().properties;
  properties.height.set(value, css::Specificity::Override());
}

void SVGRectElement::setRx(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>().properties;
  properties.rx.set(value, css::Specificity::Override());
}

void SVGRectElement::setRy(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>().properties;
  properties.ry.set(value, css::Specificity::Override());
}

Lengthd SVGRectElement::x() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.x.getRequired() : Lengthd();
}

Lengthd SVGRectElement::y() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.y.getRequired() : Lengthd();
}

Lengthd SVGRectElement::width() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.width.getRequired() : Lengthd();
}

Lengthd SVGRectElement::height() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.height.getRequired() : Lengthd();
}

std::optional<Lengthd> SVGRectElement::rx() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.rx.get() : std::nullopt;
}

std::optional<Lengthd> SVGRectElement::ry() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.ry.get() : std::nullopt;
}

Lengthd SVGRectElement::computedX() const {
  compute();
  return handle_.get<components::ComputedRectComponent>().properties.x.getRequired();
}

Lengthd SVGRectElement::computedY() const {
  compute();
  return handle_.get<components::ComputedRectComponent>().properties.y.getRequired();
}

Lengthd SVGRectElement::computedWidth() const {
  compute();
  return handle_.get<components::ComputedRectComponent>().properties.width.getRequired();
}

Lengthd SVGRectElement::computedHeight() const {
  compute();
  return handle_.get<components::ComputedRectComponent>().properties.height.getRequired();
}

Lengthd SVGRectElement::computedRx() const {
  compute();

  const components::ComputedStyleComponent& style =
      handle_.get<components::ComputedStyleComponent>();
  return std::get<0>(handle_.get<components::ComputedRectComponent>().properties.calculateRx(
      style.viewbox(), FontMetrics()));
}

Lengthd SVGRectElement::computedRy() const {
  compute();

  const components::ComputedStyleComponent& style =
      handle_.get_or_emplace<components::ComputedStyleComponent>();
  return std::get<0>(handle_.get<components::ComputedRectComponent>().properties.calculateRy(
      style.viewbox(), FontMetrics()));
}

std::optional<PathSpline> SVGRectElement::computedSpline() const {
  compute();
  if (const auto* computedPath = handle_.try_get<components::ComputedPathComponent>()) {
    return computedPath->spline;
  } else {
    return std::nullopt;
  }
}

void SVGRectElement::invalidate() const {
  handle_.remove<components::ComputedRectComponent>();
  handle_.remove<components::ComputedPathComponent>();
}

void SVGRectElement::compute() const {
  auto& rect = handle_.get_or_emplace<components::RectComponent>();
  rect.computePath(handle_, FontMetrics());
}

}  // namespace donner::svg
