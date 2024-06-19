#include "donner/svg/SVGEllipseElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg {

SVGEllipseElement SVGEllipseElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGEllipseElement(handle);
}

void SVGEllipseElement::setCx(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.cx.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setCy(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.cy.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setRx(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.rx.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setRy(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::EllipseComponent>().properties;
  properties.ry.set(value, css::Specificity::Override());
}

Lengthd SVGEllipseElement::cx() const {
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.cx.getRequired() : Lengthd();
}

Lengthd SVGEllipseElement::cy() const {
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.cy.getRequired() : Lengthd();
}

std::optional<Lengthd> SVGEllipseElement::rx() const {
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.rx.get() : std::nullopt;
}

std::optional<Lengthd> SVGEllipseElement::ry() const {
  const auto* component = handle_.try_get<components::EllipseComponent>();
  return component ? component->properties.ry.get() : std::nullopt;
}

Lengthd SVGEllipseElement::computedCx() const {
  compute();
  return handle_.get<components::ComputedEllipseComponent>().properties.cx.getRequired();
}

Lengthd SVGEllipseElement::computedCy() const {
  compute();
  return handle_.get<components::ComputedEllipseComponent>().properties.cy.getRequired();
}

Lengthd SVGEllipseElement::computedRx() const {
  compute();

  const components::ComputedStyleComponent& style =
      handle_.get<components::ComputedStyleComponent>();
  return std::get<0>(handle_.get<components::ComputedEllipseComponent>().properties.calculateRx(
      style.viewbox.value(), FontMetrics()));
}

Lengthd SVGEllipseElement::computedRy() const {
  compute();

  const components::ComputedStyleComponent& style =
      handle_.get_or_emplace<components::ComputedStyleComponent>();
  return std::get<0>(handle_.get<components::ComputedEllipseComponent>().properties.calculateRy(
      style.viewbox.value(), FontMetrics()));
}

void SVGEllipseElement::invalidate() const {
  handle_.remove<components::ComputedEllipseComponent>();
  handle_.remove<components::ComputedPathComponent>();
}

void SVGEllipseElement::compute() const {
  auto& ellipse = handle_.get_or_emplace<components::EllipseComponent>();
  components::ShapeSystem().createComputedPath(handle_, ellipse, FontMetrics(), nullptr);
}

}  // namespace donner::svg
