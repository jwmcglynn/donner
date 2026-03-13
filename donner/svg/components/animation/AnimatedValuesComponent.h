#pragma once
/// @file

#include <string>
#include <unordered_map>

namespace donner::svg::components {

/**
 * Component on target entities that holds animated attribute overrides.
 *
 * When an animation is active, its computed value is stored here. The style system checks this
 * map before falling back to base attribute values.
 *
 * For Phase 1 (`<set>` only), values are stored as raw strings that get re-parsed by the
 * style system as if they were attribute values.
 */
struct AnimatedValuesComponent {
  /// Map from attribute name to its animated value (as a raw string for Phase 1).
  std::unordered_map<std::string, std::string> overrides;
};

}  // namespace donner::svg::components
