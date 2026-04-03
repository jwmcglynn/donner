#include "donner/svg/SVGFETileElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFETileElement SVGFETileElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FETileComponent>();
  return SVGFETileElement(handle);
}

}  // namespace donner::svg
