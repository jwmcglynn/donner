#pragma once
/// @file

#include <memory>
#include <optional>
#include <string>

#include "donner/editor/DialogPresenter.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorInputBridge.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerInspectorPanel.h"
#include "donner/editor/MenuBarPresenter.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/RenderPanePresenter.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SidebarPresenter.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::editor::gui {
class EditorWindow;
}

namespace donner::editor::repro {
class ReproRecorder;
}  // namespace donner::editor::repro

namespace donner::editor {

struct EditorShellOptions {
  std::string svgPath;
  std::optional<std::string> initialSource;
  std::optional<std::string> initialPath;
  std::string editorNoticeText;
  bool experimentalMode = false;
  /// Optional destination path for a `.donner-repro` recording of the
  /// user's UI interactions. When set, the shell constructs a
  /// `ReproRecorder` and snapshots ImGui input state at the start of
  /// every frame. Written to disk in the destructor.
  std::optional<std::string> reproOutputPath;
};

/// Stateful advanced editor frontend shell. Owns all long-lived GUI/editor orchestration state.
class EditorShell {
public:
  EditorShell(gui::EditorWindow& window, EditorShellOptions options);
  ~EditorShell();

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
  void renderSidebars(float rightPaneX, float rightPaneWidth, float paneOriginY,
                      float treePaneHeight, float inspectorPaneY, float inspectorPaneHeight,
                      float layerPanelPaneY, float layerPanelHeight, ImGuiWindowFlags paneFlags);
  void renderRightPaneSplitter(float windowWidth, float paneOriginY, float paneHeight);
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
  LayerInspectorPanel layerInspectorPanel_;
  RenderPanePresenter renderPanePresenter_;
  DialogPresenter dialogPresenter_;

  std::string lastWindowTitle_;
  bool viewportInitialized_ = false;
  /// Width (in pixels) of the right-side column hosting the tree view,
  /// inspector, and compositor layer panel. Mutated by the
  /// drag-to-resize splitter rendered between the render pane and the
  /// right column.
  float rightPaneWidth_ = 420.0f;
  std::optional<svg::SVGElement> lastHighlightedSelection_;
  std::optional<svg::SVGElement> lastTreeSelection_;
  std::optional<ImVec2> lastPostedScreenPoint_;
  bool treeviewPendingScroll_ = false;
  bool treeSelectionOriginatedInTree_ = false;
  /// Design doc 0033 §M8: set when an M8 fast-path click consumed the
  /// pending click without going through the `!isBusy()`-gated post-
  /// onMouseDown cache refresh. The follow-up fires on the next idle
  /// frame so re-drag hit testing catches up without posting a
  /// pre-move render ahead of the drag update.
  bool pendingClickFollowupAfterIdle_ = false;

  ImFont* uiFontBold_ = nullptr;
  ImFont* codeFont_ = nullptr;

  /// Optional UI-input recorder. Populated when `options_.reproOutputPath`
  /// is set. Snapshots ImGui state at the start of each frame and
  /// flushes to disk in the destructor. See
  /// `donner/editor/repro/ReproRecorder.h`.
  std::unique_ptr<repro::ReproRecorder> reproRecorder_;
};

}  // namespace donner::editor
