#include "donner/svg/SVGFETurbulenceElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFETurbulenceElement SVGFETurbulenceElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FETurbulenceComponent>();
  return SVGFETurbulenceElement(handle);
}

}  // namespace donner::svg
