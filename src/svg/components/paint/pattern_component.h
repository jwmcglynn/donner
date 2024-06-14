#pragma once
/// @file

#include "src/svg/components/layout/sized_element_component.h"
#include "src/svg/core/pattern.h"
#include "src/svg/graph/reference.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

/**
 * Parameters for `<pattern>` elements which are not captured by \ref ViewBoxComponent and \ref
 * SizedElementComponent.
 */
struct PatternComponent {
  std::optional<PatternUnits> patternUnits;
  std::optional<PatternContentUnits> patternContentUnits;
  std::optional<Reference> href;
  SizedElementProperties sizeProperties;
};

struct ComputedPatternComponent {
  bool initialized = false;
  PatternUnits patternUnits = PatternUnits::Default;
  PatternContentUnits patternContentUnits = PatternContentUnits::Default;
  Boxd tileRect = Boxd::CreateEmpty(Vector2d());

  void resolveAndInheritAttributes(EntityHandle handle, EntityHandle base = EntityHandle());
};

}  // namespace donner::svg::components
