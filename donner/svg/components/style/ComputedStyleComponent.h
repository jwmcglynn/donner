#pragma once
/// @file

#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::svg::components {

struct ComputedStyleComponent {
  std::optional<PropertyRegistry> properties;
};

}  // namespace donner::svg::components
