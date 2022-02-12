#include "src/svg/svg_circle_element.h"

#include "src/svg/components/circle_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGCircleElement SVGCircleElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGCircleElement(CreateEntity(registry, RcString(Tag), Type));
}

void SVGCircleElement::setCx(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<CircleComponent>().properties;
  properties.cx.set(value, css::Specificity::Override());
}

void SVGCircleElement::setCy(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<CircleComponent>().properties;
  properties.cy.set(value, css::Specificity::Override());
}

void SVGCircleElement::setR(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<CircleComponent>().properties;
  properties.r.set(value, css::Specificity::Override());
}

Lengthd SVGCircleElement::cx() const {
  const auto* component = handle_.try_get<CircleComponent>();
  return component ? component->properties.cx.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::cy() const {
  const auto* component = handle_.try_get<CircleComponent>();
  return component ? component->properties.cy.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::r() const {
  const auto* component = handle_.try_get<CircleComponent>();
  return component ? component->properties.r.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::computedCx() const {
  compute();
  return handle_.get<ComputedCircleComponent>().properties.cx.getRequired();
}
Lengthd SVGCircleElement::computedCy() const {
  compute();
  return handle_.get<ComputedCircleComponent>().properties.cy.getRequired();
}
Lengthd SVGCircleElement::computedR() const {
  compute();
  return handle_.get<ComputedCircleComponent>().properties.r.getRequired();
}

void SVGCircleElement::invalidate() const {
  handle_.remove<ComputedCircleComponent>();
  handle_.remove<ComputedPathComponent>();
}

void SVGCircleElement::compute() const {
  auto& circle = handle_.get_or_emplace<CircleComponent>();
  circle.computePath(handle_, FontMetrics());
}

}  // namespace donner::svg
