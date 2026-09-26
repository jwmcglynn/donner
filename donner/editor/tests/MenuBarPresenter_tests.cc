#include "donner/editor/MenuBarPresenter.h"

#include <gtest/gtest.h>

#include <set>
#include <string_view>
#include <vector>

#include "donner/editor/EditorMenuModel.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
namespace {

void CollectCommands(std::span<const EditorMenuNode> nodes,
                     std::vector<const EditorMenuNode*>* commands) {
  for (const EditorMenuNode& node : nodes) {
    if (node.kind == EditorMenuNode::Kind::Command) {
      commands->push_back(&node);
    } else if (node.kind == EditorMenuNode::Kind::Submenu) {
      CollectCommands(node.children, commands);
    }
  }
}

const EditorMenuNode* FindCommand(MenuBarCommand command) {
  std::vector<const EditorMenuNode*> commands;
  for (const EditorMenu& menu : EditorMenuModel()) {
    CollectCommands(menu.items, &commands);
  }
  for (const EditorMenuNode* node : commands) {
    if (node->command == command) {
      return node;
    }
  }
  return nullptr;
}

TEST(EditorMenuModelTest, EverySemanticCommandAppearsOnceWithDistinctAccelerator) {
  const std::span<const EditorMenu> menus = EditorMenuModel();
  ASSERT_EQ(menus.size(), 4u);
  EXPECT_EQ(menus[0].title, "DONNER");
  EXPECT_EQ(menus[1].title, "File");
  EXPECT_EQ(menus[2].title, "Edit");
  EXPECT_EQ(menus[3].title, "View");

  std::vector<const EditorMenuNode*> commands;
  for (const EditorMenu& menu : menus) {
    CollectCommands(menu.items, &commands);
  }
  std::set<int> commandIds;
  std::set<std::string_view> shortcuts;
  for (const EditorMenuNode* node : commands) {
    EXPECT_FALSE(node->label.empty());
    EXPECT_TRUE(commandIds.insert(static_cast<int>(node->command)).second) << node->label;
    if (!node->shortcut.empty()) {
      EXPECT_TRUE(shortcuts.insert(node->shortcut).second) << node->shortcut;
    }
  }
  EXPECT_EQ(commandIds.size(), static_cast<std::size_t>(MenuBarCommand::ResetLayout) + 1u);
  const EditorMenuNode* saveAs = FindCommand(MenuBarCommand::SaveFileAs);
  const EditorMenuNode* sourceFocus = FindCommand(MenuBarCommand::ToggleSourceFocusMode);
  ASSERT_NE(saveAs, nullptr);
  ASSERT_NE(sourceFocus, nullptr);
  EXPECT_EQ(saveAs->shortcut, "Cmd+Shift+S");
  EXPECT_EQ(sourceFocus->shortcut, "Cmd+Enter");
}

TEST(EditorMenuModelTest, EnabledAndCheckStateFollowFocusAndViewState) {
  MenuBarState state;
  const EditorMenuNode* copy = FindCommand(MenuBarCommand::Copy);
  const EditorMenuNode* selectAll = FindCommand(MenuBarCommand::SelectAll);
  const EditorMenuNode* save = FindCommand(MenuBarCommand::SaveFile);
  const EditorMenuNode* compositor = FindCommand(MenuBarCommand::SetCompositedRenderingFilterOnly);
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(selectAll, nullptr);
  ASSERT_NE(save, nullptr);
  ASSERT_NE(compositor, nullptr);

  EXPECT_FALSE(PresentEditorMenuItem(*copy, state).enabled);
  EXPECT_FALSE(PresentEditorMenuItem(*selectAll, state).enabled);
  EXPECT_FALSE(PresentEditorMenuItem(*save, state).enabled);
  EXPECT_FALSE(PresentEditorMenuItem(*compositor, state).checked);

  state.sourcePaneFocused = true;
  state.canSave = true;
  state.compositedRenderingMode = CompositedRenderingMode::FilterOnly;
  EXPECT_TRUE(PresentEditorMenuItem(*copy, state).enabled);
  EXPECT_TRUE(PresentEditorMenuItem(*selectAll, state).enabled);
  EXPECT_TRUE(PresentEditorMenuItem(*save, state).enabled);
  EXPECT_TRUE(PresentEditorMenuItem(*compositor, state).checked);
}

TEST(EditorMenuModelTest, NativeTextToolCaptureCannotDispatchCanvasEditCommands) {
  MenuBarState state;
  state.textToolEditing = true;
  state.hasShapeSelection = true;
  state.hasShapeClipboard = true;
  state.hasSelectableElements = true;
  state.canUndo = true;

  EXPECT_TRUE(SuppressNativeMenuShortcuts(state));
  const EditorMenuNode* newFile = FindCommand(MenuBarCommand::NewFile);
  ASSERT_NE(newFile, nullptr);
  EXPECT_FALSE(NativeMenuActivationAllowed(*newFile, state, /*keyEquivalent=*/true));
  EXPECT_TRUE(NativeMenuActivationAllowed(*newFile, state, /*keyEquivalent=*/false));
  for (const MenuBarCommand command :
       {MenuBarCommand::Undo, MenuBarCommand::Cut, MenuBarCommand::Copy, MenuBarCommand::Paste,
        MenuBarCommand::PasteInFront, MenuBarCommand::SelectAll, MenuBarCommand::DeselectAll}) {
    const EditorMenuNode* item = FindCommand(command);
    ASSERT_NE(item, nullptr);
    EXPECT_FALSE(PresentEditorMenuItem(*item, state, EditorMenuSurface::Native).enabled)
        << item->label;
    EXPECT_FALSE(NativeMenuActivationAllowed(*item, state, /*keyEquivalent=*/false));
    EXPECT_TRUE(PresentEditorMenuItem(*item, state).enabled) << item->label;
  }
}

TEST(EditorMenuModelTest, NativeInspectorTextInputCaptureCannotDispatchCanvasEditCommands) {
  MenuBarState state;
  state.inspectorTextInputFocused = true;
  state.hasShapeSelection = true;
  state.hasShapeClipboard = true;
  state.hasSelectableElements = true;

  EXPECT_TRUE(SuppressNativeMenuShortcuts(state));
  const EditorMenuNode* newFile = FindCommand(MenuBarCommand::NewFile);
  ASSERT_NE(newFile, nullptr);
  EXPECT_FALSE(NativeMenuActivationAllowed(*newFile, state, /*keyEquivalent=*/true));
  for (const MenuBarCommand command : {MenuBarCommand::Cut, MenuBarCommand::Copy,
                                       MenuBarCommand::Paste, MenuBarCommand::SelectAll}) {
    const EditorMenuNode* item = FindCommand(command);
    ASSERT_NE(item, nullptr);
    EXPECT_FALSE(PresentEditorMenuItem(*item, state, EditorMenuSurface::Native).enabled)
        << item->label;
  }
  state.inspectorTextInputFocused = false;
  EXPECT_FALSE(SuppressNativeMenuShortcuts(state));
  const EditorMenuNode* copy = FindCommand(MenuBarCommand::Copy);
  ASSERT_NE(copy, nullptr);
  EXPECT_TRUE(PresentEditorMenuItem(*copy, state, EditorMenuSurface::Native).enabled);
  EXPECT_TRUE(NativeMenuActivationAllowed(*copy, state, /*keyEquivalent=*/true));
}

TEST(EditorMenuModelTest, SourceFocusKeepsTextEditingCommandsAndDisablesCanvasOnlyActions) {
  MenuBarState state;
  state.sourcePaneFocused = true;
  state.canUndo = true;
  state.canRedo = true;
  state.hasShapeClipboard = true;
  state.hasTextSelection = true;
  state.canGroup = true;
  state.canUngroup = true;

  for (const MenuBarCommand command :
       {MenuBarCommand::Undo, MenuBarCommand::Redo, MenuBarCommand::Paste}) {
    const EditorMenuNode* item = FindCommand(command);
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(NativeMenuActivationAllowed(*item, state, /*keyEquivalent=*/true)) << item->label;
  }
  for (const MenuBarCommand command :
       {MenuBarCommand::PasteInFront, MenuBarCommand::ConvertTextToOutlines, MenuBarCommand::Group,
        MenuBarCommand::Ungroup}) {
    const EditorMenuNode* item = FindCommand(command);
    ASSERT_NE(item, nullptr);
    EXPECT_FALSE(PresentEditorMenuItem(*item, state, EditorMenuSurface::Native).enabled)
        << item->label;
    EXPECT_FALSE(PresentEditorMenuItem(*item, state, EditorMenuSurface::ImGui).enabled)
        << item->label;
    EXPECT_FALSE(NativeMenuActivationAllowed(*item, state, /*keyEquivalent=*/true));
  }
  // Source focus wins even if an in-canvas TextTool session has not committed.
  state.textToolEditing = true;
  EXPECT_FALSE(SuppressNativeMenuShortcuts(state));
  const EditorMenuNode* undo = FindCommand(MenuBarCommand::Undo);
  ASSERT_NE(undo, nullptr);
  EXPECT_TRUE(NativeMenuActivationAllowed(*undo, state, /*keyEquivalent=*/true));
}

class MenuBarPresenterTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  static MenuBarActions RenderFrame(const MenuBarState& state, ImFont* boldMenuFont = nullptr) {
    ImGui::NewFrame();
    const MenuBarActions actions = MenuBarPresenter().render(state, boldMenuFont);
    ImGui::Render();
    return actions;
  }

  static void PrimeMainMenuPopup(const char* menuLabel) {
    ImGui::NewFrame();
    ASSERT_TRUE(ImGui::BeginMainMenuBar());
    ImGui::OpenPopup(menuLabel);
    ImGui::EndMainMenuBar();
    ImGui::Render();
  }

private:
  ImGuiContext* context_ = nullptr;
};

TEST_F(MenuBarPresenterTest, RendersDisabledMenusWithoutActions) {
  MenuBarState state;
  state.sourcePaneFocused = false;
  state.canSave = false;
  state.canRevert = false;
  state.canUndo = false;
  state.canRedo = false;
  state.sourceFocusMode = false;

  const MenuBarActions actions = RenderFrame(state);

  EXPECT_FALSE(actions.openAbout);
  EXPECT_FALSE(actions.newFile);
  EXPECT_FALSE(actions.openFile);
  EXPECT_FALSE(actions.saveFile);
  EXPECT_FALSE(actions.saveFileAs);
  EXPECT_FALSE(actions.revertFile);
  EXPECT_FALSE(actions.quit);
  EXPECT_FALSE(actions.undo);
  EXPECT_FALSE(actions.redo);
  EXPECT_FALSE(actions.cut);
  EXPECT_FALSE(actions.copy);
  EXPECT_FALSE(actions.paste);
  EXPECT_FALSE(actions.selectAll);
  EXPECT_FALSE(actions.zoomIn);
  EXPECT_FALSE(actions.zoomOut);
  EXPECT_FALSE(actions.actualSize);
  EXPECT_FALSE(actions.toggleSourceFocusMode);
}

TEST_F(MenuBarPresenterTest, RendersEnabledStateWithBoldMenuFont) {
  MenuBarState state;
  state.sourcePaneFocused = true;
  state.canSave = true;
  state.canRevert = true;
  state.canUndo = true;
  state.canRedo = true;
  state.sourceFocusMode = true;

  ImFont* font = ImGui::GetIO().Fonts->Fonts[0];
  const MenuBarActions actions = RenderFrame(state, font);

  EXPECT_FALSE(actions.openAbout);
  EXPECT_FALSE(actions.newFile);
  EXPECT_FALSE(actions.openFile);
  EXPECT_FALSE(actions.saveFile);
  EXPECT_FALSE(actions.saveFileAs);
  EXPECT_FALSE(actions.revertFile);
  EXPECT_FALSE(actions.quit);
  EXPECT_FALSE(actions.undo);
  EXPECT_FALSE(actions.redo);
  EXPECT_FALSE(actions.cut);
  EXPECT_FALSE(actions.copy);
  EXPECT_FALSE(actions.paste);
  EXPECT_FALSE(actions.selectAll);
  EXPECT_FALSE(actions.zoomIn);
  EXPECT_FALSE(actions.zoomOut);
  EXPECT_FALSE(actions.actualSize);
  EXPECT_FALSE(actions.toggleSourceFocusMode);
}

TEST_F(MenuBarPresenterTest, RendersEachOpenMenuWithoutImplicitActions) {
  MenuBarState state;
  state.sourcePaneFocused = true;
  state.canSave = true;
  state.canRevert = true;
  state.canUndo = true;
  state.canRedo = true;
  state.hasShapeSelection = true;
  state.hasShapeClipboard = true;
  state.hasTextSelection = true;
  state.hasSelectableElements = true;
  state.sourceFocusMode = true;
  state.showCompositorDebugPanel = true;
  state.perfOverlayMode = PerfOverlayMode::FullGraph;

  for (const char* menuLabel : {"DONNER", "File", "Edit", "View"}) {
    PrimeMainMenuPopup(menuLabel);
    const MenuBarActions actions = RenderFrame(state);
    EXPECT_FALSE(actions.openAbout) << menuLabel;
    EXPECT_FALSE(actions.newFile) << menuLabel;
    EXPECT_FALSE(actions.openFile) << menuLabel;
    EXPECT_FALSE(actions.openSamples) << menuLabel;
    EXPECT_FALSE(actions.saveFile) << menuLabel;
    EXPECT_FALSE(actions.saveFileAs) << menuLabel;
    EXPECT_FALSE(actions.exportViewportSvg) << menuLabel;
    EXPECT_FALSE(actions.exportViewportSvgWithOverlay) << menuLabel;
    EXPECT_FALSE(actions.revertFile) << menuLabel;
    EXPECT_FALSE(actions.quit) << menuLabel;
    EXPECT_FALSE(actions.undo) << menuLabel;
    EXPECT_FALSE(actions.redo) << menuLabel;
    EXPECT_FALSE(actions.cut) << menuLabel;
    EXPECT_FALSE(actions.copy) << menuLabel;
    EXPECT_FALSE(actions.paste) << menuLabel;
    EXPECT_FALSE(actions.pasteInFront) << menuLabel;
    EXPECT_FALSE(actions.convertTextToOutlines) << menuLabel;
    EXPECT_FALSE(actions.group) << menuLabel;
    EXPECT_FALSE(actions.ungroup) << menuLabel;
    EXPECT_FALSE(actions.selectAll) << menuLabel;
    EXPECT_FALSE(actions.selectAllCanvas) << menuLabel;
    EXPECT_FALSE(actions.deselectAll) << menuLabel;
    EXPECT_FALSE(actions.deselectAllCanvas) << menuLabel;
    EXPECT_FALSE(actions.zoomIn) << menuLabel;
    EXPECT_FALSE(actions.zoomOut) << menuLabel;
    EXPECT_FALSE(actions.actualSize) << menuLabel;
    EXPECT_FALSE(actions.toggleSourceFocusMode) << menuLabel;
    EXPECT_FALSE(actions.toggleCompositorDebugPanel) << menuLabel;
    EXPECT_FALSE(actions.toggleCompositorTileOverlay) << menuLabel;
    EXPECT_FALSE(actions.setPerfOverlayMode) << menuLabel;
  }
}

TEST(MenuBarPresenterActionsTest, ApplyViewMenuToggleActionsHandlesNullAndIndependentToggles) {
  bool showCompositorDebugPanel = false;
  bool compositorTileOverlay = false;
  PerfOverlayMode perfOverlayMode = PerfOverlayMode::Off;

  ApplyViewMenuToggleActions(MenuBarActions{}, &showCompositorDebugPanel, &perfOverlayMode);
  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::Off);

  MenuBarActions actions;
  actions.toggleCompositorDebugPanel = true;
  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, nullptr);
  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::Off);

  actions = MenuBarActions{};
  actions.toggleCompositorTileOverlay = true;
  ApplyViewMenuToggleActions(actions, nullptr, nullptr, nullptr, &compositorTileOverlay);
  EXPECT_TRUE(compositorTileOverlay);

  actions = MenuBarActions{};
  actions.setPerfOverlayMode = true;
  actions.perfOverlayMode = PerfOverlayMode::FpsPill;
  ApplyViewMenuToggleActions(actions, nullptr, &perfOverlayMode);
  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::FpsPill);

  actions.toggleCompositorDebugPanel = true;
  actions.perfOverlayMode = PerfOverlayMode::FullGraph;
  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &perfOverlayMode);
  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::FullGraph);

  CompositedRenderingMode compositedRenderingMode = CompositedRenderingMode::On;
  actions = MenuBarActions{};
  actions.setCompositedRenderingMode = true;
  actions.compositedRenderingMode = CompositedRenderingMode::FilterOnly;
  ApplyViewMenuToggleActions(actions, nullptr, nullptr);
  EXPECT_EQ(compositedRenderingMode, CompositedRenderingMode::On)
      << "null out-param must leave the caller's mode untouched";
  ApplyViewMenuToggleActions(actions, nullptr, nullptr, nullptr, nullptr, &compositedRenderingMode);
  EXPECT_EQ(compositedRenderingMode, CompositedRenderingMode::FilterOnly);
}

TEST(MenuBarPresenterActionsTest, ApplyMenuBarCommandIgnoresInactiveAndNullActions) {
  MenuBarState state;
  MenuBarActions actions;

  ApplyMenuBarCommand(false, MenuBarCommand::OpenAbout, state, &actions);
  EXPECT_FALSE(actions.openAbout);

  ApplyMenuBarCommand(true, MenuBarCommand::OpenAbout, state, nullptr);
  EXPECT_FALSE(actions.openAbout);
}

TEST(MenuBarPresenterActionsTest, ApplyMenuBarCommandMapsSimpleCommandsToActions) {
  MenuBarState state;

  MenuBarActions actions;
  ApplyMenuBarCommand(true, MenuBarCommand::OpenAbout, state, &actions);
  EXPECT_TRUE(actions.openAbout);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::NewFile, state, &actions);
  EXPECT_TRUE(actions.newFile);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::OpenFile, state, &actions);
  EXPECT_TRUE(actions.openFile);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::OpenSamples, state, &actions);
  EXPECT_TRUE(actions.openSamples);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SaveFile, state, &actions);
  EXPECT_TRUE(actions.saveFile);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SaveFileAs, state, &actions);
  EXPECT_TRUE(actions.saveFileAs);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ExportViewportSvg, state, &actions);
  EXPECT_TRUE(actions.exportViewportSvg);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ExportViewportSvgWithOverlay, state, &actions);
  EXPECT_TRUE(actions.exportViewportSvgWithOverlay);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::RevertFile, state, &actions);
  EXPECT_TRUE(actions.revertFile);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Quit, state, &actions);
  EXPECT_TRUE(actions.quit);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Undo, state, &actions);
  EXPECT_TRUE(actions.undo);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Redo, state, &actions);
  EXPECT_TRUE(actions.redo);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Cut, state, &actions);
  EXPECT_TRUE(actions.cut);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Copy, state, &actions);
  EXPECT_TRUE(actions.copy);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Paste, state, &actions);
  EXPECT_TRUE(actions.paste);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::PasteInFront, state, &actions);
  EXPECT_TRUE(actions.pasteInFront);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ConvertTextToOutlines, state, &actions);
  EXPECT_TRUE(actions.convertTextToOutlines);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Group, state, &actions);
  EXPECT_TRUE(actions.group);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::Ungroup, state, &actions);
  EXPECT_TRUE(actions.ungroup);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ZoomIn, state, &actions);
  EXPECT_TRUE(actions.zoomIn);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ZoomOut, state, &actions);
  EXPECT_TRUE(actions.zoomOut);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ActualSize, state, &actions);
  EXPECT_TRUE(actions.actualSize);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ToggleSourceFocusMode, state, &actions);
  EXPECT_TRUE(actions.toggleSourceFocusMode);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ToggleCompositorDebugPanel, state, &actions);
  EXPECT_TRUE(actions.toggleCompositorDebugPanel);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::ToggleCompositorTileOverlay, state, &actions);
  EXPECT_TRUE(actions.toggleCompositorTileOverlay);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SetPerfOverlayOff, state, &actions);
  EXPECT_TRUE(actions.setPerfOverlayMode);
  EXPECT_EQ(actions.perfOverlayMode, PerfOverlayMode::Off);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SetPerfOverlayFpsPill, state, &actions);
  EXPECT_TRUE(actions.setPerfOverlayMode);
  EXPECT_EQ(actions.perfOverlayMode, PerfOverlayMode::FpsPill);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SetPerfOverlayFullGraph, state, &actions);
  EXPECT_TRUE(actions.setPerfOverlayMode);
  EXPECT_EQ(actions.perfOverlayMode, PerfOverlayMode::FullGraph);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SetCompositedRenderingOff, state, &actions);
  EXPECT_TRUE(actions.setCompositedRenderingMode);
  EXPECT_EQ(actions.compositedRenderingMode, CompositedRenderingMode::Off);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SetCompositedRenderingFilterOnly, state, &actions);
  EXPECT_TRUE(actions.setCompositedRenderingMode);
  EXPECT_EQ(actions.compositedRenderingMode, CompositedRenderingMode::FilterOnly);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SetCompositedRenderingOn, state, &actions);
  EXPECT_TRUE(actions.setCompositedRenderingMode);
  EXPECT_EQ(actions.compositedRenderingMode, CompositedRenderingMode::On);
}

TEST(MenuBarPresenterActionsTest, ApplyMenuBarCommandRoutesSelectCommandsBySourceFocus) {
  MenuBarState state;
  state.sourcePaneFocused = true;
  MenuBarActions actions;

  ApplyMenuBarCommand(true, MenuBarCommand::SelectAll, state, &actions);
  EXPECT_TRUE(actions.selectAll);
  EXPECT_FALSE(actions.selectAllCanvas);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::DeselectAll, state, &actions);
  EXPECT_TRUE(actions.deselectAll);
  EXPECT_FALSE(actions.deselectAllCanvas);

  state.sourcePaneFocused = false;
  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::SelectAll, state, &actions);
  EXPECT_FALSE(actions.selectAll);
  EXPECT_TRUE(actions.selectAllCanvas);

  actions = MenuBarActions{};
  ApplyMenuBarCommand(true, MenuBarCommand::DeselectAll, state, &actions);
  EXPECT_FALSE(actions.deselectAll);
  EXPECT_TRUE(actions.deselectAllCanvas);
}

}  // namespace
}  // namespace donner::editor
