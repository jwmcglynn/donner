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

/// Cmd+Shift+A ("Deselect All") clears the canvas selection. The Shift requirement keeps it
/// distinct from a plain Cmd+A; it only acts on the canvas (not while the source pane owns keyboard
/// focus) and only when something is selected, so it is a no-op when the selection is already
/// empty.
[[nodiscard]] inline bool CanDeselectAllFromShortcut(bool pressedA, bool cmd, bool shift,
                                                     bool hasSelection, bool anyPopupOpen,
                                                     bool sourcePaneFocused) {
  return pressedA && cmd && shift && hasSelection && !anyPopupOpen && !sourcePaneFocused;
}

/// Plain Cmd+A ("Select All") selects every selectable element on the canvas. The *absence* of
/// Shift keeps it distinct from the Cmd+Shift+A "Deselect All" chord, so the two never collide. It
/// only acts on the canvas: when the source pane owns keyboard focus, Cmd+A keeps doing text
/// Select-All in the XML editor instead, so this returns false in that case. It is suppressed while
/// a modal popup is open, and is a no-op when the document has nothing selectable.
[[nodiscard]] inline bool CanSelectAllFromCanvasShortcut(bool pressedA, bool cmd, bool shift,
                                                         bool anyPopupOpen, bool sourcePaneFocused,
                                                         bool canvasHasSelectableElements) {
  return pressedA && cmd && !shift && canvasHasSelectableElements && !anyPopupOpen &&
         !sourcePaneFocused;
}

/// Source focus mode can be toggled globally with Cmd/Ctrl+Enter as long as no modal popup owns
/// keyboard input.
[[nodiscard]] inline bool CanToggleSourceFocusModeFromShortcut(bool enterKey, bool commandDown,
                                                               bool anyPopupOpen) {
  return enterKey && commandDown && !anyPopupOpen;
}

}  // namespace donner::editor
