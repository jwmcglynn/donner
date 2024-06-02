#pragma once
/// @file

#include "src/svg/properties/property_registry.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

struct ComputedStyleComponent {
  std::optional<PropertyRegistry> properties;
  std::optional<Boxd> viewbox;
};

}  // namespace donner::svg::components
