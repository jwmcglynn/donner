#pragma once
/// @file

#include <string>
#include <unordered_map>

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
};

}  // namespace donner::svg::components
