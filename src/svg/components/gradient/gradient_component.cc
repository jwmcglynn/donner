#include "src/svg/components/gradient/gradient_component.h"

#include "src/base/math_utils.h"
#include "src/svg/components/evaluated_reference_component.h"
#include "src/svg/components/gradient/linear_gradient_component.h"
#include "src/svg/components/gradient/radial_gradient_component.h"

namespace donner::svg::components {

// Resolve unspecified attributes to default values or inherit from the given base gradient element.
// This method is used to propagate attributes such as `x1`, `y1`, `cx`, `cy`, `r`, etc from the
// base element to the current element.
void ComputedGradientComponent::resolveAndInheritAttributes(EntityHandle handle,
                                                            EntityHandle base) {
  if (base) {
    if (auto* computedBase = base.try_get<ComputedGradientComponent>()) {
      gradientUnits = computedBase->gradientUnits;
      spreadMethod = computedBase->spreadMethod;
    }
  }

  // This lets <linearGradient> and <radialGradient> elements inherit shared attributes from each
  // other
  const GradientComponent& gradient = handle.get<GradientComponent>();
  if (gradient.gradientUnits) {
    gradientUnits = gradient.gradientUnits.value();
  }
  if (gradient.spreadMethod) {
    spreadMethod = gradient.spreadMethod.value();
  }

  // Inherit attributes from matching element types
  if (auto* linearGradient = handle.try_get<LinearGradientComponent>()) {
    linearGradient->inheritAttributes(handle, base);
  }

  if (auto* radialGradient = handle.try_get<RadialGradientComponent>()) {
    radialGradient->inheritAttributes(handle, base);
  }
}

}  // namespace donner::svg::components
