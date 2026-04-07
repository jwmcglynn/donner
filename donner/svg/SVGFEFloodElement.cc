#include "donner/svg/SVGFEFloodElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEFloodElement SVGFEFloodElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEFloodComponent>();
  return SVGFEFloodElement(handle);
}

}  // namespace donner::svg
