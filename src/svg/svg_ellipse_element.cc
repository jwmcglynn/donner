#include "src/svg/svg_ellipse_element.h"

#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/ellipse_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGEllipseElement SVGEllipseElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGEllipseElement(CreateEntity(registry, RcString(Tag), Type));
}

void SVGEllipseElement::setCx(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.cx.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setCy(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.cy.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setRx(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.rx.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setRy(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.ry.set(value, css::Specificity::Override());
}

Lengthd SVGEllipseElement::cx() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.cx.getRequired() : Lengthd();
}

Lengthd SVGEllipseElement::cy() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.cy.getRequired() : Lengthd();
}

std::optional<Lengthd> SVGEllipseElement::rx() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.rx.get() : std::nullopt;
}

std::optional<Lengthd> SVGEllipseElement::ry() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.ry.get() : std::nullopt;
}

Lengthd SVGEllipseElement::computedCx() const {
  compute();
  return handle_.get<ComputedEllipseComponent>().properties.cx.getRequired();
}

Lengthd SVGEllipseElement::computedCy() const {
  compute();
  return handle_.get<ComputedEllipseComponent>().properties.cy.getRequired();
}

Lengthd SVGEllipseElement::computedRx() const {
  compute();
  return handle_.get<ComputedEllipseComponent>().properties.calculateRx();
}

Lengthd SVGEllipseElement::computedRy() const {
  compute();
  return handle_.get<ComputedEllipseComponent>().properties.calculateRy();
}

void SVGEllipseElement::invalidate() const {
  handle_.remove<ComputedEllipseComponent>();
  handle_.remove<ComputedPathComponent>();
}

void SVGEllipseElement::compute() const {
  auto& circle = handle_.get_or_emplace<EllipseComponent>();
  circle.computePath(handle_, FontMetrics());
}

}  // namespace donner::svg
