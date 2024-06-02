#include "src/svg/svg_fe_gaussian_blur_element.h"

#include "src/svg/components/filter/filter_primitive_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGFEGaussianBlurElement SVGFEGaussianBlurElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::FEGaussianBlurComponent>();
  return SVGFEGaussianBlurElement(handle);
}

double SVGFEGaussianBlurElement::stdDeviationX() const {
  return handle_.get<components::FEGaussianBlurComponent>().stdDeviationX;
}

double SVGFEGaussianBlurElement::stdDeviationY() const {
  return handle_.get<components::FEGaussianBlurComponent>().stdDeviationY;
}

void SVGFEGaussianBlurElement::setStdDeviation(double valueX, double valueY) {
  auto& feComponent = handle_.get<components::FEGaussianBlurComponent>();
  feComponent.stdDeviationX = valueX;
  feComponent.stdDeviationY = valueY;
}

}  // namespace donner::svg
