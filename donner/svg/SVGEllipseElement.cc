#include "donner/svg/SVGEllipseElement.h"

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg {

SVGEllipseElement SVGEllipseElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGEllipseElement(handle);
}

void SVGEllipseElement::setCx(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.cx.set(value, css::Specificity::Override());
  access.bumpMutationRevision();
}

void SVGEllipseElement::setCy(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.cy.set(value, css::Specificity::Override());
  access.bumpMutationRevision();
}

void SVGEllipseElement::setRx(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.rx.set(value, css::Specificity::Override());
  access.bumpMutationRevision();
}

void SVGEllipseElement::setRy(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.ry.set(value, css::Specificity::Override());
  access.bumpMutationRevision();
}

Lengthd SVGEllipseElement::cx() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.cx.getRequired() : Lengthd();
}

Lengthd SVGEllipseElement::cy() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.cy.getRequired() : Lengthd();
}

std::optional<Lengthd> SVGEllipseElement::rx() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.rx.get() : std::nullopt;
}

std::optional<Lengthd> SVGEllipseElement::ry() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.ry.get() : std::nullopt;
}

Lengthd SVGEllipseElement::computedCx() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::ComputedEllipseComponent>().properties.cx.getRequired();
}

Lengthd SVGEllipseElement::computedCy() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::ComputedEllipseComponent>().properties.cy.getRequired();
}

Lengthd SVGEllipseElement::computedRx() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();

  return std::get<0>(handle_.get<components::ComputedEllipseComponent>().properties.calculateRx(
      components::LayoutSystem().getViewBox(handle_), FontMetrics()));
}

Lengthd SVGEllipseElement::computedRy() const {
  compute();
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();

  return std::get<0>(handle_.get<components::ComputedEllipseComponent>().properties.calculateRy(
      components::LayoutSystem().getViewBox(handle_), FontMetrics()));
}

void SVGEllipseElement::invalidate() const {
  handle_.remove<components::ComputedEllipseComponent>();
  handle_.remove<components::ComputedPathComponent>();
}

void SVGEllipseElement::compute() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  auto& ellipse = handle_.get_or_emplace<components::EllipseComponent>();
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  components::ShapeSystem().createComputedPath(handle_, ellipse, FontMetrics(), disabledSink);
}

}  // namespace donner::svg
