#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/core/Pattern.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/registry/Registry.h"

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
  Transformd viewTransform;

  void resolveAndInheritAttributes(EntityHandle handle, EntityHandle base = EntityHandle());
};

}  // namespace donner::svg::components
