#include "src/svg/svg_stop_element.h"

#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/components/stop_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGStopElement SVGStopElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, RcString(Tag), Type);
  handle.emplace<RenderingBehaviorComponent>(RenderingBehavior::Nonrenderable);
  return SVGStopElement(handle);
}

void SVGStopElement::setOffset(float value) {
  assert(value >= 0.0f && value <= 1.0f);

  invalidate();
  handle_.get_or_emplace<StopComponent>().properties.offset = value;
}

void SVGStopElement::setStopColor(css::Color value) {
  invalidate();
  handle_.get_or_emplace<StopComponent>().properties.stopColor.set(value,
                                                                   css::Specificity::Override());
}

void SVGStopElement::setStopOpacity(double value) {
  assert(value >= 0.0 && value <= 1.0);

  invalidate();
  handle_.get_or_emplace<StopComponent>().properties.stopOpacity.set(value,
                                                                     css::Specificity::Override());
}

float SVGStopElement::offset() const {
  return handle_.get_or_emplace<StopComponent>().properties.offset;
}

css::Color SVGStopElement::stopColor() const {
  return handle_.get_or_emplace<StopComponent>().properties.stopColor.getRequired();
}

double SVGStopElement::stopOpacity() const {
  return handle_.get_or_emplace<StopComponent>().properties.stopOpacity.getRequired();
}

css::Color SVGStopElement::computedStopColor() const {
  // TODO: Cache the result so this doesn't need to recompute?
  handle_.get_or_emplace<StopComponent>().compute(handle_);
  return handle_.get<ComputedStopComponent>().properties.stopColor.getRequired();
}

double SVGStopElement::computedStopOpacity() const {
  handle_.get_or_emplace<StopComponent>().compute(handle_);
  return handle_.get<ComputedStopComponent>().properties.stopOpacity.getRequired();
}

void SVGStopElement::invalidate() const {
  handle_.remove<ComputedStopComponent>();
}

}  // namespace donner::svg
