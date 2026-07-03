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

// ---------------------------------------------------------------------------
// Focus-aware Select All / Deselect All routing.
//
// Cmd+A and Cmd+Shift+A each pick a branch by `sourcePaneFocused`:
//   - source pane focused  -> the *source-pane* policy fires (text Select-All / text Deselect),
//                             and the *canvas* policy does NOT;
//   - source pane unfocused -> the *canvas* policy fires (canvas Select-All / canvas clear),
//                             and the *source-pane* policy does NOT.
// Shift discriminates Select All (no Shift) from Deselect All (Shift) so the two never collide.
// ---------------------------------------------------------------------------

// Case 1: text-focused Cmd+A -> text Select-All branch, not canvas.
TEST(KeyboardShortcutPolicyTest, SelectAllTextFocusedRoutesToSourcePaneNotCanvas) {
  EXPECT_TRUE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                                 /*anyPopupOpen=*/false,
                                                 /*sourcePaneFocused=*/true));
  // The canvas Select-All must NOT also fire while the source pane owns focus.
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                              /*anyPopupOpen=*/false, /*sourcePaneFocused=*/true,
                                              /*canvasHasSelectableElements=*/true));
}

// Case 2: text-focused Cmd+Shift+A -> text Deselect branch, not canvas clear.
TEST(KeyboardShortcutPolicyTest, DeselectAllTextFocusedRoutesToSourcePaneNotCanvas) {
  EXPECT_TRUE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                   /*anyPopupOpen=*/false,
                                                   /*sourcePaneFocused=*/true));
  // The canvas clear must NOT also fire while the source pane owns focus, even if the canvas has a
  // selection.
  EXPECT_FALSE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                                /*sourcePaneFocused=*/true));
}

// Case 3: canvas-focused Cmd+A -> canvas Select-All branch, not text.
TEST(KeyboardShortcutPolicyTest, SelectAllCanvasFocusedRoutesToCanvasNotSourcePane) {
  EXPECT_TRUE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                             /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                             /*canvasHasSelectableElements=*/true));
  // The text Select-All must NOT fire when the source pane is not focused.
  EXPECT_FALSE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                                  /*anyPopupOpen=*/false,
                                                  /*sourcePaneFocused=*/false));
}

// Case 4: canvas-focused Cmd+Shift+A -> canvas clear branch, not text.
TEST(KeyboardShortcutPolicyTest, DeselectAllCanvasFocusedRoutesToCanvasNotSourcePane) {
  EXPECT_TRUE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                               /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                               /*sourcePaneFocused=*/false));
  // The text Deselect must NOT fire when the source pane is not focused.
  EXPECT_FALSE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                    /*anyPopupOpen=*/false,
                                                    /*sourcePaneFocused=*/false));
}

// Shift discriminates Select All from Deselect All in BOTH focus contexts, so the two chords never
// collide.
TEST(KeyboardShortcutPolicyTest, ShiftDiscriminatesSelectAllFromDeselectAll) {
  // Source pane focused: plain Cmd+A is Select-All only; Cmd+Shift+A is Deselect only.
  EXPECT_TRUE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                                 /*anyPopupOpen=*/false,
                                                 /*sourcePaneFocused=*/true));
  EXPECT_FALSE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true,
                                                    /*shift=*/false,
                                                    /*anyPopupOpen=*/false,
                                                    /*sourcePaneFocused=*/true));
  EXPECT_TRUE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                   /*anyPopupOpen=*/false,
                                                   /*sourcePaneFocused=*/true));
  EXPECT_FALSE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                  /*anyPopupOpen=*/false,
                                                  /*sourcePaneFocused=*/true));

  // Canvas focused: same Shift discrimination on the canvas branches.
  EXPECT_TRUE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                             /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                             /*canvasHasSelectableElements=*/true));
  EXPECT_FALSE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                                /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                                /*sourcePaneFocused=*/false));
  EXPECT_TRUE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                               /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                               /*sourcePaneFocused=*/false));
  EXPECT_FALSE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                              /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
                                              /*canvasHasSelectableElements=*/true));
}

TEST(KeyboardShortcutPolicyTest, DeselectAllCanvasRequiresCmdShiftAWithSelection) {
  // Cmd+Shift+A with a canvas selection and no popup/source focus fires.
  EXPECT_TRUE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                               /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                               /*sourcePaneFocused=*/false));

  // Shift+A without Cmd is not the chord.
  EXPECT_FALSE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/false, /*shift=*/true,
                                                /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                                /*sourcePaneFocused=*/false));

  // No-op when nothing is selected.
  EXPECT_FALSE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                /*hasSelection=*/false, /*anyPopupOpen=*/false,
                                                /*sourcePaneFocused=*/false));

  // Suppressed while a popup is open.
  EXPECT_FALSE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                /*hasSelection=*/true, /*anyPopupOpen=*/true,
                                                /*sourcePaneFocused=*/false));

  // Not pressing A does nothing.
  EXPECT_FALSE(CanDeselectAllFromCanvasShortcut(/*pressedA=*/false, /*cmd=*/true, /*shift=*/true,
                                                /*hasSelection=*/true, /*anyPopupOpen=*/false,
                                                /*sourcePaneFocused=*/false));
}

TEST(KeyboardShortcutPolicyTest, DeselectAllSourcePaneRequiresCmdShiftAWithFocus) {
  // Cmd+Shift+A with the source pane focused and no popup fires (no canvas selection needed —
  // collapsing the caret is always valid).
  EXPECT_TRUE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                   /*anyPopupOpen=*/false,
                                                   /*sourcePaneFocused=*/true));

  // Shift+A without Cmd is not the chord.
  EXPECT_FALSE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/false,
                                                    /*shift=*/true,
                                                    /*anyPopupOpen=*/false,
                                                    /*sourcePaneFocused=*/true));

  // Suppressed while a popup is open.
  EXPECT_FALSE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/true,
                                                    /*anyPopupOpen=*/true,
                                                    /*sourcePaneFocused=*/true));

  // Not pressing A does nothing.
  EXPECT_FALSE(CanDeselectAllFromSourcePaneShortcut(/*pressedA=*/false, /*cmd=*/true,
                                                    /*shift=*/true,
                                                    /*anyPopupOpen=*/false,
                                                    /*sourcePaneFocused=*/true));
}

TEST(KeyboardShortcutPolicyTest, SelectAllCanvasRequiresPlainCmdAWithSelectableElements) {
  // Plain Cmd+A on the canvas, with selectable elements and no popup/source focus, fires.
  EXPECT_TRUE(CanSelectAllFromCanvasShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                             /*anyPopupOpen=*/false, /*sourcePaneFocused=*/false,
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

TEST(KeyboardShortcutPolicyTest, SelectAllSourcePaneRequiresPlainCmdAWithFocus) {
  // Plain Cmd+A with the source pane focused and no popup fires.
  EXPECT_TRUE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                                 /*anyPopupOpen=*/false,
                                                 /*sourcePaneFocused=*/true));

  // Suppressed while a modal popup is open.
  EXPECT_FALSE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/true, /*shift=*/false,
                                                  /*anyPopupOpen=*/true,
                                                  /*sourcePaneFocused=*/true));

  // A without Cmd is not the chord.
  EXPECT_FALSE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/true, /*cmd=*/false, /*shift=*/false,
                                                  /*anyPopupOpen=*/false,
                                                  /*sourcePaneFocused=*/true));

  // Not pressing A does nothing.
  EXPECT_FALSE(CanSelectAllFromSourcePaneShortcut(/*pressedA=*/false, /*cmd=*/true, /*shift=*/false,
                                                  /*anyPopupOpen=*/false,
                                                  /*sourcePaneFocused=*/true));
}

}  // namespace
}  // namespace donner::editor
