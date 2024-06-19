#include "donner/svg/SVGFEGaussianBlurElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

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
