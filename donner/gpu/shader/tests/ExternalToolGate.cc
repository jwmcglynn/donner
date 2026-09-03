#include "donner/gpu/shader/tests/ExternalToolGate.h"

namespace donner::gpu::shader {

baseline::MissingComparisonDisposition DispositionForMissingExternalTool(
    bool underContinuousIntegration) {
  return baseline::DispositionForMissingAdapter(underContinuousIntegration);
}

std::string MissingExternalToolMessage(std::string_view toolName,
                                       std::string_view unavailableReason,
                                       std::string_view laneMarker,
                                       baseline::MissingComparisonDisposition disposition) {
  std::string message(toolName);
  message += " is unavailable: ";
  message += unavailableReason;

  if (disposition == baseline::MissingComparisonDisposition::FailClosed) {
    message +=
        "\nFailing rather than skipping: this lane selected the target, and gtest reports a "
        "run whose every case skipped as a pass, so skipping here would report success "
        "without validating anything. Install ";
    message += toolName;
    message += " on the lane, or exclude the target from it explicitly.";
    if (!laneMarker.empty()) {
      message += "\nAutomated lane identified by ";
      message += laneMarker;
      message += ".";
    }
  }

  return message;
}

}  // namespace donner::gpu::shader
