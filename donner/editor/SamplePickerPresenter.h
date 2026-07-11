#pragma once
/// @file

#include <cstddef>
#include <string>
#include <string_view>

#include "donner/editor/EditorSampleCatalog.h"

struct ImVec2;

namespace donner::editor {

/// Minimum logical height used by every primary picker action.
inline constexpr float kSamplePickerMinTouchTarget = 44.0f;

/// Width below which the picker keeps its sample actions in one column.
inline constexpr float kSamplePickerNarrowBreakpoint = 640.0f;

/// Maximum number of columns and visible catalog entries in the bounded surface.
inline constexpr std::size_t kSamplePickerMaxColumns = 3;
inline constexpr std::size_t kSamplePickerMaxVisibleSamples = 8;

/// Public destination for the presenter's GitHub action. The presenter only
/// reports the action; the host decides whether and how to navigate to it.
inline constexpr std::string_view kSamplePickerGitHubUrl = "https://github.com/jwmcglynn/donner";

enum class SamplePickerLayoutMode {
  Narrow,
  Wide,
};

/// Pure geometry for the sample grid. Values are logical pixels in the pane.
struct SamplePickerLayout {
  SamplePickerLayoutMode mode = SamplePickerLayoutMode::Narrow;
  std::size_t columns = 1;
  std::size_t rows = 0;
  float cardWidth = 0.0f;
  float cardHeight = kSamplePickerMinTouchTarget;
};

/// Compute a bounded, touch-sized grid without requiring an ImGui context.
[[nodiscard]] SamplePickerLayout ComputeSamplePickerLayout(float availableWidth,
                                                           std::size_t sampleCount) noexcept;

/// Return the concise description used for a catalog sample card.
[[nodiscard]] std::string_view SamplePickerDescription(std::string_view sampleId) noexcept;

struct SamplePickerState {
  /// The host can hide the welcome surface while a document-specific surface is active.
  bool visible = true;
  /// Stable catalog ID of the currently selected sample, if any.
  std::string_view selectedSampleId;
};

/// Edge-triggered requests emitted by one rendered picker frame.
struct SamplePickerActions {
  bool dismiss = false;
  bool openFile = false;
  bool loadSample = false;
  std::string sampleId;
  bool openGitHub = false;
};

enum class SamplePickerCommand {
  Dismiss,
  OpenFile,
  LoadSample,
  OpenGitHub,
};

/// Apply a semantic picker command to an action accumulator.
///
/// This helper is deliberately independent of ImGui so command routing can be
/// tested and kept separate from the presenter's drawing code.
void ApplySamplePickerCommand(bool activated, SamplePickerCommand command,
                              std::string_view sampleId, SamplePickerActions* actions);

/// Draw the welcome/sample surface inside the current ImGui pane.
class SamplePickerPresenter {
public:
  /// Render using the current pane's available width and return edge-triggered actions.
  [[nodiscard]] SamplePickerActions render(const SamplePickerState& state) const;
};

}  // namespace donner::editor
