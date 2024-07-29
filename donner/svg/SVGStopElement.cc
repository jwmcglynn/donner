#include "donner/svg/SVGStopElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/PaintSystem.h"
#include "donner/svg/components/paint/StopComponent.h"

namespace donner::svg {

SVGStopElement SVGStopElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  return SVGStopElement(handle);
}

void SVGStopElement::setOffset(float value) {
  assert(value >= 0.0f && value <= 1.0f);

  invalidate();
  handle_.get_or_emplace<components::StopComponent>().properties.offset = value;
}

void SVGStopElement::setStopColor(css::Color value) {
  invalidate();
  handle_.get_or_emplace<components::StopComponent>().properties.stopColor.set(
      value, css::Specificity::Override());
}

void SVGStopElement::setStopOpacity(double value) {
  assert(value >= 0.0 && value <= 1.0);

  invalidate();
  handle_.get_or_emplace<components::StopComponent>().properties.stopOpacity.set(
      value, css::Specificity::Override());
}

float SVGStopElement::offset() const {
  return handle_.get_or_emplace<components::StopComponent>().properties.offset;
}

css::Color SVGStopElement::stopColor() const {
  return handle_.get_or_emplace<components::StopComponent>().properties.stopColor.getRequired();
}

double SVGStopElement::stopOpacity() const {
  return handle_.get_or_emplace<components::StopComponent>().properties.stopOpacity.getRequired();
}

css::Color SVGStopElement::computedStopColor() const {
  const components::ComputedStopComponent& computed = components::PaintSystem().createComputedStop(
      handle_, handle_.get_or_emplace<components::StopComponent>(), nullptr);
  return computed.properties.stopColor.getRequired();
}

double SVGStopElement::computedStopOpacity() const {
  const components::ComputedStopComponent& computed = components::PaintSystem().createComputedStop(
      handle_, handle_.get_or_emplace<components::StopComponent>(), nullptr);
  return computed.properties.stopOpacity.getRequired();
}

void SVGStopElement::invalidate() const {
  handle_.remove<components::ComputedStopComponent>();
}

}  // namespace donner::svg
