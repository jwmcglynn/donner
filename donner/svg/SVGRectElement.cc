#include "donner/svg/SVGRectElement.h"

#include "donner/base/ParseWarningSink.h"
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
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.x.set(value, css::Specificity::Override());
}

void SVGRectElement::setY(Lengthd value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.y.set(value, css::Specificity::Override());
}

void SVGRectElement::setWidth(Lengthd value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.width.set(value, css::Specificity::Override());
}

void SVGRectElement::setHeight(Lengthd value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.height.set(value, css::Specificity::Override());
}

void SVGRectElement::setRx(std::optional<Lengthd> value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.rx.set(value, css::Specificity::Override());
}

void SVGRectElement::setRy(std::optional<Lengthd> value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.ry.set(value, css::Specificity::Override());
}

Lengthd SVGRectElement::x() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.x.get().value() : Lengthd();
}

Lengthd SVGRectElement::y() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.y.get().value() : Lengthd();
}

Lengthd SVGRectElement::width() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.width.get().value() : Lengthd();
}

Lengthd SVGRectElement::height() const {
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.height.get().value() : Lengthd();
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
  return handle_.get<components::ComputedRectComponent>().properties.x.get().value();
}

Lengthd SVGRectElement::computedY() const {
  compute();
  return handle_.get<components::ComputedRectComponent>().properties.y.get().value();
}

Lengthd SVGRectElement::computedWidth() const {
  compute();
  return handle_.get<components::ComputedRectComponent>().properties.width.get().value();
}

Lengthd SVGRectElement::computedHeight() const {
  compute();
  return handle_.get<components::ComputedRectComponent>().properties.height.get().value();
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

std::optional<Path> SVGRectElement::computedSpline() const {
  compute();
  if (const auto* computedPath = handle_.try_get<components::ComputedPathComponent>()) {
    return computedPath->spline;
  } else {
    return std::nullopt;
  }
}

void SVGRectElement::invalidate() const {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.remove<components::ComputedRectComponent>(access);
  handle_.remove<components::ComputedPathComponent>(access);
}

void SVGRectElement::compute() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  auto& rect = handle_.get_or_emplace<components::RectComponent>(access);
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  components::ShapeSystem().createComputedPath(handle_, rect, FontMetrics(), disabledSink);
}

}  // namespace donner::svg
