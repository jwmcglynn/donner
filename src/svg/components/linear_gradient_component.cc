#include "src/svg/components/linear_gradient_component.h"

#include "src/svg/components/gradient_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

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

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::LinearGradient>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <radialGradient> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg
