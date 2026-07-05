#include "donner/editor/MenuBarPresenter.h"

#include <gtest/gtest.h>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
namespace {

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
  state.showPerfOverlay = true;

  for (const char* menuLabel : {"Donner SVG Editor", "File", "Edit", "View"}) {
    PrimeMainMenuPopup(menuLabel);
    const MenuBarActions actions = RenderFrame(state);
    EXPECT_FALSE(actions.openAbout) << menuLabel;
    EXPECT_FALSE(actions.openFile) << menuLabel;
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
    EXPECT_FALSE(actions.selectAll) << menuLabel;
    EXPECT_FALSE(actions.selectAllCanvas) << menuLabel;
    EXPECT_FALSE(actions.deselectAll) << menuLabel;
    EXPECT_FALSE(actions.deselectAllCanvas) << menuLabel;
    EXPECT_FALSE(actions.zoomIn) << menuLabel;
    EXPECT_FALSE(actions.zoomOut) << menuLabel;
    EXPECT_FALSE(actions.actualSize) << menuLabel;
    EXPECT_FALSE(actions.toggleSourceFocusMode) << menuLabel;
    EXPECT_FALSE(actions.toggleCompositorDebugPanel) << menuLabel;
    EXPECT_FALSE(actions.togglePerfOverlay) << menuLabel;
  }
}

TEST(MenuBarPresenterActionsTest, ApplyViewMenuToggleActionsHandlesNullAndIndependentToggles) {
  bool showCompositorDebugPanel = false;
  bool showPerfOverlay = true;

  ApplyViewMenuToggleActions(MenuBarActions{}, &showCompositorDebugPanel, &showPerfOverlay);
  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_TRUE(showPerfOverlay);

  MenuBarActions actions;
  actions.toggleCompositorDebugPanel = true;
  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, nullptr);
  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_TRUE(showPerfOverlay);

  actions = MenuBarActions{};
  actions.togglePerfOverlay = true;
  ApplyViewMenuToggleActions(actions, nullptr, &showPerfOverlay);
  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_FALSE(showPerfOverlay);

  actions.toggleCompositorDebugPanel = true;
  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &showPerfOverlay);
  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_TRUE(showPerfOverlay);
}

}  // namespace
}  // namespace donner::editor
