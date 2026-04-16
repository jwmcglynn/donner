#pragma once
/// @file

#include <optional>
#include <string>

#include "donner/editor/DialogPresenter.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorInputBridge.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/MenuBarPresenter.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/RenderPanePresenter.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SidebarPresenter.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/ViewportInteractionController.h"
#include "imgui.h"

namespace donner::editor::gui {
class EditorWindow;
}

namespace donner::editor {

struct EditorShellOptions {
  std::string svgPath;
  bool experimentalMode = false;
};

/// Stateful advanced editor frontend shell. Owns all long-lived GUI/editor orchestration state.
class EditorShell {
public:
  EditorShell(gui::EditorWindow& window, EditorShellOptions options);

  [[nodiscard]] bool valid() const { return valid_; }
  void runFrame();

private:
  bool tryOpenPath(std::string_view path, std::string* error);
  void applyExperimentalModeChange(bool enabled);
  void updateWindowTitle();
  void handleGlobalShortcuts();
  void renderSourcePane(float paneOriginY, float paneHeight, ImFont* codeFont);
  void renderRenderPane(const Vector2d& renderPaneOrigin, const Vector2d& renderPaneSize,
                        ImGuiWindowFlags paneFlags);
  void renderSidebars(float rightPaneX, float paneOriginY, float treePaneHeight,
                      float inspectorPaneY, float inspectorPaneHeight, ImGuiWindowFlags paneFlags);
  void highlightSelectionSourceIfNeeded();

  gui::EditorWindow& window_;
  EditorShellOptions options_;
  bool valid_ = false;

  EditorApp app_;
  SelectTool selectTool_;
  TextEditor textEditor_;
  GlTextureCache textures_;
  RenderCoordinator renderCoordinator_;
  DocumentSyncController documentSyncController_;
  ViewportInteractionController interactionController_;
  EditorInputBridge inputBridge_;
  MenuBarPresenter menuBarPresenter_;
  SidebarPresenter sidebarPresenter_;
  RenderPanePresenter renderPanePresenter_;
  DialogPresenter dialogPresenter_;

  std::string lastWindowTitle_;
  bool viewportInitialized_ = false;
  std::optional<svg::SVGElement> lastHighlightedSelection_;
  std::optional<svg::SVGElement> lastTreeSelection_;
  bool treeviewPendingScroll_ = false;
  bool treeSelectionOriginatedInTree_ = false;

  ImFont* uiFontBold_ = nullptr;
  ImFont* codeFont_ = nullptr;
};

}  // namespace donner::editor
