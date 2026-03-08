#include "donner/svg/SVGFEImageElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEImageElement SVGFEImageElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEImageComponent>();
  return SVGFEImageElement(handle);
}

}  // namespace donner::svg
