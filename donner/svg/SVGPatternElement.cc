#include "donner/svg/SVGPatternElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg {

SVGPatternElement SVGPatternElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);

  auto& renderingBehavior = handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  renderingBehavior.inheritsParentTransform = false;
  renderingBehavior.appliesSelfTransform = false;

  handle.emplace<components::PatternComponent>();
  handle.emplace<components::DoNotInheritFillOrStrokeTag>();
  handle.emplace<components::ViewBoxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  return SVGPatternElement(handle);
}

std::optional<Boxd> SVGPatternElement::viewBox() const {
  return handle_.get<components::ViewBoxComponent>().viewBox;
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
  return components::LayoutSystem().getRawEntityFromParentTransform(handle_);
}

std::optional<RcString> SVGPatternElement::href() const {
  auto maybeHref = handle_.get_or_emplace<components::PatternComponent>().href;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

void SVGPatternElement::setViewBox(OptionalRef<Boxd> viewBox) {
  handle_.get<components::ViewBoxComponent>().viewBox = viewBox;
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

void SVGPatternElement::setWidth(OptionalRef<Lengthd> value) {
  handle_.get<components::PatternComponent>().sizeProperties.width.set(
      value, css::Specificity::Override());
}

void SVGPatternElement::setHeight(OptionalRef<Lengthd> value) {
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
  components::LayoutSystem().setRawEntityFromParentTransform(handle_, transform);
}

void SVGPatternElement::setHref(OptionalRef<RcStringOrRef> value) {
  if (value) {
    handle_.get_or_emplace<components::PatternComponent>().href = RcString(value.value());
  } else {
    handle_.get_or_emplace<components::PatternComponent>().href = std::nullopt;
  }

  // Force the shadow tree to be regenerated.
  handle_.remove<components::ComputedShadowTreeComponent>();
}

}  // namespace donner::svg
