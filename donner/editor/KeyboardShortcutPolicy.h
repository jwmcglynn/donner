#pragma once
/// @file

namespace donner::editor {

/// Canvas-level delete should fire whenever a selection exists, no modal is open, and the source
/// pane is not actively focused. `ImGuiIO::WantCaptureKeyboard` is too broad because non-text UI
/// panes also set it.
[[nodiscard]] inline bool CanDeleteSelectedElementsFromShortcut(bool deleteKey, bool hasSelection,
                                                                bool anyPopupOpen,
                                                                bool sourcePaneFocused) {
  return deleteKey && hasSelection && !anyPopupOpen && !sourcePaneFocused;
}

/// Source focus mode can be toggled globally with Cmd/Ctrl+Enter as long as no modal popup owns
/// keyboard input.
[[nodiscard]] inline bool CanToggleSourceFocusModeFromShortcut(bool enterKey, bool commandDown,
                                                               bool anyPopupOpen) {
  return enterKey && commandDown && !anyPopupOpen;
}

}  // namespace donner::editor
