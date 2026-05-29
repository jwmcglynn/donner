#include "donner/svg/components/paint/PatternComponent.h"

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/layout/SizedElementComponent.h"

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
  if (pattern.sizeProperties.x.isSpecified()) {
    sizeProperties.x = pattern.sizeProperties.x;
  }
  if (pattern.sizeProperties.y.isSpecified()) {
    sizeProperties.y = pattern.sizeProperties.y;
  }
  if (pattern.sizeProperties.width.isSpecified()) {
    sizeProperties.width = pattern.sizeProperties.width;
  }
  if (pattern.sizeProperties.height.isSpecified()) {
    sizeProperties.height = pattern.sizeProperties.height;
  }
}

}  // namespace donner::svg::components
