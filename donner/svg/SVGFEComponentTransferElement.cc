#include "donner/svg/SVGFEComponentTransferElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEComponentTransferElement SVGFEComponentTransferElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEComponentTransferComponent>();
  return SVGFEComponentTransferElement(handle);
}

}  // namespace donner::svg
