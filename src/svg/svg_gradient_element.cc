#include "src/svg/svg_gradient_element.h"

#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/gradient_component.h"
#include "src/svg/components/shadow_tree_component.h"

namespace donner::svg {

SVGGradientElement::SVGGradientElement(EntityHandle handle) : SVGElement(handle) {
  handle_.emplace<GradientComponent>();
}

std::optional<RcString> SVGGradientElement::href() const {
  auto maybeHref = handle_.get_or_emplace<GradientComponent>().href;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

GradientUnits SVGGradientElement::gradientUnits() const {
  return handle_.get_or_emplace<GradientComponent>().gradientUnits.value_or(GradientUnits::Default);
}

GradientSpreadMethod SVGGradientElement::spreadMethod() const {
  return handle_.get_or_emplace<GradientComponent>().spreadMethod.value_or(
      GradientSpreadMethod::Default);
}

void SVGGradientElement::setHref(std::optional<RcString> value) {
  handle_.get_or_emplace<GradientComponent>().href = value;
  // Force the shadow tree to be regenerated.
  handle_.remove<ComputedShadowTreeComponent>();
}

void SVGGradientElement::setGradientUnits(GradientUnits value) {
  handle_.get_or_emplace<GradientComponent>().gradientUnits = value;
}

void SVGGradientElement::setSpreadMethod(GradientSpreadMethod value) {
  handle_.get_or_emplace<GradientComponent>().spreadMethod = value;
}

}  // namespace donner::svg
