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
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FEOffsetComponent>();
  return component ? component->dx : components::FEOffsetComponent().dx;
}

double SVGFEOffsetElement::dy() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FEOffsetComponent>();
  return component ? component->dy : components::FEOffsetComponent().dy;
}

void SVGFEOffsetElement::setOffset(double dx, double dy) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  auto& component = handle_.get_or_emplace<components::FEOffsetComponent>(access);
  component.dx = dx;
  component.dy = dy;
  invalidateFilter();
}

}  // namespace donner::svg
