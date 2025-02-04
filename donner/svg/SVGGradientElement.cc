#include "donner/svg/SVGGradientElement.h"

#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"

namespace donner::svg {

SVGGradientElement::SVGGradientElement(EntityHandle handle) : SVGElement(handle) {
  handle_.emplace<components::GradientComponent>();
}

std::optional<RcString> SVGGradientElement::href() const {
  auto maybeHref = handle_.get_or_emplace<components::GradientComponent>().href;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

GradientUnits SVGGradientElement::gradientUnits() const {
  return handle_.get_or_emplace<components::GradientComponent>().gradientUnits.value_or(
      GradientUnits::Default);
}

Transformd SVGGradientElement::gradientTransform() const {
  return components::LayoutSystem().getEntityFromParentTransform(handle_);
}

GradientSpreadMethod SVGGradientElement::spreadMethod() const {
  return handle_.get_or_emplace<components::GradientComponent>().spreadMethod.value_or(
      GradientSpreadMethod::Default);
}

void SVGGradientElement::setHref(const std::optional<RcString>& value) {
  handle_.get_or_emplace<components::GradientComponent>().href = value;
  // Force the shadow tree to be regenerated.
  handle_.remove<components::ComputedShadowTreeComponent>();
}

void SVGGradientElement::setGradientUnits(GradientUnits value) {
  handle_.get_or_emplace<components::GradientComponent>().gradientUnits = value;
}

void SVGGradientElement::setGradientTransform(const Transformd& value) {
  components::LayoutSystem().setEntityFromParentTransform(handle_, value);
}

void SVGGradientElement::setSpreadMethod(GradientSpreadMethod value) {
  handle_.get_or_emplace<components::GradientComponent>().spreadMethod = value;
}

}  // namespace donner::svg
