#include "donner/svg/SVGGraphicsElement.h"

#include "donner/svg/components/layout/LayoutSystem.h"

namespace donner::svg {

SVGGraphicsElement::SVGGraphicsElement(EntityHandle handle) : SVGElement(handle) {}

Transform2d SVGGraphicsElement::transform() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  return components::LayoutSystem().getRawEntityFromParentTransform(handle_);
}

void SVGGraphicsElement::setTransform(const Transform2d& transform) {
  DocumentWriteAccess access = handle_.writeAccess();
  components::LayoutSystem().setRawEntityFromParentTransform(handle_, transform);
  access.bumpMutationRevision();
}

Transform2d SVGGraphicsElement::elementFromWorld() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  return components::LayoutSystem().getEntityFromWorldTransform(handle_);
}

}  // namespace donner::svg
