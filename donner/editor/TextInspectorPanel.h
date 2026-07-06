#pragma once
/// @file
///
/// `TextInspectorPanel` renders text-specific property controls in the editor
/// inspector area. It is shown only when the selection is exactly one
/// `<text>` element. Content edits are debounced into a single undoable
/// command (committed on focus loss or after a short idle), while the
/// style/position attributes are written through the SetAttribute mutation
/// seam.

#include <array>
#include <optional>

#include "donner/svg/SVGElement.h"

namespace donner::svg {
class FontCatalog;
}  // namespace donner::svg

namespace donner::editor {

class EditorApp;

/// Inspector section for a single selected `<text>` element.
class TextInspectorPanel {
public:
  /// Idle delay (seconds) after the last content keystroke before the pending
  /// content edit is committed as one undo entry.
  static constexpr double kContentCommitIdleSeconds = 0.5;

  /**
   * Render the Text section if the selection is exactly one `<text>` element.
   *
   * @param liveApp Live editor app for queuing mutations, or null while the
   *   renderer owns the document (renders nothing).
   * @param nowSeconds Monotonic time in seconds, used for content-edit
   *   debouncing. Pass `ImGui::GetTime()` from the shell.
   * @param fontCatalog Optional font catalog used to annotate the free-text
   *   font-family field with how it resolves (Embedded / System / Public Sans
   *   fallback). Pass null to skip the annotation.
   * @return true if a document mutation was queued this frame.
   */
  bool render(EditorApp* liveApp, double nowSeconds,
              const svg::FontCatalog* fontCatalog = nullptr);

private:
  /// Re-seed the local edit buffers from the currently-selected element.
  void syncBuffersFromSelection(const svg::SVGElement& text);

  /// Commit a pending debounced content edit, if any, as one SetTextContent
  /// command. Returns true if a command was queued.
  bool commitPendingContent(EditorApp& liveApp);

  /// Element the buffers were last synced from, so we re-seed on selection
  /// change instead of clobbering an in-progress edit.
  std::optional<svg::SVGElement> trackedElement_;

  std::array<char, 512> contentBuffer_{};
  std::array<char, 256> fontFamilyBuffer_{};
  std::array<char, 64> fontSizeBuffer_{};
  std::array<char, 64> strokeWidthBuffer_{};

  std::array<float, 4> fillColor_{0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 4> strokeColor_{0.0f, 0.0f, 0.0f, 1.0f};

  /// Pending (uncommitted) content edit state for debouncing.
  bool contentDirty_ = false;
  double contentLastEditSeconds_ = 0.0;
};

}  // namespace donner::editor
