#pragma once
/// @file

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/EditorSampleCatalog.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

/// Minimum logical height used by every primary picker action.
inline constexpr float kSamplePickerMinTouchTarget = 44.0f;

/// Width below which the picker keeps its sample actions in one column.
inline constexpr float kSamplePickerNarrowBreakpoint = 640.0f;

/// Maximum number of columns and visible catalog entries in the bounded surface.
inline constexpr std::size_t kSamplePickerMaxColumns = 3;
/// Maximum number of sample cards displayed by one picker layout.
inline constexpr std::size_t kSamplePickerMaxVisibleSamples = 8;

/// Responsive layout selected from the available picker width.
enum class SamplePickerLayoutMode {
  Narrow,
  Wide,
};

/// Pure geometry for the sample grid. Values are logical pixels in the pane.
struct SamplePickerLayout {
  /// Responsive arrangement used for the sample cards.
  SamplePickerLayoutMode mode = SamplePickerLayoutMode::Narrow;
  /// Number of card columns.
  std::size_t columns = 1;
  /// Number of card rows.
  std::size_t rows = 0;
  /// Width of each sample card in logical UI pixels.
  float cardWidth = 0.0f;
  /// Height of each sample card in logical UI pixels.
  float cardHeight = kSamplePickerMinTouchTarget;
};

/// Compute a bounded, touch-sized grid without requiring an ImGui context.
[[nodiscard]] SamplePickerLayout ComputeSamplePickerLayout(float availableWidth,
                                                           std::size_t sampleCount) noexcept;

/// Return the concise description used for a catalog sample card.
[[nodiscard]] std::string_view SamplePickerDescription(std::string_view sampleId) noexcept;

/// Application state consumed when presenting the sample picker.
struct SamplePickerState {
  /// The host can hide the welcome surface while a document-specific surface is active.
  bool visible = true;
  /// Stable catalog ID of the currently selected sample, if any.
  std::string_view selectedSampleId;
};

/// Edge-triggered requests emitted by one rendered picker frame.
struct SamplePickerActions {
  /// Request dismissing the sample picker.
  bool dismiss = false;
  /// Request opening a document from disk.
  bool openFile = false;
  /// Request creating an empty document.
  bool newDocument = false;
  /// Request loading the sample named by sampleId.
  bool loadSample = false;
  /// Catalog identifier selected when loadSample is true.
  std::string sampleId;
  /// Request opening the project repository.
  bool openGitHub = false;
  /// Cards whose actual ImGui rectangles intersect the current clip region.
  std::vector<std::size_t> visibleSampleIndices;
};

/// Donner-rendered sample artwork uploaded to a texture the picker can blit.
struct SamplePickerThumbnail {
  /// ImGui texture identifier for the rendered thumbnail.
  ImTextureID texture = 0;
  /// Lower-right UV boundary of the valid thumbnail payload.
  Vector2d uvBottomRight = Vector2d(1.0, 1.0);
  /// Thumbnail width divided by height, used to preserve its proportions.
  float aspectRatio = 1.0f;
};

/// Resolve one catalog entry to its already-rendered thumbnail texture.
using SamplePickerThumbnailProvider =
    std::function<SamplePickerThumbnail(const EditorSample& sample, std::size_t index)>;

/// Actions exposed by the sample picker controls.
enum class SamplePickerCommand {
  Dismiss,
  OpenFile,
  NewDocument,
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
  [[nodiscard]] SamplePickerActions render(
      const SamplePickerState& state,
      const SamplePickerThumbnailProvider& thumbnailProvider = {}) const;
};

}  // namespace donner::editor
