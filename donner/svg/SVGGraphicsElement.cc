#include "donner/svg/SVGGraphicsElement.h"

#include "donner/svg/components/layout/LayoutSystem.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transformd SVGGraphicsElement::transform() const {
  return components::LayoutSystem().getEntityFromParentTransform(handle_);
}

void SVGGraphicsElement::setTransform(const Transformd& transform) {
  components::LayoutSystem().setEntityFromParentTransform(handle_, transform);
}

Transformd SVGGraphicsElement::elementFromWorld() const {
  return components::LayoutSystem().getEntityFromWorldTransform(handle_);
}

}  // namespace donner::svg
