#include "donner/svg/SVGGraphicsElement.h"

#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/style/StyleSystem.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transformd SVGGraphicsElement::transform() const {
  computeTransform();
  return handle_.get<components::ComputedLocalTransformComponent>().transform;
}

void SVGGraphicsElement::setTransform(const Transformd& transform) {
  auto& component = handle_.get_or_emplace<components::TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGGraphicsElement::invalidateTransform() {
  handle_.remove<components::ComputedLocalTransformComponent>();
}

void SVGGraphicsElement::computeTransform() const {
  components::LayoutSystem().createComputedLocalTransformComponentWithStyle(
      handle_, components::StyleSystem().computeStyle(handle_, nullptr), FontMetrics(), nullptr);
}

}  // namespace donner::svg
