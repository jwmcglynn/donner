#include "donner/svg/components/paint/LinearGradientComponent.h"

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

void LinearGradientComponent::inheritAttributes(EntityHandle handle, EntityHandle base) {
  handle.emplace_or_replace<ComputedLinearGradientComponent>().inheritAttributes(handle, base);
}

void ComputedLinearGradientComponent::inheritAttributes(EntityHandle handle, EntityHandle base) {
  if (base) {
    // Inherit from a ComputedLinearGradientComponent in the base, if it exists. The base may also
    // be a radial gradient, and shared properties for that are handled by
    // ComputedGradientComponent.
    if (const auto* computedBase = base.try_get<ComputedLinearGradientComponent>()) {
      *this = *computedBase;
    }
  }

  // Then override with the current entity.
  const auto& linearGradient = handle.get<LinearGradientComponent>();
  if (linearGradient.x1) {
    x1 = linearGradient.x1.value();
  }
  if (linearGradient.y1) {
    y1 = linearGradient.y1.value();
  }
  if (linearGradient.x2) {
    x2 = linearGradient.x2.value();
  }
  if (linearGradient.y2) {
    y2 = linearGradient.y2.value();
  }
}

}  // namespace donner::svg::components
