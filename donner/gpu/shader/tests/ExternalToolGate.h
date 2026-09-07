#pragma once
/// @file
/// What a suite that drives an external verification tool does when that tool is not installed.
///
/// This is for the tools a build cannot supply: the offline Metal compiler ships as a downloadable
/// Xcode component, so a machine without it is a real state. Skipping is right for a developer in
/// that state. On an automated lane it is not: gtest exits successfully when every case skipped, so
/// a lane that never had the tool reports the same green as a lane that compiled every module.

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/base/tests/ContinuousIntegrationMarkers.h"
#include "donner/gpu/baseline/FrozenBaselinePolicy.h"

namespace donner::gpu::shader {

/**
 * The message a run reports when the external tool it drives is unavailable.
 *
 * @param toolName Tool the suite drives, named the way a person would install it.
 * @param unavailableReason What the probe found.
 * @param laneMarker Marker that identified the lane, empty when there is none.
 * @param disposition What the run is about to do.
 * @return A message naming the tool and, when failing, the marker and why skipping was not an
 *   option.
 */
std::string MissingExternalToolMessage(std::string_view toolName,
                                       std::string_view unavailableReason,
                                       std::string_view laneMarker,
                                       baseline::MissingComparisonDisposition disposition);

}  // namespace donner::gpu::shader

/**
 * Gates a test case on an external verification tool, ending the case when it is unavailable:
 * skipped on a developer machine, failed on an automated lane.
 *
 * Use directly in a `TEST` body. Placed in a helper, the return ends the helper rather than the
 * case, and the case would go on to run without the tool.
 *
 * @param toolName Tool the suite drives, named the way a person would install it.
 * @param unavailableReason What the probe found, empty when the tool is usable.
 */
#define DONNER_REQUIRE_EXTERNAL_TOOL(toolName, unavailableReason)                               \
  do {                                                                                          \
    const std::string donnerToolReason = (unavailableReason);                                   \
    if (!donnerToolReason.empty()) {                                                            \
      const ::donner::gpu::baseline::MissingComparisonDisposition donnerToolDisposition =       \
          ::donner::gpu::baseline::DispositionForMissingAdapter(                                \
              ::donner::tests::RunningUnderContinuousIntegration());                            \
      const std::string donnerToolMessage = ::donner::gpu::shader::MissingExternalToolMessage(  \
          (toolName), donnerToolReason, ::donner::tests::FirstContinuousIntegrationMarkerSet(), \
          donnerToolDisposition);                                                               \
      if (donnerToolDisposition ==                                                              \
          ::donner::gpu::baseline::MissingComparisonDisposition::FailClosed) {                  \
        ADD_FAILURE() << donnerToolMessage;                                                     \
        return;                                                                                 \
      }                                                                                         \
      GTEST_SKIP() << donnerToolMessage;                                                        \
    }                                                                                           \
  } while (false)
