#pragma once
/// @file
/// Gates a Metal test fixture's SetUp() on having created a device: skip on a developer machine,
/// fail on an automated lane. Every Metal device test in this package needs this, so it is a
/// shared macro rather than a hand copy in each fixture's preamble.

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/base/tests/ContinuousIntegrationMarkers.h"

namespace donner::gpu::metal::tests {

/**
 * The message a run reports when it could not create the GPU device its case needs.
 *
 * @param gateLabel What the gate is, for a reader who sees only this line.
 * @param disposition What the run is about to do.
 * @return The message, explaining when failing why a skip was not an option.
 */
inline std::string NoMetalDeviceMessage(
    std::string_view gateLabel, ::donner::tests::MissingRequirementDisposition disposition) {
  std::string message = "no GPU device could be created for ";
  message += gateLabel;
  message += ".";
  if (disposition == ::donner::tests::MissingRequirementDisposition::FailClosed) {
    message +=
        " Failing rather than skipping: this lane selected a target that needs a device, so a "
        "driver or runner that stopped providing one has disabled the gate, and a skip would "
        "report that as success.";
  }
  return message;
}

}  // namespace donner::gpu::metal::tests

/**
 * Gates a fixture's SetUp() on \p device having been created, ending the case when it was not:
 * skipped on a developer machine, failed on an automated lane.
 *
 * Use directly in SetUp(). Placed in a helper function, GTEST_SKIP()/FAIL() would return out of
 * the helper rather than SetUp(), and the fixture would go on to run its cases without a device.
 *
 * @param device Pointer-like value that is null when no device was created.
 * @param gateLabel What the gate is, for a reader who sees only this line.
 */
#define DONNER_REQUIRE_METAL_DEVICE(device, gateLabel)                                     \
  do {                                                                                     \
    if (!(device)) {                                                                       \
      const ::donner::tests::MissingRequirementDisposition donnerMetalDeviceDisposition =  \
          ::donner::tests::DispositionForMissingRequirement(                               \
              ::donner::tests::RunningUnderContinuousIntegration());                       \
      const std::string donnerMetalDeviceMessage =                                         \
          ::donner::gpu::metal::tests::NoMetalDeviceMessage((gateLabel),                   \
                                                            donnerMetalDeviceDisposition); \
      if (donnerMetalDeviceDisposition ==                                                  \
          ::donner::tests::MissingRequirementDisposition::FailClosed) {                    \
        FAIL() << donnerMetalDeviceMessage;                                                \
      }                                                                                    \
      GTEST_SKIP() << donnerMetalDeviceMessage;                                            \
    }                                                                                      \
  } while (false)
