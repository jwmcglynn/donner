#include "src/svg/components/radial_gradient_component.h"

#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

void RadialGradientComponent::inheritAttributes(EntityHandle handle, EntityHandle base) {
  handle.emplace_or_replace<ComputedRadialGradientComponent>().inheritAttributes(handle, base);
}

void ComputedRadialGradientComponent::inheritAttributes(EntityHandle handle, EntityHandle base) {
  if (base) {
    // Inherit from a ComputedRadialGradientComponent in the base, if it exists. The base may also
    // be a radial gradient, and shared properties for that are handled by
    // ComputedGradientComponent.
    if (const auto* computedBase = base.try_get<ComputedRadialGradientComponent>()) {
      *this = *computedBase;
    }
  }

  // Then override with the current entity.
  const auto& radialGradient = handle.get<RadialGradientComponent>();
  if (radialGradient.cx) {
    cx = radialGradient.cx.value();
  }
  if (radialGradient.cy) {
    cy = radialGradient.cy.value();
  }
  if (radialGradient.r) {
    r = radialGradient.r.value();
  }
  if (radialGradient.fx) {
    fx = radialGradient.fx.value();
  }
  if (radialGradient.fy) {
    fy = radialGradient.fy.value();
  }
  if (radialGradient.fr) {
    fr = radialGradient.fr.value();
  }
}

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::RadialGradient>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <radialGradient> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg
