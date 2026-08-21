#pragma once
/// @file
/// Side-effect controller for semantic actions emitted by the sample picker.

#include <functional>

#include "donner/editor/ExternalUrlLauncher.h"
#include "donner/editor/SamplePickerPresenter.h"

namespace donner::editor {

/// Function that opens one typed external destination.
using ExternalUrlLaunchFunction = std::function<bool(ExternalUrlTarget)>;

/// Applies sample-picker actions that cross the editor's platform boundary.
class SamplePickerController {
public:
  /// @param launchExternalUrl Platform URL launch port. Tests may provide a recording fake.
  explicit SamplePickerController(ExternalUrlLaunchFunction launchExternalUrl = LaunchExternalUrl);

  /// Apply external side effects requested by the presenter.
  ///
  /// @param actions Semantic actions emitted for the current frame.
  /// @return True when an external launch was requested and accepted.
  [[nodiscard]] bool applyExternalActions(const SamplePickerActions& actions) const;

private:
  ExternalUrlLaunchFunction launchExternalUrl_;
};

}  // namespace donner::editor
