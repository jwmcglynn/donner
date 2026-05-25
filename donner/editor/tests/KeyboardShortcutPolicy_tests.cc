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

}  // namespace
}  // namespace donner::editor
