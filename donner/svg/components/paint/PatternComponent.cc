#include "donner/svg/components/paint/PatternComponent.h"

#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

void ComputedPatternComponent::inheritAttributesFrom(EntityHandle handle, EntityHandle base) {
  const PatternComponent& pattern = handle.get<PatternComponent>();

  if (base) {
    if (auto* computedBase = base.try_get<ComputedPatternComponent>()) {
      patternUnits = computedBase->patternUnits;
      patternContentUnits = computedBase->patternContentUnits;
      tileRect = computedBase->tileRect;
      preserveAspectRatio = computedBase->preserveAspectRatio;
      viewBox = computedBase->viewBox;
      sizeProperties = computedBase->sizeProperties;
    }
  }

  if (pattern.patternUnits) {
    patternUnits = pattern.patternUnits.value();
  }
  if (pattern.patternContentUnits) {
    patternContentUnits = pattern.patternContentUnits.value();
  }
  if (pattern.sizeProperties.x.hasValue()) {
    sizeProperties.x = pattern.sizeProperties.x;
  }
  if (pattern.sizeProperties.y.hasValue()) {
    sizeProperties.y = pattern.sizeProperties.y;
  }
  if (pattern.sizeProperties.width.hasValue()) {
    sizeProperties.width = pattern.sizeProperties.width;
  }
  if (pattern.sizeProperties.height.hasValue()) {
    sizeProperties.height = pattern.sizeProperties.height;
  }
}

}  // namespace donner::svg::components

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Pattern>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // In SVG2, <pattern> still has normal attributes, not presentation attributes that can be
  // specified in CSS.
  return false;
}

}  // namespace donner::svg::parser
