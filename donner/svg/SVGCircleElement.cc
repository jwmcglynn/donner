#include "donner/svg/SVGCircleElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg {

SVGCircleElement SVGCircleElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGCircleElement(handle);
}

void SVGCircleElement::setCx(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::CircleComponent>().properties;
  properties.cx.set(value, css::Specificity::Override());
}

void SVGCircleElement::setCy(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::CircleComponent>().properties;
  properties.cy.set(value, css::Specificity::Override());
}

void SVGCircleElement::setR(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<components::CircleComponent>().properties;
  properties.r.set(value, css::Specificity::Override());
}

Lengthd SVGCircleElement::cx() const {
  const auto* component = handle_.try_get<components::CircleComponent>();
  return component ? component->properties.cx.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::cy() const {
  const auto* component = handle_.try_get<components::CircleComponent>();
  return component ? component->properties.cy.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::r() const {
  const auto* component = handle_.try_get<components::CircleComponent>();
  return component ? component->properties.r.getRequired() : Lengthd();
}

Lengthd SVGCircleElement::computedCx() const {
  compute();
  return handle_.get<components::ComputedCircleComponent>().properties.cx.getRequired();
}

Lengthd SVGCircleElement::computedCy() const {
  compute();
  return handle_.get<components::ComputedCircleComponent>().properties.cy.getRequired();
}

Lengthd SVGCircleElement::computedR() const {
  compute();
  return handle_.get<components::ComputedCircleComponent>().properties.r.getRequired();
}

void SVGCircleElement::invalidate() const {
  handle_.remove<components::ComputedCircleComponent>();
  handle_.remove<components::ComputedPathComponent>();
}

void SVGCircleElement::compute() const {
  auto& circle = handle_.get_or_emplace<components::CircleComponent>();
  components::ShapeSystem().createComputedPath(handle_, circle, FontMetrics(), nullptr);
}

}  // namespace donner::svg
