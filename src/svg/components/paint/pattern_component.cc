#include "src/svg/components/paint/pattern_component.h"

#include "src/svg/components/evaluated_reference_component.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/graph/recursion_guard.h"
#include "src/svg/properties/presentation_attribute_parsing.h"  // IWYU pragma: keep, defines ParsePresentationAttribute

namespace donner::svg::components {

void ComputedPatternComponent::resolveAndInheritAttributes(EntityHandle handle, EntityHandle base) {
  if (base) {
    if (auto* computedBase = base.try_get<ComputedPatternComponent>()) {
      patternUnits = computedBase->patternUnits;
      patternContentUnits = computedBase->patternContentUnits;
      tileRect = computedBase->tileRect;
    }
  }

  // TODO: Inherit viewbox, transform, preserveAspectRatio, x, y, width, and height.

  const PatternComponent& pattern = handle.get<PatternComponent>();
  if (pattern.patternUnits) {
    patternUnits = pattern.patternUnits.value();
  }
  if (pattern.patternContentUnits) {
    patternContentUnits = pattern.patternContentUnits.value();
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
