#pragma once
/// @file

#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

class StyleSystem {
public:
  const ComputedStyleComponent& computeProperties(EntityHandle handle);

  void applyStyleToLayout(EntityHandle handle);

private:
  void computePropertiesInto(EntityHandle handle, ComputedStyleComponent& computedStyle);
};

}  // namespace donner::svg::components
