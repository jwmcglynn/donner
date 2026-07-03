#include <gtest/gtest.h>

#include "donner/editor/MenuBarPresenter.h"

namespace donner::editor {

// The Compositor Debug panel is a developer diagnostics view and must default
// to hidden; the render-pane performance overlay defaults to visible. EditorShell
// mirrors these defaults into its `showCompositorDebugPanel_` / `showPerfOverlay_`
// members and seeds the View-menu checkmarks from them via `MenuBarState`. Asserting
// the `MenuBarState` defaults pins the user-visible default-visibility contract
// without needing a live ImGui/GL context to construct an `EditorShell`.
TEST(ViewMenuVisibility, MenuBarStateDefaultsMatchVisibilityContract) {
  const MenuBarState state;
  EXPECT_FALSE(state.showCompositorDebugPanel)
      << "Compositor Debug panel must default to hidden (developer diagnostics only)";
  EXPECT_TRUE(state.showPerfOverlay) << "Performance overlay must default to visible";
}

// The toggle actions are edge-triggered requests, so they must default to false
// (no toggle requested) on a freshly constructed `MenuBarActions`.
TEST(ViewMenuVisibility, MenuBarActionsToggleRequestsDefaultFalse) {
  const MenuBarActions actions;
  EXPECT_FALSE(actions.toggleCompositorDebugPanel);
  EXPECT_FALSE(actions.togglePerfOverlay);
}

TEST(ViewMenuVisibility, ToggleActionsFlipCompositorDebugPanel) {
  bool showCompositorDebugPanel = false;
  bool showPerfOverlay = true;
  MenuBarActions actions;
  actions.toggleCompositorDebugPanel = true;

  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &showPerfOverlay);

  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_TRUE(showPerfOverlay);

  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &showPerfOverlay);

  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_TRUE(showPerfOverlay);
}

TEST(ViewMenuVisibility, ToggleActionsFlipPerfOverlay) {
  bool showCompositorDebugPanel = false;
  bool showPerfOverlay = true;
  MenuBarActions actions;
  actions.togglePerfOverlay = true;

  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &showPerfOverlay);

  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_FALSE(showPerfOverlay);

  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &showPerfOverlay);

  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_TRUE(showPerfOverlay);
}

}  // namespace donner::editor
