#include "donner/svg/SVGUnknownElement.h"

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

SVGUnknownElement SVGUnknownElement::CreateOn(EntityHandle handle,
                                              const xml::XMLQualifiedNameRef& tagName) {
  CreateEntityOn(handle, tagName, Type);
  return SVGUnknownElement(handle);
}

}  // namespace donner::svg
