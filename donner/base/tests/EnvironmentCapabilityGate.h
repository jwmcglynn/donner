#pragma once
/// @file
/// What a test does when an environment capability it needs (a filesystem or sandbox operation,
/// not a build artifact or a piece of hardware) is unavailable: creating a symlink, for example,
/// on a filesystem or sandbox that forbids it.
///
/// That is not a property every machine has, so a developer machine in that state should skip
/// visibly and go on to the rest of the suite. An automated lane is not "some machine in that
/// state" though: it is where a case exercising a security protection (path-escape rejection
/// through a symlink, a save that refuses to follow one) is supposed to run on every commit, and
/// gtest reports a run whose every case skipped as a pass. A lane that never had the capability
/// reports the same green as a lane that exercised the protection, so on an automated lane this
/// fails closed instead, naming the capability and the marker that identified the lane.
///
/// Checks the same automated-lane markers as donner/gpu/baseline/FrozenBaselinePolicy.h, by
/// calling the single definition in donner/base/tests/ContinuousIntegrationMarkers.h rather than
/// keeping a copy: that policy delegates to the same functions.

#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <string_view>

#include "donner/base/tests/ContinuousIntegrationMarkers.h"

namespace donner::tests {

/// What a run should do when the environment capability it needs is unavailable.
enum class MissingEnvironmentCapabilityDisposition {
  Skip,        //!< Report the case as skipped, naming the capability that is unavailable.
  FailClosed,  //!< Fail: skipping here would silently retire the case that checks it.
};

/// Streams the disposition name. @param os Stream. @param disposition Value. @return `os`.
inline std::ostream& operator<<(std::ostream& os,
                                MissingEnvironmentCapabilityDisposition disposition) {
  switch (disposition) {
    case MissingEnvironmentCapabilityDisposition::Skip: return os << "Skip";
    case MissingEnvironmentCapabilityDisposition::FailClosed: return os << "FailClosed";
  }
  return os << "MissingEnvironmentCapabilityDisposition(unknown)";
}

/**
 * The disposition for a run whose needed environment capability is unavailable.
 *
 * @param underContinuousIntegration Whether this process is on an automated lane.
 * @return What the run should do.
 */
inline MissingEnvironmentCapabilityDisposition DispositionForMissingEnvironmentCapability(
    bool underContinuousIntegration) {
  return underContinuousIntegration ? MissingEnvironmentCapabilityDisposition::FailClosed
                                    : MissingEnvironmentCapabilityDisposition::Skip;
}

/**
 * The message a run reports when an environment capability it needs is unavailable.
 *
 * @param capabilityLabel What the capability is, for a reader who sees only this line.
 * @param unavailableReason What the probe found, for example a `std::error_code`'s message.
 * @param laneMarker Marker that identified the lane, empty when there is none.
 * @param disposition What the run is about to do.
 * @return A message naming the capability and, when failing, the marker and why skipping was not
 *   an option.
 */
inline std::string MissingEnvironmentCapabilityMessage(
    std::string_view capabilityLabel, std::string_view unavailableReason,
    std::string_view laneMarker, MissingEnvironmentCapabilityDisposition disposition) {
  std::string message(capabilityLabel);
  message += " is unavailable: ";
  message += unavailableReason;

  if (disposition == MissingEnvironmentCapabilityDisposition::FailClosed) {
    message +=
        "\nFailing rather than skipping: this lane selected a target whose case checks this "
        "capability, and gtest reports a run whose every case skipped as a pass, so skipping "
        "here would report success without exercising the protection it checks.";
    if (!laneMarker.empty()) {
      message += "\nAutomated lane identified by ";
      message += laneMarker;
      message += ".";
    }
  }

  return message;
}

}  // namespace donner::tests

/**
 * Gates a test case on an environment capability being available, ending the case when it is not:
 * skipped on a developer machine, failed on an automated lane.
 *
 * Use directly in a `TEST` or `TEST_F` body. Placed in a helper that returns a value, the skip or
 * fatal failure would end the helper rather than the case, and the case would go on to run
 * without the capability.
 *
 * @param condition An expression producing a value contextually convertible to `std::string`
 *   (matching `std::error_code::message()`), empty when the capability is available and, when
 *   not, the reason it is unavailable. Evaluated once.
 * @param capabilityLabel What the capability is, for a reader who sees only the failure or skip
 *   line.
 */
#define DONNER_REQUIRE_ENVIRONMENT_CAPABILITY(condition, capabilityLabel)         \
  do {                                                                            \
    const std::string donnerEnvironmentCapabilityReason = (condition);            \
    if (!donnerEnvironmentCapabilityReason.empty()) {                             \
      const ::donner::tests::MissingEnvironmentCapabilityDisposition              \
          donnerEnvironmentCapabilityDisposition =                                \
              ::donner::tests::DispositionForMissingEnvironmentCapability(        \
                  ::donner::tests::RunningUnderContinuousIntegration());          \
      const std::string donnerEnvironmentCapabilityMessage =                      \
          ::donner::tests::MissingEnvironmentCapabilityMessage(                   \
              (capabilityLabel), donnerEnvironmentCapabilityReason,               \
              ::donner::tests::FirstContinuousIntegrationMarkerSet(),             \
              donnerEnvironmentCapabilityDisposition);                            \
      if (donnerEnvironmentCapabilityDisposition ==                               \
          ::donner::tests::MissingEnvironmentCapabilityDisposition::FailClosed) { \
        FAIL() << donnerEnvironmentCapabilityMessage;                             \
      }                                                                           \
      GTEST_SKIP() << donnerEnvironmentCapabilityMessage;                         \
    }                                                                             \
  } while (false)
