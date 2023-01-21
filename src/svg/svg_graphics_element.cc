#include "src/svg/svg_graphics_element.h"

#include "src/svg/components/transform_component.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transformd SVGGraphicsElement::transform() const {
  computeTransform();
  return handle_.get<ComputedTransformComponent>().transform;
}

void SVGGraphicsElement::setTransform(Transformd transform) {
  auto& component = handle_.get_or_emplace<TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGGraphicsElement::invalidateTransform() {
  handle_.remove<ComputedTransformComponent>();
}

void SVGGraphicsElement::computeTransform() const {
  auto& transform = handle_.get_or_emplace<TransformComponent>();
  transform.compute(handle_, FontMetrics());
}

}  // namespace donner::svg
