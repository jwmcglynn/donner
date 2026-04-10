// @file
#pragma once

#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"

namespace donner::svg::components {

/**
 * System to compute text layout spans from raw text and positioning attributes.
 *
 * @ingroup ecs_systems
 */
class TextSystem {
public:
  /**
   * Instantiate computed text spans for all entities with \ref TextComponent.
   *
   * @param registry The registry to instantiate the computed text spans for.
   * @param warningSink If non-null, a vector to store any warnings that occur during the
   * instantiation.
   */
  void instantiateAllComputedComponents(Registry& registry, ParseWarningSink& warningSink);

  /**
   * Instantiate computed text spans for a specific text root.
   *
   * @param handle Text root handle.
   * @param warningSink If non-null, warnings are appended here.
   */
  void instantiateComputedComponent(EntityHandle handle, ParseWarningSink& warningSink);
};

}  // namespace donner::svg::components
