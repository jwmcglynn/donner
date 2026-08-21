#include "donner/editor/SamplePickerController.h"

#include <utility>

namespace donner::editor {

SamplePickerController::SamplePickerController(ExternalUrlLaunchFunction launchExternalUrl)
    : launchExternalUrl_(std::move(launchExternalUrl)) {}

bool SamplePickerController::applyExternalActions(const SamplePickerActions& actions) const {
  if (!actions.openGitHub || !launchExternalUrl_) {
    return false;
  }

  return launchExternalUrl_(ExternalUrlTarget::DonnerRepository);
}

}  // namespace donner::editor
