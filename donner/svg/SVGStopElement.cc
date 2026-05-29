#include "donner/svg/SVGStopElement.h"

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/PaintSystem.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {

SVGStopElement SVGStopElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  return SVGStopElement(handle);
}

void SVGStopElement::setOffset(float value) {
  assert(value >= 0.0f && value <= 1.0f);

  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::StopComponent>(access).properties.offset = value;
}

void SVGStopElement::setStopColor(css::Color value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::StopComponent>(access).properties.stopColor.set(
      value, css::Specificity::Override());
}

void SVGStopElement::setStopOpacity(double value) {
  assert(value >= 0.0 && value <= 1.0);

  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::StopComponent>(access).properties.stopOpacity.set(
      value, css::Specificity::Override());
}

float SVGStopElement::offset() const {
  const auto* component = handle_.try_get<components::StopComponent>();
  return component ? component->properties.offset : components::StopProperties().offset;
}

css::Color SVGStopElement::stopColor() const {
  const auto* component = handle_.try_get<components::StopComponent>();
  return component ? component->properties.stopColor.get().value()
                   : components::StopProperties().stopColor.get().value();
}

double SVGStopElement::stopOpacity() const {
  const auto* component = handle_.try_get<components::StopComponent>();
  return component ? component->properties.stopOpacity.get().value()
                   : components::StopProperties().stopOpacity.get().value();
}

css::Color SVGStopElement::computedStopColor() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  const components::ComputedStopComponent& computed = components::PaintSystem().createComputedStop(
      handle_, handle_.get_or_emplace<components::StopComponent>(access), disabledSink);
  return computed.properties.stopColor.get().value();
}

double SVGStopElement::computedStopOpacity() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  const components::ComputedStopComponent& computed = components::PaintSystem().createComputedStop(
      handle_, handle_.get_or_emplace<components::StopComponent>(access), disabledSink);
  return computed.properties.stopOpacity.get().value();
}

void SVGStopElement::invalidate() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  handle_.remove<components::ComputedStopComponent>(access);
  components::RenderingContext(*handle_.registry()).invalidateRenderTree();
}

}  // namespace donner::svg
