#include "src/svg/svg_circle_element.h"

#include "src/svg/components/circle_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGCircleElement SVGCircleElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGCircleElement(registry, CreateEntity(registry, RcString(Tag), Type));
}

void SVGCircleElement::setCx(Lengthd value) {
  invalidate();

  auto& properties = registry_.get().get_or_emplace<CircleComponent>(entity_).properties;
  properties.cx.set(value, css::Specificity::Override());
}

void SVGCircleElement::setCy(Lengthd value) {
  invalidate();

  auto& properties = registry_.get().get_or_emplace<CircleComponent>(entity_).properties;
  properties.cy.set(value, css::Specificity::Override());
}

void SVGCircleElement::setR(Lengthd value) {
  invalidate();

  auto& properties = registry_.get().get_or_emplace<CircleComponent>(entity_).properties;
  properties.r.set(value, css::Specificity::Override());
}

Lengthd SVGCircleElement::cx() const {
  const auto* component = registry_.get().try_get<CircleComponent>(entity_);
  return component ? component->properties.cx.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::cy() const {
  const auto* component = registry_.get().try_get<CircleComponent>(entity_);
  return component ? component->properties.cy.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::r() const {
  const auto* component = registry_.get().try_get<CircleComponent>(entity_);
  return component ? component->properties.r.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::computedCx() const {
  compute();
  return registry_.get().get<ComputedCircleComponent>(entity_).properties.cx.getRequired();
}
Lengthd SVGCircleElement::computedCy() const {
  compute();
  return registry_.get().get<ComputedCircleComponent>(entity_).properties.cy.getRequired();
}
Lengthd SVGCircleElement::computedR() const {
  compute();
  return registry_.get().get<ComputedCircleComponent>(entity_).properties.r.getRequired();
}

void SVGCircleElement::invalidate() const {
  registry_.get().remove<ComputedCircleComponent>(entity_);
  registry_.get().remove<ComputedPathComponent>(entity_);
}

void SVGCircleElement::compute() const {
  EntityHandle handle = EntityHandle(registry_.get(), entity_);

  auto& circle = handle.get_or_emplace<CircleComponent>();
  circle.computePath(handle, FontMetrics());
}

}  // namespace donner::svg
