#include "donner/svg/SVGSwitchElement.h"

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

SVGSwitchElement SVGSwitchElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  return SVGSwitchElement(handle);
}

}  // namespace donner::svg
