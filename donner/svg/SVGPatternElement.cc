#include "donner/svg/SVGPatternElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/layout/ViewboxComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/style/StyleComponent.h"  // DoNotInheritFillOrStrokeTag
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

namespace {

/// The default `xMidYMid meet` value for \ref xml_pattern `preserveAspectRatio`
static constexpr PreserveAspectRatio kPatternDefaultPreserveAspectRatio{
    PreserveAspectRatio::Align::XMidYMid, PreserveAspectRatio::MeetOrSlice::Meet};

}  // namespace

SVGPatternElement SVGPatternElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle
      .emplace<components::RenderingBehaviorComponent>(
          components::RenderingBehavior::ShadowOnlyChildren)
      .appliesTransform = false;
  handle.emplace<components::PatternComponent>();
  handle.emplace<components::DoNotInheritFillOrStrokeTag>();
  handle.emplace<components::ViewboxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>(kPatternDefaultPreserveAspectRatio);
  return SVGPatternElement(handle);
}

std::optional<Boxd> SVGPatternElement::viewbox() const {
  return handle_.get<components::ViewboxComponent>().viewbox;
}

PreserveAspectRatio SVGPatternElement::preserveAspectRatio() const {
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
}

Lengthd SVGPatternElement::x() const {
  return handle_.get<components::PatternComponent>().sizeProperties.x.getRequired();
}

Lengthd SVGPatternElement::y() const {
  return handle_.get<components::PatternComponent>().sizeProperties.y.getRequired();
}

std::optional<Lengthd> SVGPatternElement::width() const {
  return handle_.get<components::PatternComponent>().sizeProperties.width.get();
}

std::optional<Lengthd> SVGPatternElement::height() const {
  return handle_.get<components::PatternComponent>().sizeProperties.height.get();
}

PatternUnits SVGPatternElement::patternUnits() const {
  return handle_.get_or_emplace<components::PatternComponent>().patternUnits.value_or(
      PatternUnits::Default);
}

PatternContentUnits SVGPatternElement::patternContentUnits() const {
  return handle_.get_or_emplace<components::PatternComponent>().patternContentUnits.value_or(
      PatternContentUnits::Default);
}

Transformd SVGPatternElement::patternTransform() const {
  computeTransform();
  return handle_.get<components::ComputedLocalTransformComponent>().entityFromParent;
}

std::optional<RcString> SVGPatternElement::href() const {
  auto maybeHref = handle_.get_or_emplace<components::PatternComponent>().href;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

void SVGPatternElement::setViewbox(std::optional<Boxd> viewbox) {
  handle_.get<components::ViewboxComponent>().viewbox = viewbox;
}

void SVGPatternElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>().preserveAspectRatio =
      preserveAspectRatio;
}

void SVGPatternElement::setX(Lengthd value) {
  handle_.get<components::PatternComponent>().sizeProperties.x.set(value,
                                                                   css::Specificity::Override());
}

void SVGPatternElement::setY(Lengthd value) {
  handle_.get<components::PatternComponent>().sizeProperties.y.set(value,
                                                                   css::Specificity::Override());
}

void SVGPatternElement::setWidth(std::optional<Lengthd> value) {
  handle_.get<components::PatternComponent>().sizeProperties.width.set(
      value, css::Specificity::Override());
}

void SVGPatternElement::setHeight(std::optional<Lengthd> value) {
  handle_.get<components::PatternComponent>().sizeProperties.height.set(
      value, css::Specificity::Override());
}

void SVGPatternElement::setPatternUnits(PatternUnits value) {
  handle_.get_or_emplace<components::PatternComponent>().patternUnits = value;
}

void SVGPatternElement::setPatternContentUnits(PatternContentUnits value) {
  handle_.get_or_emplace<components::PatternComponent>().patternContentUnits = value;
}

void SVGPatternElement::setPatternTransform(Transformd transform) {
  auto& component = handle_.get_or_emplace<components::TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGPatternElement::setHref(const std::optional<RcStringOrRef>& value) {
  if (value) {
    handle_.get_or_emplace<components::PatternComponent>().href = RcString(value.value());
  } else {
    handle_.get_or_emplace<components::PatternComponent>().href = std::nullopt;
  }

  // Force the shadow tree to be regenerated.
  handle_.remove<components::ComputedShadowTreeComponent>();
}

void SVGPatternElement::invalidateTransform() {
  handle_.remove<components::ComputedLocalTransformComponent>();
}

void SVGPatternElement::computeTransform() const {
  components::LayoutSystem().createComputedLocalTransformComponentWithStyle(
      handle_, components::StyleSystem().computeStyle(handle_, nullptr), FontMetrics(), nullptr);
}

}  // namespace donner::svg
