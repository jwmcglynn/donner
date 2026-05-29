#include "donner/svg/SVGCircleElement.h"

#include "donner/base/ParseWarningSink.h"
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
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::CircleComponent>(access).properties;
  properties.cx.set(value, css::Specificity::Override());
}

void SVGCircleElement::setCy(Lengthd value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::CircleComponent>(access).properties;
  properties.cy.set(value, css::Specificity::Override());
}

void SVGCircleElement::setR(Lengthd value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  auto& properties = handle_.get_or_emplace<components::CircleComponent>(access).properties;
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
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.remove<components::ComputedCircleComponent>(access);
  handle_.remove<components::ComputedPathComponent>(access);
}

void SVGCircleElement::compute() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  auto& circle = handle_.get_or_emplace<components::CircleComponent>(access);
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  components::ShapeSystem().createComputedPath(handle_, circle, FontMetrics(), disabledSink);
}

}  // namespace donner::svg
