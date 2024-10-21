#include "donner/svg/SVGGElement.h"

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

SVGGElement SVGGElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  return SVGGElement(handle);
}

}  // namespace donner::svg
