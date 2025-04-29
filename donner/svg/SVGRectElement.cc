#include "donner/svg/SVGRectElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg {

SVGRectElement SVGRectElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
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

  return std::get<0>(handle_.get<components::ComputedRectComponent>().properties.calculateRx(
      components::LayoutSystem().getViewBox(handle_), FontMetrics()));
}

Lengthd SVGRectElement::computedRy() const {
  compute();

  return std::get<0>(handle_.get<components::ComputedRectComponent>().properties.calculateRy(
      components::LayoutSystem().getViewBox(handle_), FontMetrics()));
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
  components::ShapeSystem().createComputedPath(handle_, rect, FontMetrics(), nullptr);
}

}  // namespace donner::svg
