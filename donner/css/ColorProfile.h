#pragma once
/// @file

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "donner/css/Color.h"

namespace donner::css {

/**
 * Registry that resolves CSS color profile names to \ref ColorSpaceId values.
 *
 * Custom profile names registered through `@color-profile` are stored in lowercase so they can be
 * matched in a case-insensitive manner alongside the built-in SVG2 profile names.
 */
class ColorProfileRegistry {
public:
  /// Construct an empty registry.
  ColorProfileRegistry() = default;

  /// Register a profile alias.
  void registerProfile(std::string profileName, ColorSpaceId id);

  /// Resolve a profile name to a color space identifier.
  std::optional<ColorSpaceId> resolve(std::string_view profileName) const;

  /// Number of registered profiles.
  size_t size() const { return profiles_.size(); }

private:
  std::unordered_map<std::string, ColorSpaceId> profiles_;
};

}  // namespace donner::css
