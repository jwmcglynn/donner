#include "donner/svg/SVGFEOffsetElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEOffsetElement SVGFEOffsetElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEOffsetComponent>();
  return SVGFEOffsetElement(handle);
}

double SVGFEOffsetElement::dx() const {
  return handle_.get<components::FEOffsetComponent>().dx;
}

double SVGFEOffsetElement::dy() const {
  return handle_.get<components::FEOffsetComponent>().dy;
}

void SVGFEOffsetElement::setOffset(double dx, double dy) {
  auto& component = handle_.get<components::FEOffsetComponent>();
  component.dx = dx;
  component.dy = dy;
}

}  // namespace donner::svg
