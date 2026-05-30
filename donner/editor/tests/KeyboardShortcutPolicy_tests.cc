#include "donner/editor/KeyboardShortcutPolicy.h"

#include <gtest/gtest.h>

namespace donner::editor {
namespace {

TEST(KeyboardShortcutPolicyTest, DeleteAllowedWhenCanvasOwnsShortcut) {
  EXPECT_TRUE(CanDeleteSelectedElementsFromShortcut(
      /*deleteKey=*/true, /*hasSelection=*/true, /*anyPopupOpen=*/false,
      /*sourcePaneFocused=*/false));
}

TEST(KeyboardShortcutPolicyTest, DeleteBlockedBySourcePaneFocus) {
  EXPECT_FALSE(CanDeleteSelectedElementsFromShortcut(
      /*deleteKey=*/true, /*hasSelection=*/true, /*anyPopupOpen=*/false,
      /*sourcePaneFocused=*/true));
}

TEST(KeyboardShortcutPolicyTest, DeleteBlockedWithoutSelectionOrWithPopup) {
  EXPECT_FALSE(CanDeleteSelectedElementsFromShortcut(
      /*deleteKey=*/true, /*hasSelection=*/false, /*anyPopupOpen=*/false,
      /*sourcePaneFocused=*/false));
  EXPECT_FALSE(CanDeleteSelectedElementsFromShortcut(
      /*deleteKey=*/true, /*hasSelection=*/true, /*anyPopupOpen=*/true,
      /*sourcePaneFocused=*/false));
  EXPECT_FALSE(CanDeleteSelectedElementsFromShortcut(
      /*deleteKey=*/false, /*hasSelection=*/true, /*anyPopupOpen=*/false,
      /*sourcePaneFocused=*/false));
}

TEST(KeyboardShortcutPolicyTest, SourceFocusToggleRequiresCommandEnterWithoutPopup) {
  EXPECT_TRUE(CanToggleSourceFocusModeFromShortcut(
      /*enterKey=*/true, /*commandDown=*/true, /*anyPopupOpen=*/false));
  EXPECT_FALSE(CanToggleSourceFocusModeFromShortcut(
      /*enterKey=*/false, /*commandDown=*/true, /*anyPopupOpen=*/false));
  EXPECT_FALSE(CanToggleSourceFocusModeFromShortcut(
      /*enterKey=*/true, /*commandDown=*/false, /*anyPopupOpen=*/false));
  EXPECT_FALSE(CanToggleSourceFocusModeFromShortcut(
      /*enterKey=*/true, /*commandDown=*/true, /*anyPopupOpen=*/true));
}

TEST(KeyboardShortcutPolicyTest, DeselectAllRequiresCmdShiftAWithSelectionOnCanvas) {
  // Cmd+Shift+A with a canvas selection and no popup/source focus fires.
  EXPECT_TRUE(CanDeselectAllFromShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                         /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                         /*sourcePaneFocused=*/false));

  // Plain Cmd+A (no Shift) must NOT trigger Deselect All — that is Select All.
  EXPECT_FALSE(CanDeselectAllFromShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                          /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                          /*sourcePaneFocused=*/false));

  // Shift+A without Cmd is not the chord.
  EXPECT_FALSE(CanDeselectAllFromShortcut(/*pressedA=*/true, /*cmd=*/false, /*shift=*/true,
                                          /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                          /*sourcePaneFocused=*/false));

  // No-op when nothing is selected.
  EXPECT_FALSE(CanDeselectAllFromShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                          /*hasSelection=*/false, /*anyPopupOpen=*/false,
                                          /*sourcePaneFocused=*/false));

  // Suppressed while a popup is open or the source pane owns keyboard focus.
  EXPECT_FALSE(CanDeselectAllFromShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                          /*hasSelection=*/true, /*anyPopupOpen=*/true,
                                          /*sourcePaneFocused=*/false));
  EXPECT_FALSE(CanDeselectAllFromShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                          /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                          /*sourcePaneFocused=*/true));

  // Not pressing A does nothing.
  EXPECT_FALSE(CanDeselectAllFromShortcut(/*pressedA=*/false, /*cmd=*/true, /*shift=*/true,
                                          /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                          /*sourcePaneFocused=*/false));
}

TEST(KeyboardShortcutPolicyTest, SelectAllRequiresPlainCmdAOnCanvasWithSelectableElements) {
  // Plain Cmd+A on the canvas, with selectable elements and no popup/source focus, fires.
  EXPECT_TRUE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                             /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                             /*canvasHasSelectableElements=*/true));

  // Cmd+Shift+A is "Deselect All", not Select All — the Shift must suppress this.
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                              /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                              /*canvasHasSelectableElements=*/true));

  // When the source pane owns keyboard focus, Cmd+A is text Select-All in the editor, not canvas
  // Select-All.
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                              /*anyPopupOpen=*/false, /*sourcePaneFocused=*/true,
                                              /*canvasHasSelectableElements=*/true));

  // Suppressed while a modal popup is open.
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                              /*anyPopupOpen=*/true, /*sourcePaneFocused=*/false,
                                              /*canvasHasSelectableElements=*/true));

  // No-op when the document has nothing selectable.
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                              /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                              /*canvasHasSelectableElements=*/false));

  // A without Cmd is not the chord.
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/false, /*shift=*/false,
                                              /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                              /*canvasHasSelectableElements=*/true));

  // Not pressing A does nothing.
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/false, /*cmd=*/true, /*shift=*/false,
                                              /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                              /*canvasHasSelectableElements=*/true));
}

}  // namespace
}  // namespace donner::editor
