#include "src/svg/svg_gradient_element.h"

#include "src/svg/components/paint/gradient_component.h"
#include "src/svg/components/shadow/computed_shadow_tree_component.h"
#include "src/svg/components/style/style_system.h"
#include "src/svg/components/transform_component.h"

namespace donner::svg {

namespace {

void computeTransform(EntityHandle handle) {
  auto& transform = handle.get_or_emplace<components::TransformComponent>();
  transform.computeWithPrecomputedStyle(
      handle, components::StyleSystem().computeStyle(handle, nullptr), FontMetrics(), nullptr);
}

}  // namespace

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
  computeTransform(handle_);
  return handle_.get<components::ComputedTransformComponent>().transform;
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
  auto& component = handle_.get_or_emplace<components::TransformComponent>();
  component.transform.set(CssTransform(value), css::Specificity::Override());
}

void SVGGradientElement::setSpreadMethod(GradientSpreadMethod value) {
  handle_.get_or_emplace<components::GradientComponent>().spreadMethod = value;
}

}  // namespace donner::svg
