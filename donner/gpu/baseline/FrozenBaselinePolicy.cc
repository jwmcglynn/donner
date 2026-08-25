#include "donner/gpu/baseline/FrozenBaselinePolicy.h"

#include <array>
#include <cstdlib>

namespace donner::gpu::baseline {
namespace {

constexpr std::array<std::string_view, 2> kContinuousIntegrationMarkers = {
    "GITHUB_ACTIONS",
    "DONNER_BASELINE_REQUIRE_FROZEN_ADAPTER",
};

}  // namespace

std::ostream& operator<<(std::ostream& os, UnbaselinedAdapterDisposition disposition) {
  switch (disposition) {
    case UnbaselinedAdapterDisposition::CaptureAndSkip: return os << "CaptureAndSkip";
    case UnbaselinedAdapterDisposition::FailClosed: return os << "FailClosed";
  }
  return os << "UnbaselinedAdapterDisposition(unknown)";
}

std::span<const std::string_view> ContinuousIntegrationMarkers() {
  return kContinuousIntegrationMarkers;
}

bool AnyEnvironmentVariableIsSet(std::span<const std::string_view> names) {
  for (const std::string_view name : names) {
    const char* value = std::getenv(std::string(name).c_str());
    if (value != nullptr && value[0] != '\0') {
      return true;
    }
  }
  return false;
}

bool RunningUnderContinuousIntegration() {
  return AnyEnvironmentVariableIsSet(ContinuousIntegrationMarkers());
}

UnbaselinedAdapterDisposition DispositionForUnbaselinedAdapter(
    bool /*underContinuousIntegration*/) {
  return UnbaselinedAdapterDisposition::CaptureAndSkip;
}

std::string UnbaselinedAdapterMessage(std::string_view adapterName, std::string_view adapterBackend,
                                      std::string_view slug, std::string_view capturedPath,
                                      std::string_view captureError,
                                      UnbaselinedAdapterDisposition disposition) {
  std::string message = "no frozen pixel baseline for ";
  message += adapterName;
  message += " (";
  message += adapterBackend;
  message += "). ";
  if (captureError.empty()) {
    message += "This run captured one at ";
    message += capturedPath;
    message += "; commit it under donner/gpu/baseline/baselines/";
    message += slug;
    message += "/ with the revision it was captured at, and this environment starts gating.";
  } else {
    message += "Capturing one here also failed: ";
    message += captureError;
  }
  if (disposition == UnbaselinedAdapterDisposition::FailClosed) {
    message +=
        " Failing rather than skipping: on an automated lane a skip reports success while "
        "comparing nothing, so the pixel gate would silently stop running.";
  }
  return message;
}

}  // namespace donner::gpu::baseline
