#include "donner/svg/SVGGraphicsElement.h"

#include "donner/svg/components/layout/LayoutSystem.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transformd SVGGraphicsElement::transform() const {
  return components::LayoutSystem().getEntityFromParentTranform(handle_);
}

void SVGGraphicsElement::setTransform(const Transformd& transform) {
  components::LayoutSystem().setEntityFromParentTransform(handle_, transform);
}

}  // namespace donner::svg
