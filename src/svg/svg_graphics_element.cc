#include "src/svg/svg_graphics_element.h"

#include "src/svg/components/style/style_system.h"
#include "src/svg/components/transform_component.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transformd SVGGraphicsElement::transform() const {
  computeTransform();
  return handle_.get<components::ComputedTransformComponent>().transform;
}

void SVGGraphicsElement::setTransform(Transformd transform) {
  auto& component = handle_.get_or_emplace<components::TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGGraphicsElement::invalidateTransform() {
  handle_.remove<components::ComputedTransformComponent>();
}

void SVGGraphicsElement::computeTransform() const {
  auto& transform = handle_.get_or_emplace<components::TransformComponent>();
  transform.computeWithPrecomputedStyle(
      handle_, components::StyleSystem().computeStyle(handle_, nullptr), FontMetrics(), nullptr);
}

}  // namespace donner::svg
