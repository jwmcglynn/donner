#include "donner/editor/ToolKeybinding.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {
namespace {

using ::testing::ElementsAre;

// Each tool maps to its Illustrator default single-key binding: Selection = V,
// Pen = P, Type = T. The toolbar tooltips and the keyboard shortcut handler both
// read this table, so pinning it keeps the on-screen "(V/P/T)" hints truthful.
TEST(ToolKeybinding, MapsEachToolToItsIllustratorKey) {
  EXPECT_EQ(KeybindingForTool(ToolId::Select).label, "Selection");
  EXPECT_EQ(KeybindingForTool(ToolId::Select).key, 'V');

  EXPECT_EQ(KeybindingForTool(ToolId::Pen).label, "Pen");
  EXPECT_EQ(KeybindingForTool(ToolId::Pen).key, 'P');

  EXPECT_EQ(KeybindingForTool(ToolId::Text).label, "Type");
  EXPECT_EQ(KeybindingForTool(ToolId::Text).key, 'T');
}

// The toolbar exposes Select, Pen, and Text — in that order — so the Text tool
// added in M4 has a dedicated button rather than being keyboard-only.
TEST(ToolKeybinding, ToolbarIncludesTextToolInOrder) {
  EXPECT_THAT(kToolbarTools, ElementsAre(ToolId::Select, ToolId::Pen, ToolId::Text));
}

}  // namespace
}  // namespace donner::editor
