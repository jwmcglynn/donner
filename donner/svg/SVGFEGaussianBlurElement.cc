#include "donner/svg/SVGFEGaussianBlurElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"

namespace donner::svg {

SVGFEGaussianBlurElement SVGFEGaussianBlurElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::FEGaussianBlurComponent>();
  return SVGFEGaussianBlurElement(handle);
}

double SVGFEGaussianBlurElement::stdDeviationX() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FEGaussianBlurComponent>();
  return component ? component->stdDeviationX : components::FEGaussianBlurComponent().stdDeviationX;
}

double SVGFEGaussianBlurElement::stdDeviationY() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FEGaussianBlurComponent>();
  return component ? component->stdDeviationY : components::FEGaussianBlurComponent().stdDeviationY;
}

void SVGFEGaussianBlurElement::setStdDeviation(double valueX, double valueY) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  auto& feComponent = handle_.get_or_emplace<components::FEGaussianBlurComponent>(access);
  feComponent.stdDeviationX = valueX;
  feComponent.stdDeviationY = valueY;
  invalidateFilter();
}

}  // namespace donner::svg
