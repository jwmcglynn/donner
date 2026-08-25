#pragma once
/// @file
/// What a run does when it is on an adapter that has no committed pixel baseline.
///
/// The two answers are not interchangeable. A person running the check locally on new hardware
/// wants the capture handed to them so they can commit it; an automated lane that quietly skips
/// instead is a gate that never runs, and a suite reporting success while asserting nothing is
/// worse than no suite at all. The decision lives here, apart from the GPU-bound test, so it is
/// exercised without a device on every platform.

#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace donner::gpu::baseline {

/// What a run should do when its adapter has no committed pixel baseline.
enum class UnbaselinedAdapterDisposition {
  CaptureAndSkip,  //!< Leave a capture for the committer and skip the comparison.
  FailClosed,      //!< Fail: skipping here would silently retire the gate.
};

/// Streams the disposition name. @param os Stream. @param disposition Value. @return `os`.
std::ostream& operator<<(std::ostream& os, UnbaselinedAdapterDisposition disposition);

/**
 * Environment variables whose presence marks an automated lane.
 *
 * `GITHUB_ACTIONS` is set by the hosted runner itself. The Donner-specific name lets any other
 * automated lane opt in without this list having to learn every runner's convention.
 *
 * @return The marker names, in a stable order.
 */
std::span<const std::string_view> ContinuousIntegrationMarkers();

/**
 * Whether any of `names` is set to a non-empty value in the process environment.
 *
 * @param names Environment variable names to check.
 * @return True when at least one is present and non-empty.
 */
bool AnyEnvironmentVariableIsSet(std::span<const std::string_view> names);

/// Whether this process is running on an automated lane. @return True on such a lane.
bool RunningUnderContinuousIntegration();

/**
 * The disposition for an adapter with no committed baseline.
 *
 * @param underContinuousIntegration Whether this process is on an automated lane.
 * @return What the run should do.
 */
UnbaselinedAdapterDisposition DispositionForUnbaselinedAdapter(bool underContinuousIntegration);

/**
 * The message a run reports when its adapter has no committed baseline.
 *
 * @param adapterName Live adapter name.
 * @param adapterBackend Live adapter backend.
 * @param slug Directory name the baseline for this adapter belongs in.
 * @param capturedPath Where this run left a capture, empty when none was written.
 * @param captureError Why the capture failed, empty on success.
 * @param disposition What the run is about to do.
 * @return A message naming what to commit and, when failing, why skipping was not an option.
 */
std::string UnbaselinedAdapterMessage(std::string_view adapterName, std::string_view adapterBackend,
                                      std::string_view slug, std::string_view capturedPath,
                                      std::string_view captureError,
                                      UnbaselinedAdapterDisposition disposition);

}  // namespace donner::gpu::baseline
