#pragma once
/// @file

#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

struct ComputedStyleComponent {
  std::optional<PropertyRegistry> properties;
};

}  // namespace donner::svg::components
