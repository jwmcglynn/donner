#include "donner/svg/SVGGraphicsElement.h"

#include "donner/svg/components/TransformComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transformd SVGGraphicsElement::transform() const {
  computeTransform();
  return handle_.get<components::ComputedTransformComponent>().transform;
}

void SVGGraphicsElement::setTransform(const Transformd& transform) {
  auto& component = handle_.get_or_emplace<components::TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGGraphicsElement::invalidateTransform() {
  handle_.remove<components::ComputedTransformComponent>();
}

void SVGGraphicsElement::computeTransform() const {
  components::LayoutSystem().createComputedTransformComponentWithStyle(
      handle_, components::StyleSystem().computeStyle(handle_, nullptr), FontMetrics(), nullptr);
}

}  // namespace donner::svg
