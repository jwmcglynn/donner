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

/// What a run should do when it cannot perform the comparison it exists to perform.
enum class MissingComparisonDisposition {
  Skip,        //!< Report the case as skipped, leaving any artifact a person needs.
  FailClosed,  //!< Fail: skipping here would silently retire the gate.
};

/// Streams the disposition name. @param os Stream. @param disposition Value. @return `os`.
std::ostream& operator<<(std::ostream& os, MissingComparisonDisposition disposition);

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
 * Directory name the baselines for one adapter are filed under, for example `apple_m1_pro_metal`.
 * Lowercase, with every run of non-alphanumeric characters collapsed to one underscore, so one
 * adapter always resolves to one directory on every platform.
 *
 * This lives here rather than beside the capture library because both sides need it and only one
 * of them can link the capture library: the wgpu-backed capture writes these directories, and the
 * per-backend vertical slices, which deliberately carry no wgpu dependency, read them.
 *
 * @param adapterName Vendor and device string the driver reports.
 * @param adapterBackend Backend name, for example `Metal` or `Vulkan`.
 * @return The directory name, or `unknown_adapter` when the inputs carry no alphanumerics.
 */
std::string AdapterSlug(std::string_view adapterName, std::string_view adapterBackend);

/**
 * The disposition for a run that cannot create a GPU device at all.
 *
 * A missing device is not the same situation as a missing baseline, but it has the same
 * consequence: the comparison does not happen. On a developer machine without a working driver
 * that is a skip. On an automated lane it is a failure, because a lane selected this target and
 * then compared nothing, and a driver or runner regression that silently disables a gate is
 * indistinguishable from the gate passing.
 *
 * @param underContinuousIntegration Whether this process is on an automated lane.
 * @return What the run should do.
 */
MissingComparisonDisposition DispositionForMissingAdapter(bool underContinuousIntegration);

/**
 * The disposition for an adapter with no committed baseline.
 *
 * @param underContinuousIntegration Whether this process is on an automated lane.
 * @return What the run should do.
 */
MissingComparisonDisposition DispositionForUnbaselinedAdapter(bool underContinuousIntegration);

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
                                      MissingComparisonDisposition disposition);

/**
 * The message a run reports when it cannot create a GPU device.
 *
 * @param gateLabel What the gate is, for a reader who sees only this line.
 * @param disposition What the run is about to do.
 * @return A message naming the gate and, when failing, why skipping was not an option.
 */
std::string NoAdapterMessage(std::string_view gateLabel, MissingComparisonDisposition disposition);

}  // namespace donner::gpu::baseline
