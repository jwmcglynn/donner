#include "donner/svg/SVGGraphicsElement.h"

#include "donner/svg/components/layout/LayoutSystem.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transform2d SVGGraphicsElement::transform() const {
  return components::LayoutSystem().getRawEntityFromParentTransform(handle_);
}

void SVGGraphicsElement::setTransform(const Transform2d& transform) {
  components::LayoutSystem().setRawEntityFromParentTransform(handle_, transform);
}

Transform2d SVGGraphicsElement::elementFromWorld() const {
  return components::LayoutSystem().getEntityFromWorldTransform(handle_);
}

}  // namespace donner::svg
