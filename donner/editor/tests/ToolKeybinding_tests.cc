#include "donner/editor/ToolKeybinding.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {
namespace {

using ::testing::ElementsAre;

// Each tool maps to the single-key binding design-tool users expect:
// Selection = V, Pen = P, Type = T, Eyedropper = I. The toolbar tooltips and the keyboard
// shortcut handler both read this table, so pinning it keeps the on-screen
// "(V/P/T)" hints truthful.
TEST(ToolKeybinding, MapsEachToolToItsStandardKey) {
  EXPECT_EQ(KeybindingForTool(ToolId::Select).label, "Selection");
  EXPECT_EQ(KeybindingForTool(ToolId::Select).key, 'V');

  EXPECT_EQ(KeybindingForTool(ToolId::Pen).label, "Pen");
  EXPECT_EQ(KeybindingForTool(ToolId::Pen).key, 'P');

  EXPECT_EQ(KeybindingForTool(ToolId::Text).label, "Type");
  EXPECT_EQ(KeybindingForTool(ToolId::Text).key, 'T');

  EXPECT_EQ(KeybindingForTool(ToolId::Eyedropper).label, "Eyedropper");
  EXPECT_EQ(KeybindingForTool(ToolId::Eyedropper).key, 'I');
}

TEST(ToolKeybinding, ToolbarIncludesEyedropperAfterText) {
  EXPECT_THAT(kToolbarTools,
              ElementsAre(ToolId::Select, ToolId::Pen, ToolId::Text, ToolId::Eyedropper));
}

}  // namespace
}  // namespace donner::editor
