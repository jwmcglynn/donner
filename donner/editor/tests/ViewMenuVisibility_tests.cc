#include <gtest/gtest.h>

#include "donner/editor/MenuBarPresenter.h"

namespace donner::editor {

// The Compositor Debug panel is a developer diagnostics view and must default
// to hidden; the render-pane performance overlay likewise defaults to off.
// EditorShell mirrors these defaults into its `showCompositorDebugPanel_` /
// `perfOverlayMode_` members and seeds the View-menu checkmarks from them via
// `MenuBarState`. Asserting the `MenuBarState` defaults pins the user-visible
// default-visibility contract without needing a live ImGui/GL context to
// construct an `EditorShell`.
TEST(ViewMenuVisibility, MenuBarStateDefaultsMatchVisibilityContract) {
  const MenuBarState state;
  EXPECT_FALSE(state.showCompositorDebugPanel)
      << "Compositor Debug panel must default to hidden (developer diagnostics only)";
  EXPECT_EQ(state.perfOverlayMode, PerfOverlayMode::Off)
      << "Performance overlay must default to off";
}

// The toggle actions are edge-triggered requests, so they must default to false
// (no toggle requested) on a freshly constructed `MenuBarActions`.
TEST(ViewMenuVisibility, MenuBarActionsToggleRequestsDefaultFalse) {
  const MenuBarActions actions;
  EXPECT_FALSE(actions.toggleCompositorDebugPanel);
  EXPECT_FALSE(actions.setPerfOverlayMode);
}

TEST(ViewMenuVisibility, ToggleActionsFlipCompositorDebugPanel) {
  bool showCompositorDebugPanel = false;
  PerfOverlayMode perfOverlayMode = PerfOverlayMode::Off;
  MenuBarActions actions;
  actions.toggleCompositorDebugPanel = true;

  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &perfOverlayMode);

  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::Off);

  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &perfOverlayMode);

  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::Off);
}

TEST(ViewMenuVisibility, SetActionsApplyPerfOverlayMode) {
  bool showCompositorDebugPanel = false;
  PerfOverlayMode perfOverlayMode = PerfOverlayMode::Off;
  MenuBarActions actions;
  actions.setPerfOverlayMode = true;
  actions.perfOverlayMode = PerfOverlayMode::FpsPill;

  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &perfOverlayMode);

  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::FpsPill);

  actions.perfOverlayMode = PerfOverlayMode::FullGraph;
  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &perfOverlayMode);

  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::FullGraph);

  actions.perfOverlayMode = PerfOverlayMode::Off;
  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, &perfOverlayMode);

  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::Off);
}

TEST(ViewMenuVisibility, ToggleActionsIgnoreNullVisibilityPointers) {
  bool showCompositorDebugPanel = false;
  PerfOverlayMode perfOverlayMode = PerfOverlayMode::Off;
  MenuBarActions actions;
  actions.toggleCompositorDebugPanel = true;
  actions.setPerfOverlayMode = true;
  actions.perfOverlayMode = PerfOverlayMode::FullGraph;

  ApplyViewMenuToggleActions(actions, nullptr, &perfOverlayMode);
  EXPECT_FALSE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::FullGraph);

  perfOverlayMode = PerfOverlayMode::Off;
  ApplyViewMenuToggleActions(actions, &showCompositorDebugPanel, nullptr);
  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::Off);

  ApplyViewMenuToggleActions(actions, nullptr, nullptr);
  EXPECT_TRUE(showCompositorDebugPanel);
  EXPECT_EQ(perfOverlayMode, PerfOverlayMode::Off);
}

}  // namespace donner::editor
