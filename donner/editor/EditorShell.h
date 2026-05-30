#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/CompositorDebugPanel.h"
#include "donner/editor/DialogPresenter.h"
#include "donner/editor/DocumentSyncController.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorInputBridge.h"
#include "donner/editor/EditorShellLayout.h"
#include "donner/editor/FrameCostBreakdown.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerInspectorDiagnostics.h"
#include "donner/editor/LayersPanel.h"
#include "donner/editor/MenuBarPresenter.h"
#include "donner/editor/PenTool.h"
#include "donner/editor/RenderCoordinator.h"
#include "donner/editor/RenderPanePresenter.h"
#include "donner/editor/RotateCursorSet.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/SidebarPresenter.h"
#include "donner/editor/StyleSourceAnnotations.h"
#include "donner/editor/TextEditor.h"
#include "donner/editor/TextInspectorPanel.h"
#include "donner/editor/TextTool.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::editor::gui {
class EditorWindow;
}

#ifdef DONNER_EDITOR_WGPU
namespace donner::svg {
class RendererGeode;
}
#endif

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
  /// Retained overlay texture dimensions in pixels. Zero when immediate overlay presentation is
  /// active.
  Vector2i overlayDimsPx = Vector2i::Zero();
  /// Backend overlay texture/view handle, represented as an integer for diagnostics.
  std::uint64_t overlayTextureHandle = 0;
  /// Presentation-cache resource counters captured after the frame.
  PresentationResourceStats presentationResources;
  /// Latest editor rendering cost counters.
  FrameCostBreakdown frameCost;
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
  /// Return the next idle-loop wake interval needed for throttled UI work.
  [[nodiscard]] std::optional<float> nextIdleWakeSeconds() const;

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

  /// Async renderer access for replay harnesses.
  [[nodiscard]] AsyncRenderer& asyncRendererForReplay() {
    return renderCoordinator_.asyncRenderer();
  }

  /**
   * Suppress non-document render-pane presentation on the next frame.
   *
   * @param enabled True to make the next frame's readback content-only.
   */
  void setContentOnlyCaptureForNextFrameForReplay(bool enabled) {
    contentOnlyCaptureForNextFrame_ = enabled;
  }

private:
  bool tryOpenPath(std::string_view path, std::string* error);
  bool trySavePath(std::string_view path, std::string* error);

  // Shape clipboard (design doc 0047 §"Shape Clipboard"). These run when the
  // canvas selection — not the source pane — owns Cut/Copy/Paste. Each routes a
  // single CutShapes/PasteShapes reparse so the operation is one undo step and
  // the source pane stays coherent.
  void copySelectedShapesToClipboard();
  void cutSelectedShapesToClipboard();
  void pasteShapesFromClipboard(bool inFront);
  // Convert Text to Outlines (design doc 0047 §"Convert Text to Outlines"). Runs
  // when the selection is exactly one or more `<text>` elements. Builds the
  // outlined source via `convertTextToOutlines`, records one undo step, and
  // selects the new outline group(s). Abandoned without mutating if any selected
  // `<text>` fails outline generation (§"Error Handling").
  void convertSelectedTextToOutlines();
  // True when the canvas selection is non-empty and every selected element is a
  // `<text>` element — the precondition for "Convert Text to Outlines".
  [[nodiscard]] bool selectionIsAllText() const;
  void resetPresentationForLoadedDocument(std::string_view canonicalSource);
  void requestRevert();
  void requestSave();
  void requestSaveAs(std::string error = std::string());
  /// Open the save dialog to export the current viewport as a cropped SVG.
  /// When \p includeOverlay is true, the export also serializes the current
  /// editor selection overlay (Milestone 7); otherwise it is content-only
  /// (Milestone 6). See `docs/design_docs/0047-v0_8_showcase.md`.
  void requestExportViewportSvg(bool includeOverlay = false, std::string error = std::string());
  /// Generate the viewport SVG content and write it to \p path. Returns false
  /// and sets \p error on failure (mirrors \ref trySavePath's contract).
  bool tryExportViewportSvgToPath(std::string_view path, std::string* error);
  bool synchronizeSourceBeforeSave(std::string* error);
  void updateWindowTitle();
  void handleGlobalShortcuts();
  /// True when the document has at least one selectable element (the canonical marquee/Select-All
  /// set). Gates whether Cmd+A / the Edit menu's "Select All" act on the canvas.
  [[nodiscard]] bool canvasHasSelectableElements();
  /// Selects every selectable element on the canvas via the shared `setSelection()` path, so the
  /// canvas highlight, source-pane sync, and overlay all update together. No-op without a document.
  void selectAllCanvasElements();
  void renderSourcePane(float paneOriginY, float paneHeight, float paneWidth, ImFont* codeFont);
  void renderRenderPane(const Vector2d& renderPaneOrigin, const Vector2d& renderPaneSize,
                        ImGuiWindowFlags paneFlags);
  [[nodiscard]] Box2d toolPaletteScreenRect(const ImVec2& paneOrigin,
                                            const ImVec2& contentRegion) const;
  void renderToolPalette(const ImVec2& paneOrigin, const ImVec2& contentRegion);
  void renderFillStrokeToolbarWidget();
  void renderSidebars(float rightPaneX, float rightPaneWidth, float paneOriginY,
                      const RightSidebarLayout& layout, ImGuiWindowFlags paneFlags);
  void renderSourcePaneSplitter(float windowWidth, float paneOriginY, float paneHeight,
                                float sourcePaneWidth);
  void renderRightPaneSplitter(float windowWidth, float paneOriginY, float paneHeight);
  void renderLayerPanelSplitter(float rightPaneX, float rightPaneWidth,
                                const RightSidebarLayout& layout);
  void renderDockedLayerPanelDragHandle();
  void renderFloatingLayerPanel();
  void renderLayerPanelContents();
  void maybeLogResourceDiagnostics(const FrameCostBreakdown& frameCost);
  void maybeLogFrameMissTelemetry(const FrameCostBreakdown& frameCost);
  [[nodiscard]] bool highlightSelectionSourceIfNeeded();
  [[nodiscard]] std::vector<svg::SVGElement> sourceHoverElements() const;
  [[nodiscard]] std::vector<SourceByteRange> sourceHoverRangesForElements(
      const std::vector<svg::SVGElement>& elements) const;
  [[nodiscard]] std::vector<svg::SVGElement> referenceHighlightElements() const;
  [[nodiscard]] std::vector<svg::SVGElement> combinedSourcePreviewElements() const;
  void updateSourceHoverPreview();
  void refreshReferenceHighlightSummaryIfNeeded();
  void applyReferenceHighlightPreview();
  void setReferenceHighlightChipHovered(bool hovered);
  struct SelectionChipBounds {
    Box2d documentBounds;
    Box2d screenBounds;
    Vector2d chipAnchorScreen = Vector2d::Zero();
  };
  [[nodiscard]] std::optional<Box2d> referenceHighlightChipScreenRect(std::string_view label) const;
  [[nodiscard]] std::optional<SelectionChipBounds> selectionChipBounds(
      const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) const;
  [[nodiscard]] std::optional<Box2d> selectionSizeChipScreenRect(
      std::string_view label, const Vector2d& chipAnchorScreen) const;
  void renderSelectionSizeChip(
      const SelectionTransformHandleIntent& hoverTransformIntent,
      const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview);
  void renderReferenceHighlightChip();
  bool flushQueuedMutationAndRefreshOverlay();
  void renderPenToolPreview();
  void openRenderPaneContextMenu(const Vector2d& documentPoint);
  void renderRenderPaneContextMenu();
  [[nodiscard]] std::optional<StyleFocus> styleFocusAtSourceOffset(std::size_t sourceOffset) const;
  [[nodiscard]] std::optional<StyleFocus> styleFocusAtSourceCursor();
  void applyStyleFocus(StyleFocus styleFocus);
  void syncSelectionFromSourceCursorIfNeeded();
  void applySourcePartition(FocusPartition partition);
  void updateSourceFocusView(bool scrollToSelection = false);
  void updateSourceStyleDecorations();
  void applySourceStyleDecorationChipClick();
  void setSourceFocusMode(bool enabled);
  void toggleSourceFocusMode();
  void setSourcePaneVisible(bool visible);
  void revealSourceRange(SourceByteRange byteRange);

  gui::EditorWindow& window_;
  EditorShellOptions options_;
  bool valid_ = false;

  EditorApp app_;
  SelectTool selectTool_;
  PenTool penTool_;
  TextTool textTool_;
  enum class ActiveTool : std::uint8_t {
    Select,
    Pen,
    Text,
  };
  ActiveTool activeTool_ = ActiveTool::Select;
  TextEditor textEditor_;
  /// Backing store for shape Cut/Copy/Paste. Holds the headered
  /// `# donner-shape-clipboard v1` payload (see `ShapeClipboardPayload`).
  std::unique_ptr<ClipboardInterface> shapeClipboard_;
  /// Ids of just-pasted elements to select once the PasteShapes reparse lands.
  /// Resolved against the new document in the next `flushFrame()` and cleared.
  std::vector<std::string> pendingPasteSelectionIds_;
  /// Most recent "Convert Text to Outlines" failure reason, surfaced to the user
  /// (design doc 0047 §"Error Handling": the command reports the blocking
  /// element rather than partially mutating).
  std::string lastConvertTextError_;
  GlTextureCache textures_;
  RenderCoordinator renderCoordinator_;
  RotateCursorSet rotateCursorSet_;
  DocumentSyncController documentSyncController_;
  ViewportInteractionController interactionController_;
  std::optional<ViewportState> pendingViewportReplayOverride_;
  /// True while the save modal is being used for File → Export Viewport as SVG
  /// rather than an ordinary document save. Routes the dialog's write callback
  /// to \ref tryExportViewportSvgToPath.
  bool pendingViewportExport_ = false;
  /// True when the pending viewport export should include the editor selection
  /// overlay (File → Export Viewport as SVG (with overlay)). Drives
  /// \ref ViewportExportOptions::includeSelectionOverlay and the
  /// capture-at-export-time overlay snapshot in \ref tryExportViewportSvgToPath.
  bool pendingViewportExportOverlay_ = false;
  bool contentOnlyCaptureForNextFrame_ = false;
  bool contentOnlyCaptureThisFrame_ = false;
  bool requestRenderAtEndOfFrame_ = false;
  EditorInputBridge inputBridge_;
  MenuBarPresenter menuBarPresenter_;
  SidebarPresenter sidebarPresenter_;
  LayersPanel layersPanel_;
  CompositorDebugPanel compositorDebugPanel_;
  RenderPanePresenter renderPanePresenter_;
  DialogPresenter dialogPresenter_;
#ifdef DONNER_EDITOR_WGPU
  std::unique_ptr<svg::RendererGeode> directOverlayRenderer_;
#endif

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
  std::uint64_t resourceDiagnosticsFrame_ = 0;
  std::uint64_t frameTelemetryFrame_ = 0;
  std::uint64_t lastLoggedPresentationPeakBytes_ = 0;
  std::uint64_t lastLoggedWgpuTextureCreates_ = 0;
  std::uint64_t lastLoggedWgpuBufferCreates_ = 0;
  bool frameMissTelemetryWriteErrorLogged_ = false;
  bool treeSelectionOriginatedInTree_ = false;
  bool sourceSelectionOriginatedInText_ = false;
  bool sourceFocusOriginatedInStyle_ = false;
  ReferenceHighlightSummary referenceHighlightSummary_;
  std::vector<svg::SVGElement> lastReferenceHighlightSelection_;
  bool referenceHighlightActive_ = false;
  bool referenceHighlightChipHovered_ = false;
  std::vector<StyleSourceContribution> styleSourceContributions_;
  bool styleSourceDecorationsValid_ = false;
  std::uint64_t styleSourceDecorationSourceVersion_ = 0;
  std::string styleSourceDecorationText_;
  std::optional<Vector2d> renderContextMenuDocumentPoint_;
  std::optional<svg::SVGElement> renderContextMenuHitElement_;
  bool renderContextMenuOpenRequested_ = false;
  /// Suppress source-pane reselection/scrolling when a source edit remaps the same selection while
  /// the cursor remains inside the active focus partition.
  bool preserveSourceEditFocusCursor_ = false;
  /// Design doc 0033 §M8: set when an M8 fast-path click consumed the
  /// pending click without going through the `!isBusy()`-gated post-
  /// onMouseDown cache refresh. The follow-up fires on the next idle
  /// frame so re-drag hit testing catches up without posting a
  /// pre-move render ahead of the drag update.
  bool pendingClickFollowupAfterIdle_ = false;
  std::optional<double> pendingSelectClickStartSeconds_;
  bool sourceFocusMode_ = true;
  /// Preferred width for the source pane when it is visible.
  float sourcePaneWidth_ = 560.0f;
  bool sourcePaneVisible_ = true;
  /// Whether the Compositor Debug panel window renders. Off by default: it is a
  /// developer-facing composite-tile diagnostics view, toggled on via the View
  /// menu. The user-facing Layers panel is unrelated and always visible.
  bool showCompositorDebugPanel_ = false;
  /// Whether the render-pane frame-timing/perf overlay renders. On by default;
  /// toggled via the View menu.
  bool showPerfOverlay_ = true;

  ImFont* uiFontBold_ = nullptr;
  ImFont* codeFont_ = nullptr;

  /// Optional UI-input recorder. Populated when `options_.reproOutputPath`
  /// is set. Snapshots ImGui state at the start of each frame and
  /// flushes to disk in the destructor. See
  /// `donner/editor/repro/ReproRecorder.h`.
  std::unique_ptr<repro::ReproRecorder> reproRecorder_;
};

}  // namespace donner::editor
