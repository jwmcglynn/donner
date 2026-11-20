// @file
#pragma once

#include <vector>

#include "donner/base/EcsRegistry_fwd.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
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

/**
 * Whether the text content of \p handle paints no glyphs. It is hidden when its parent's content
 * is hidden, when its `display` is `none`, when its conditional processing attributes fail for
 * \p userLanguages, or when it is a `<textPath>` without a usable path or outside a text root.
 *
 * @param handle Text content element.
 * @param parentHidden Whether the content of \p handle's parent is hidden.
 * @param userLanguages User's preferred languages, in priority order.
 */
bool IsTextHidden(EntityHandle handle, bool parentHidden,
                  const std::vector<RcString>& userLanguages);

}  // namespace donner::svg::components
