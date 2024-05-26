#include "src/svg/svg_fe_gaussian_blur_element.h"

#include "src/svg/svg_document.h"

namespace donner::svg {

SVGFEGaussianBlurElement SVGFEGaussianBlurElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  return SVGFEGaussianBlurElement(handle);
}

}  // namespace donner::svg
