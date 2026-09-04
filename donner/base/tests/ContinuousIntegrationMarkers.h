#pragma once
/// @file
/// The environment markers that identify an automated lane, and the two questions every
/// fail-closed test gate in this repo needs answered from them: which marker is set, and is any
/// of them.
///
/// This is the single definition. donner/gpu/baseline/FrozenBaselinePolicy.{h,cc} delegates its
/// same-named public functions here rather than keeping its own copy of the list, and
/// donner/base/tests/EnvironmentCapabilityGate.h calls straight through to these.

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

namespace donner::tests {

/**
 * The environment variables whose presence marks an automated lane, in the order checked.
 *
 * `GITHUB_ACTIONS` is set by the hosted runner itself. The Donner-specific name lets any other
 * automated lane opt in without this list having to learn every runner's convention.
 */
inline constexpr std::array<std::string_view, 2> kContinuousIntegrationMarkers = {
    "GITHUB_ACTIONS",
    "DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER",
};

/**
 * The name of the environment marker that identified this process as running on an automated
 * lane, for a message that has to say which one did.
 *
 * @return The marker's name, aliasing static storage that outlives every caller, or an empty view
 *   when no marker is set.
 */
inline std::string_view FirstContinuousIntegrationMarkerSet() {
  for (const std::string_view name : kContinuousIntegrationMarkers) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value != nullptr && value[0] != '\0') {
      return name;
    }
  }
  return {};
}

/// Whether this process is running on an automated lane. @return True on such a lane.
inline bool RunningUnderContinuousIntegration() {
  return !FirstContinuousIntegrationMarkerSet().empty();
}

}  // namespace donner::tests
