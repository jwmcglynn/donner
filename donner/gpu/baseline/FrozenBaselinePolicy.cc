#include "donner/gpu/baseline/FrozenBaselinePolicy.h"

#include <cctype>

#include "donner/base/tests/ContinuousIntegrationMarkers.h"

namespace donner::gpu::baseline {

std::ostream& operator<<(std::ostream& os, MissingComparisonDisposition disposition) {
  switch (disposition) {
    case MissingComparisonDisposition::Skip: return os << "Skip";
    case MissingComparisonDisposition::FailClosed: return os << "FailClosed";
  }
  return os << "MissingComparisonDisposition(unknown)";
}

// Delegates to the single definition in donner/base/tests/ContinuousIntegrationMarkers.h so a
// marker dropped from the shared list is caught by that header's own tests as well as this
// policy's.
bool RunningUnderContinuousIntegration() {
  return ::donner::tests::RunningUnderContinuousIntegration();
}

MissingComparisonDisposition DispositionForUnbaselinedAdapter(bool underContinuousIntegration) {
  return underContinuousIntegration ? MissingComparisonDisposition::FailClosed
                                    : MissingComparisonDisposition::Skip;
}

MissingComparisonDisposition DispositionForMissingAdapter(bool underContinuousIntegration) {
  // Deliberately the same rule as the missing-baseline case: both end with nothing compared, and
  // a lane that reports success for either has retired the gate.
  return DispositionForUnbaselinedAdapter(underContinuousIntegration);
}

std::string UnbaselinedAdapterMessage(std::string_view adapterName, std::string_view adapterBackend,
                                      std::string_view slug, std::string_view capturedPath,
                                      std::string_view captureError,
                                      MissingComparisonDisposition disposition) {
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
  if (disposition == MissingComparisonDisposition::FailClosed) {
    message +=
        " Failing rather than skipping: on an automated lane a skip reports success while "
        "comparing nothing, so the pixel gate would silently stop running.";
  }
  return message;
}

std::string AdapterSlug(std::string_view adapterName, std::string_view adapterBackend) {
  std::string source(adapterName);
  source += ' ';
  source += adapterBackend;

  std::string slug;
  bool pendingSeparator = false;
  for (const char c : source) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      if (pendingSeparator && !slug.empty()) {
        slug += '_';
      }
      pendingSeparator = false;
      slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      pendingSeparator = true;
    }
  }
  return slug.empty() ? "unknown_adapter" : slug;
}

std::string NoAdapterMessage(std::string_view gateLabel, MissingComparisonDisposition disposition) {
  std::string message = "no GPU device could be created for ";
  message += gateLabel;
  message += ".";
  if (disposition == MissingComparisonDisposition::FailClosed) {
    message +=
        " Failing rather than skipping: this lane selected a target that needs a device, so a "
        "driver or runner that stopped providing one has disabled the gate, and a skip would "
        "report that as success.";
  }
  return message;
}

}  // namespace donner::gpu::baseline
