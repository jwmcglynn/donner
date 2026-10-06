#pragma once
/// @file
/// The environment markers that identify an automated lane, and the questions every fail-closed
/// test gate in this repo needs answered from them: which marker is set, is any of them, and so
/// whether a case missing something it needs skips or fails.
///
/// This is the single definition. donner/base/tests/EnvironmentCapabilityGate.h, the Metal device
/// gate and the external-tool gate all call straight through to these.

#include <array>
#include <cstdlib>
#include <ostream>
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
    "DONNER_AUTOMATED_LANE",
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

/// What a run does when something its case needs (a GPU device, an external tool, an environment
/// capability) is unavailable.
enum class MissingRequirementDisposition {
  Skip,        //!< Report the case as skipped, naming what is unavailable.
  FailClosed,  //!< Fail: skipping here would silently retire the case that needs it.
};

/// Streams the disposition name. @param os Stream. @param disposition Value. @return `os`.
inline std::ostream& operator<<(std::ostream& os, MissingRequirementDisposition disposition) {
  switch (disposition) {
    case MissingRequirementDisposition::Skip: return os << "Skip";
    case MissingRequirementDisposition::FailClosed: return os << "FailClosed";
  }
  return os << "MissingRequirementDisposition(unknown)";
}

/**
 * The disposition for a run missing something its case needs: a developer machine skips, while
 * an automated lane fails, because gtest reports a run whose every case skipped as a pass.
 *
 * @param underContinuousIntegration Whether this process is on an automated lane.
 * @return What the run should do.
 */
inline MissingRequirementDisposition DispositionForMissingRequirement(
    bool underContinuousIntegration) {
  return underContinuousIntegration ? MissingRequirementDisposition::FailClosed
                                    : MissingRequirementDisposition::Skip;
}

}  // namespace donner::tests
