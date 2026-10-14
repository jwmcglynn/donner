#pragma once
/// @file

#include <memory>
#include <string>
#include <unordered_map>

#include "donner/svg/components/animation/ContextualLengthRecipe.h"

namespace donner::svg::components {

/// Enables animated presentation values during the final style cascade pass.
struct AnimationStyleContext {
  bool enabled = false;  ///< Base-value sampling runs with this disabled.
};

/**
 * Component on target entities that holds animated attribute overrides.
 *
 * When an animation is active, its computed value is stored here. The style system checks this
 * map before falling back to base attribute values.
 *
 * Sampled values are stored as raw strings consumed by the style and presentation passes.
 * They remain separate from the authored XML attributes.
 */
struct AnimatedValuesComponent {
  /// Map from attribute name to its animated value (as a raw string).
  std::unordered_map<std::string, std::string> overrides;
  /// Immutable recipes for fields whose units depend on the final referencing instance.
  std::unordered_map<std::string, std::shared_ptr<const ContextualLengthRecipe>> contextualLengths;
};

}  // namespace donner::svg::components
