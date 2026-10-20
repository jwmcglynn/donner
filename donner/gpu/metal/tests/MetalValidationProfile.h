#pragma once
/// @file
/// Classifies observed native Metal shader-validation capability.

#include <cstdint>
#include <iosfwd>
#include <string>

namespace donner::gpu::metal::tests {

/// Validation profiles recognized by the native capability probe.
enum class MetalValidationProfile { Unsupported, Full, ParavirtualTextureUsageOff };

/// Prints the fixed machine-readable name of a validation profile.
/// @param os Destination stream.
/// @param profile Profile to describe.
std::ostream& operator<<(std::ostream& os, MetalValidationProfile profile);

/// Runtime observations used to recognize a validated profile.
struct MetalValidationObservation {
  std::string gpuName;
  std::int64_t osMajor = 0;
  std::int64_t osMinor = 0;
  std::int64_t osPatch = 0;
  std::string osBuild;
  /// Raw MTLShaderValidation value: Default=0, Enabled=1, Disabled=2.
  std::int64_t shaderValidationState = 0;
};

/// Recognizes full validation or the exact validated texture-usage exception.
/// @param observation Observed device, OS, and actual pipeline validation state.
/// @return Unsupported for every unrecognized disabled or unknown validation state.
MetalValidationProfile ClassifyMetalValidationProfile(
    const MetalValidationObservation& observation);

/// Describes an observation for a failure log, so an unrecognized device, OS or validation state
/// can be requalified from the log alone. The device name keeps only printable ASCII other than
/// quotes and backslashes.
/// @param observation Observed device, OS, and actual pipeline validation state.
/// @return `gpu="<name>" os=<major>.<minor>.<patch> os_build=<build> validation_state=<raw>`.
std::string DescribeMetalValidationObservation(const MetalValidationObservation& observation);

}  // namespace donner::gpu::metal::tests
