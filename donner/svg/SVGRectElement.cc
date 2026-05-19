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
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.x.set(value, css::Specificity::Override());
}

void SVGRectElement::setY(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.y.set(value, css::Specificity::Override());
}

void SVGRectElement::setWidth(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.width.set(value, css::Specificity::Override());
}

void SVGRectElement::setHeight(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.height.set(value, css::Specificity::Override());
}

void SVGRectElement::setRx(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.rx.set(value, css::Specificity::Override());
}

void SVGRectElement::setRy(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::RectComponent>(access).properties;
  properties.ry.set(value, css::Specificity::Override());
}

Lengthd SVGRectElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.x.getRequired() : Lengthd();
}

Lengthd SVGRectElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.y.getRequired() : Lengthd();
}

Lengthd SVGRectElement::width() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.width.getRequired() : Lengthd();
}

Lengthd SVGRectElement::height() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.height.getRequired() : Lengthd();
}

std::optional<Lengthd> SVGRectElement::rx() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.rx.get() : std::nullopt;
}

std::optional<Lengthd> SVGRectElement::ry() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RectComponent>();
  return component ? component->properties.ry.get() : std::nullopt;
}

Lengthd SVGRectElement::computedX() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::ComputedRectComponent>().properties.x.getRequired();
}

Lengthd SVGRectElement::computedY() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::ComputedRectComponent>().properties.y.getRequired();
}

Lengthd SVGRectElement::computedWidth() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::ComputedRectComponent>().properties.width.getRequired();
}

Lengthd SVGRectElement::computedHeight() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::ComputedRectComponent>().properties.height.getRequired();
}

Lengthd SVGRectElement::computedRx() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();

  return std::get<0>(handle_.get<components::ComputedRectComponent>().properties.calculateRx(
      components::LayoutSystem().getViewBox(handle_), FontMetrics()));
}

Lengthd SVGRectElement::computedRy() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();

  return std::get<0>(handle_.get<components::ComputedRectComponent>().properties.calculateRy(
      components::LayoutSystem().getViewBox(handle_), FontMetrics()));
}

std::optional<Path> SVGRectElement::computedSpline() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
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
