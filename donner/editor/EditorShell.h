#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "donner/editor/DialogPresenter.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorInputBridge.h"
#include "donner/editor/EditorShellLayout.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerInspectorDiagnostics.h"
#include "donner/editor/LayerInspectorPanel.h"
#include "donner/editor/MenuBarPresenter.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/RenderPanePresenter.h"
#include "donner/editor/RotateCursorSet.h"
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
  /// Optional destination path for a `.donner-repro` recording of the
  /// user's UI interactions. When set, the shell constructs a
  /// `ReproRecorder` and snapshots ImGui input state at the start of
  /// every frame. Written to disk in the destructor.
  std::optional<std::string> reproOutputPath;
};

/// Layer-inspector freshness status exposed to replay/readback harnesses.
struct LayerInspectorStatusReadback {
  /// One paint-order composited tile exposed to replay diagnostics.
  struct Tile {
    /// Stable texture-cache tile id.
    std::string id;
    /// Segment/layer tile kind.
    RenderResult::CompositedTile::Kind kind = RenderResult::CompositedTile::Kind::Segment;
    /// Raster payload generation.
    std::uint64_t generation = 0;
    /// Texture dimensions in pixels.
    Vector2i bitmapDimsPx = Vector2i::Zero();
    /// Raster canvas size that produced the texture payload.
    Vector2i rasterCanvasSize = Vector2i::Zero();
    /// Tile origin in document/canvas coordinates.
    Vector2d canvasOffsetDoc = Vector2d::Zero();
    /// Tile dimensions in document/canvas units.
    Vector2d bitmapDimsDoc = Vector2d::Zero();
    /// Drag translation represented by the presented tile.
    Vector2d dragTranslationDoc = Vector2d::Zero();
    /// Effective drag translation used by the render-pane presenter this frame.
    Vector2d presentedDragTranslationDoc = Vector2d::Zero();
    /// Backend texture/view handle, represented as an integer for diagnostics.
    std::uint64_t textureHandle = 0;
    /// True when the tile reused an existing texture with metadata-only geometry.
    bool metadataOnly = false;
    /// True when this tile represents the active drag target.
    bool isDragTarget = false;
  };

  /// Canvas freshness classification used by the layer inspector.
  CanvasFreshness canvasFreshness = CanvasFreshness::Current;
  /// Status suffix rendered beside document canvas diagnostics.
  std::string statusSuffix;
  /// Canvas size implied by the current viewport.
  Vector2i viewportDesiredCanvas = Vector2i::Zero();
  /// Canvas size committed to the document path used by the editor shell.
  Vector2i documentCanvas = Vector2i::Zero();
  /// Canvas size last rasterized by the compositor.
  Vector2i compositorCanvas = Vector2i::Zero();
  /// Metadata-only composited tiles skipped during the last upload.
  int metadataOnlyMissCount = 0;
  /// Duplicate live texture handles found across different tile ids.
  int duplicateLiveTextureCount = 0;
  /// Overlay texture dimensions in pixels.
  Vector2i overlayDimsPx = Vector2i::Zero();
  /// Backend overlay texture/view handle, represented as an integer for diagnostics.
  std::uint64_t overlayTextureHandle = 0;
  /// Paint-order texture state currently visible to the presenter.
  std::vector<Tile> tiles;
};

/// Stateful advanced editor frontend shell. Owns all long-lived GUI/editor orchestration state.
class EditorShell {
public:
  EditorShell(gui::EditorWindow& window, EditorShellOptions options);
  ~EditorShell();

  [[nodiscard]] bool valid() const { return valid_; }
  void runFrame();

  /// Current render-pane viewport. Exposed for replay/readback harnesses
  /// that need to crop the presented framebuffer to the canvas region.
  [[nodiscard]] const ViewportState& viewportForReadback() const {
    return interactionController_.viewport();
  }
  /// Override the viewport from a recorded replay frame before rendering.
  ///
  /// @param viewport Recorded viewport snapshot to install for the next frame.
  void overrideViewportForReplay(const ViewportState& viewport);
  /// Current selection label for replay/readback harnesses.
  [[nodiscard]] std::optional<std::string> selectedElementLabelForReadback() const;
  /// Current layer-inspector freshness status for replay/readback harnesses.
  [[nodiscard]] LayerInspectorStatusReadback layerInspectorStatusForReadback() const;

private:
  bool tryOpenPath(std::string_view path, std::string* error);
  bool trySavePath(std::string_view path, std::string* error);
  void requestSave();
  void requestSaveAs(std::string error = std::string());
  bool synchronizeSourceBeforeSave(std::string* error);
  void updateWindowTitle();
  void handleGlobalShortcuts();
  void renderSourcePane(float paneOriginY, float paneHeight, ImFont* codeFont);
  void renderRenderPane(const Vector2d& renderPaneOrigin, const Vector2d& renderPaneSize,
                        ImGuiWindowFlags paneFlags);
  void renderSidebars(float rightPaneX, float rightPaneWidth, float paneOriginY,
                      const RightSidebarLayout& layout, ImGuiWindowFlags paneFlags);
  void renderRightPaneSplitter(float windowWidth, float paneOriginY, float paneHeight);
  void renderLayerPanelSplitter(float rightPaneX, float rightPaneWidth,
                                const RightSidebarLayout& layout);
  void renderDockedLayerPanelDragHandle();
  void renderFloatingLayerPanel();
  void renderLayerPanelContents();
  [[nodiscard]] bool highlightSelectionSourceIfNeeded();
  [[nodiscard]] std::vector<svg::SVGElement> sourceHoverElements() const;
  [[nodiscard]] std::vector<SourceByteRange> sourceHoverRangesForElements(
      const std::vector<svg::SVGElement>& elements) const;
  void updateSourceHoverPreview();
  [[nodiscard]] std::optional<StyleFocus> styleFocusAtSourceOffset(std::size_t sourceOffset) const;
  [[nodiscard]] std::optional<StyleFocus> styleFocusAtSourceCursor();
  void applyStyleFocus(StyleFocus styleFocus);
  void syncSelectionFromSourceCursorIfNeeded();
  void applySourcePartition(FocusPartition partition);
  void updateSourceFocusView(bool scrollToSelection = false);
  void setSourceFocusMode(bool enabled);
  void toggleSourceFocusMode();

  gui::EditorWindow& window_;
  EditorShellOptions options_;
  bool valid_ = false;

  EditorApp app_;
  SelectTool selectTool_;
  TextEditor textEditor_;
  GlTextureCache textures_;
  RenderCoordinator renderCoordinator_;
  RotateCursorSet rotateCursorSet_;
  DocumentSyncController documentSyncController_;
  ViewportInteractionController interactionController_;
  std::optional<ViewportState> pendingViewportReplayOverride_;
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
  /// Fraction of the lower right column assigned to the compositor tile
  /// panel. Mutated by the horizontal splitter between Inspector and
  /// Layers.
  float layerPanelHeightFraction_ = 0.5f;
  /// Whether the compositor tile panel is detached from the right column.
  bool layerPanelDetached_ = false;
  /// Set while the mouse drag that detached the panel is still in progress.
  bool layerPanelDetachDragActive_ = false;
  /// Force the floating Layers window to use \ref layerPanelFloatingPos_
  /// and \ref layerPanelFloatingSize_ on its next frame.
  bool layerPanelFloatingNeedsPlacement_ = false;
  /// Last floating Layers window position in screen pixels.
  ImVec2 layerPanelFloatingPos_ = ImVec2(0.0f, 0.0f);
  /// Last floating Layers window size in screen pixels.
  ImVec2 layerPanelFloatingSize_ = ImVec2(420.0f, 360.0f);
  std::vector<svg::SVGElement> lastHighlightedSelection_;
  std::optional<svg::SVGElement> lastTreeSelection_;
  std::optional<ImVec2> lastPostedScreenPoint_;
  bool treeviewPendingScroll_ = false;
  bool treeSelectionOriginatedInTree_ = false;
  bool sourceSelectionOriginatedInText_ = false;
  bool sourceFocusOriginatedInStyle_ = false;
  /// Design doc 0033 §M8: set when an M8 fast-path click consumed the
  /// pending click without going through the `!isBusy()`-gated post-
  /// onMouseDown cache refresh. The follow-up fires on the next idle
  /// frame so re-drag hit testing catches up without posting a
  /// pre-move render ahead of the drag update.
  bool pendingClickFollowupAfterIdle_ = false;
  bool sourceFocusMode_ = true;

  ImFont* uiFontBold_ = nullptr;
  ImFont* codeFont_ = nullptr;

  /// Optional UI-input recorder. Populated when `options_.reproOutputPath`
  /// is set. Snapshots ImGui state at the start of each frame and
  /// flushes to disk in the destructor. See
  /// `donner/editor/repro/ReproRecorder.h`.
  std::unique_ptr<repro::ReproRecorder> reproRecorder_;
};

}  // namespace donner::editor
