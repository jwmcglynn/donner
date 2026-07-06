#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/CanvasScrollbars.h"
#include "donner/editor/ClipboardInterface.h"
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
#include "donner/editor/NativeDialogCoordinator.h"
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
#include "donner/svg/renderer/Renderer.h"

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
struct ReproAction;
}  // namespace donner::editor::repro

namespace donner::editor {

#ifdef DONNER_EDITOR_WGPU
class FramebufferCheckerboardRenderer;
#endif

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

/// Document-space replay input consumed by the editor shell test harness.
struct EditorShellDocumentReplayInput {
  /// Current pointer position in SVG document coordinates.
  Vector2d documentPoint = Vector2d::Zero();
  /// True while the left mouse button is held.
  bool leftMouseDown = false;
  /// True on the frame where the left mouse button was pressed.
  bool leftMousePressed = false;
  /// True on the frame where the left mouse button was released.
  bool leftMouseReleased = false;
  /// Mouse modifiers to pass to the active canvas tool.
  MouseModifiers modifiers;
  /// Optional element id captured by the replay hit-test checkpoint.
  std::optional<std::string> hitElementId;
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
    /// Affine transform represented by the presented tile.
    Transform2d documentFromCachedDocument = Transform2d();
    /// Effective affine transform used by the render-pane presenter this frame.
    Transform2d presentedDocumentFromCachedDocument = Transform2d();
    /// Backend texture/view handle, represented as an integer for diagnostics.
    std::uint64_t textureHandle = 0;
    /// Renderer-owned texture snapshot retained by the presentation cache, if any.
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot;
    /// True when the tile reused an existing texture with metadata-only geometry.
    bool metadataOnly = false;
    /// True when this tile represents the active drag target.
    bool isDragTarget = false;
  };

  /// One Layers-panel row thumbnail exposed to replay diagnostics.
  struct RowThumbnail {
    /// Visible row label.
    std::string displayName;
    /// Stable row id used for thumbnail texture cache keys.
    std::uint64_t stableId = 0;
    /// Cached thumbnail bitmap dimensions, or zero when the row has no raster thumbnail.
    Vector2i bitmapDimsPx = Vector2i::Zero();
  };

  /// Layers-panel thumbnail refresh diagnostics.
  struct ThumbnailRefreshStats {
    /// Document frame version observed during the refresh.
    std::uint64_t documentFrameVersion = 0;
    /// Visible row count after refreshing the model.
    std::size_t rowCount = 0;
    /// Number of row thumbnails rasterized through Donner during this refresh.
    std::size_t renderedCount = 0;
    /// Number of cached row thumbnails reused during this refresh.
    std::size_t reusedCount = 0;
    /// Number of rows skipped because the canvas render tree had pending invalidation.
    std::size_t skippedForCanvasInvalidationCount = 0;
    /// Wall time spent in thumbnail rasterization work.
    double renderMs = 0.0;
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
  /// Current live document frame version.
  std::uint64_t documentFrameVersion = 0;
  /// Last document frame version published to the presentation.
  std::uint64_t displayedDocVersion = 0;
  /// Document version represented by the current immediate overlay snapshot, if any.
  std::optional<std::uint64_t> immediateOverlayDocumentVersion;
  /// Selected entity eligible for composited presentation, or entt::null.
  Entity selectedCompositedEntity = entt::null;
  /// Whether the last document flush applied commands.
  bool lastFlushAppliedCommands = false;
  /// Whether the last document flush replaced the document.
  bool lastFlushReplacedDocument = false;
  /// Whether the last document flush removed any element.
  bool lastFlushRemovedElements = false;
  /// Entities whose cached composited pixels were invalidated by the last flush.
  std::vector<Entity> lastFlushCacheInvalidatedElements;
  /// True when the shell is carrying a render request into a later frame.
  bool requestRenderAtEndOfFrame = false;
  /// Selected entity whose promoted pixels are known stale, or entt::null.
  Entity pendingSelectedLayerRasterizationEntity = entt::null;
  /// Document version at which the selected-layer rasterization was requested.
  std::uint64_t pendingSelectedLayerRasterizationVersion = 0;
  /// Selected element's raw `style` attribute, if present.
  std::optional<std::string> selectedStyleAttribute;
  /// Selected element's parsed local style fill, if the StyleComponent exists.
  std::optional<std::string> selectedLocalStyleFill;
  /// Selected element's existing computed fill, if a ComputedStyleComponent exists.
  std::optional<std::string> selectedComputedFill;
  /// Selected element's renderer-instance resolved fill, if a RenderingInstanceComponent exists.
  std::optional<std::string> selectedRenderingInstanceFill;
  /// Selected element's raw `d` attribute, if present.
  std::optional<std::string> selectedPathDataAttribute;
  /// Selected element's text content, if it is a `<text>` element.
  std::optional<std::string> selectedTextContent;
  /// Presentation-cache resource counters captured after the frame.
  PresentationResourceStats presentationResources;
  /// Presentation coverage (bounded raster + overview infill) after the frame.
  PresentationCoverageDiagnostics presentationCoverage;
  /// Number of retained overview tiles available as zoom/pan infill.
  std::size_t overviewTileCount = 0;
  /// Render-pane ImGui window scroll state. The canvas pane must never
  /// window-scroll (that would move the overlay chrome instead of the
  /// document), so a non-zero max is a layout bug.
  float renderPaneScrollY = 0.0f;
  float renderPaneScrollMaxY = 0.0f;
  /// Latest editor rendering cost counters.
  FrameCostBreakdown frameCost;
  /// Active drag transform driving the presenter, if any.
  std::optional<SelectTool::ActiveDragPreview> activeDragPreview;
  /// Drag transform represented by the displayed cached content, if any.
  std::optional<SelectTool::ActiveDragPreview> displayedDragPreview;
  /// Paint-order texture state currently visible to the presenter.
  std::vector<Tile> tiles;
  /// Layers-panel row thumbnail bitmaps currently cached by the shell.
  std::vector<RowThumbnail> rowThumbnails;
  /// Layers-panel thumbnail refresh diagnostics.
  ThumbnailRefreshStats thumbnailRefreshStats;
};

/// Stateful advanced editor frontend shell. Owns all long-lived GUI/editor orchestration state.
class EditorShell {
  friend class EditorShellTestAccess;

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
  /**
   * Queue document-space pointer input for the next replay frame.
   *
   * This bypasses GUI-coordinate hit testing while keeping the frame rendered through the real
   * editor shell and presentation stack.
   *
   * @param input Replay pointer input for the next frame.
   */
  void queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput input);
  /// Queue one scroll event for the next replay frame.
  ///
  /// @param event Recorded scroll event to feed through the render-pane gesture bridge.
  void queueScrollEventForReplayForTesting(RenderPaneScrollEvent event);
  /// Apply one semantic `.rnr` replay action before the next frame.
  ///
  /// @param action Tool or paint action decoded from a replay frame.
  void applyReplayActionForTesting(const repro::ReproAction& action);
  /// Current selection label for replay/readback harnesses.
  [[nodiscard]] std::optional<std::string> selectedElementLabelForReadback() const;

  /// Current document source text, for replay/readback harness assertions
  /// about gesture writebacks (nullopt without a document/source store).
  [[nodiscard]] std::optional<std::string> documentSourceForReadback() const;
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
  void applyPendingDocumentSpaceReplayInputForTesting();

  // Shape clipboard. These run when the
  // canvas selection - not the source pane - owns Cut/Copy/Paste. Each routes a
  // single CutShapes/PasteShapes reparse so the operation is one undo step and
  // the source pane stays coherent.
  void copySelectedShapesToClipboard();
  void cutSelectedShapesToClipboard();
  void pasteShapesFromClipboard(bool inFront);
  // Convert Text to Outlines. Runs
  // when the selection is exactly one or more `<text>` elements. Builds the
  // outline group via `convertTextToOutlines`, applies it as structural DOM
  // edits (insert group + paths, delete the `<text>`), records one undo step,
  // and selects the new outline group(s). Abandoned without mutating if any
  // selected `<text>` fails outline generation.
  void convertSelectedTextToOutlines();
  // True when the canvas selection is non-empty and every selected element is a
  // `<text>` element - the precondition for "Convert Text to Outlines".
  [[nodiscard]] bool selectionIsAllText() const;
  void resetPresentationForLoadedDocument(std::string_view canonicalSource);
  void requestRevert();
  /// Present the "Open SVG" chooser. Uses the native OS dialog when
  /// available (macOS), otherwise the in-editor ImGui path modal.
  void promptOpenFile();
  void requestSave();
  void requestSaveAs(std::string error = std::string());
  /// Open the save dialog to export the current viewport as a cropped SVG.
  /// When \p includeOverlay is true, the export also serializes the current
  /// editor selection overlay; otherwise it is content-only.
  void requestExportViewportSvg(bool includeOverlay = false, std::string error = std::string());
  /// Generate the viewport SVG content and write it to \p path. Returns false
  /// and sets \p error on failure (mirrors \ref trySavePath's contract).
  bool tryExportViewportSvgToPath(std::string_view path, std::string* error);
  bool synchronizeSourceBeforeSave(std::string* error);
  void updateWindowTitle();
  void applyMenuActions(const MenuBarActions& menuActions);
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
  /// Discoverability hint while the text tool is idle: double-click places
  /// point text, drag draws a text box. Bottom-center of the render pane.
  void renderTextToolHint();
  [[nodiscard]] SelectionChromeDetail selectionChromeDetailForActiveTool() const;
  bool flushQueuedMutationAndRefreshOverlay();
  /// Re-run the post-flush presentation refresh after a tool that flushes the
  /// document internally (the text tool's wrap measurement).
  void refreshAfterToolDrivenFlush();
  /// Keyboard handling while the in-canvas text editing session is active:
  /// typing, caret movement, Cmd+B/I/U style toggles, Escape commit.
  void handleTextEditingKeyboard();
  /// Emulated canvas scrollbars along the pane edges: they represent the
  /// document extent relative to the viewport and pan the canvas when
  /// dragged. The pane window itself never scrolls.
  void renderCanvasScrollbars();
  /// Point the render coordinator's pen live-geometry preview at the path the
  /// Pen tool is actively editing (or keep it alive after a session ends
  /// until the async raster of the final geometry lands), clearing it
  /// otherwise. Called before every overlay capture.
  void updatePenLivePreviewTarget();
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
  /// (the command reports the blocking element rather than partially mutating).
  std::string lastConvertTextError_;
  GlTextureCache textures_;
  /// Layers-panel thumbnail texture cache. Uploads each row's Donner-rendered
  /// preview bitmap to a GL/WGPU texture (same path as the render pane) keyed by
  /// row stable id, so ImGui can blit the real thumbnail instead of a swatch.
  GlTextureCache thumbnailTextures_;
  /// Clean offscreen renderer used only for Layers-panel thumbnails. This
  /// shares the editor's Geode device but is never bound to the live framebuffer,
  /// so row previews cannot inherit presentation state from the main renderer.
  svg::Renderer layerThumbnailRenderer_;
  RenderCoordinator renderCoordinator_;
  RotateCursorSet rotateCursorSet_;
  DocumentSyncController documentSyncController_;
  ViewportInteractionController interactionController_;
  std::optional<ViewportState> pendingViewportReplayOverride_;
  std::optional<EditorShellDocumentReplayInput> pendingDocumentSpaceReplayInput_;
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
  /// Render-pane ImGui scroll state sampled each frame for diagnostics.
  float renderPaneScrollYForDiagnostics_ = 0.0f;
  float renderPaneScrollMaxYForDiagnostics_ = 0.0f;
  /// True when an actively-moving pen drag flushed new path geometry this UI
  /// frame. The end-of-frame render request is skipped for such frames so the
  /// async worker stays idle during continuous pen drags: the live preview
  /// presents the geometry, and an idle worker guarantees the next drag
  /// frame's flush is never deferred behind a busy render.
  bool penDragFlushedThisFrame_ = false;
  EditorInputBridge inputBridge_;
  MenuBarPresenter menuBarPresenter_;
  SidebarPresenter sidebarPresenter_;
  TextInspectorPanel textInspectorPanel_;
  LayersPanel layersPanel_;
  /// Element hovered in the Layers panel as of the last frame, fed into the
  /// source-hover preview so the canvas and source pane highlight the element
  /// under the cursor - mirroring source-pane hover. Reset when no row is
  /// hovered.
  std::optional<svg::SVGElement> layersPanelHoverElement_;
  CompositorDebugPanel compositorDebugPanel_;
  RenderPanePresenter renderPanePresenter_;
  DialogPresenter dialogPresenter_;
  NativeDialogCoordinator nativeDialogs_;
#ifdef DONNER_EDITOR_WGPU
  std::unique_ptr<FramebufferCheckerboardRenderer> directCheckerboardRenderer_;
  std::unique_ptr<svg::RendererGeode> directDocumentRenderer_;
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
  /// Full UI-frame cost most recently published to diagnostics/readback.
  FrameCostBreakdown latestFrameCostForReadback_;
  /// Direct document presentation cost completed after the last shell frame.
  FrameCostBreakdown::DirectPresentation lastDirectPresentationCost_;
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
  /// Render-pane frame-timing/perf overlay mode. Off by default; set via the
  /// View menu's Performance Overlay submenu.
  PerfOverlayMode perfOverlayMode_ = PerfOverlayMode::Off;

  ImFont* uiFontBold_ = nullptr;
  ImFont* codeFont_ = nullptr;

  /// Optional UI-input recorder. Populated when `options_.reproOutputPath`
  /// is set. Snapshots ImGui state at the start of each frame and
  /// flushes to disk in the destructor. See
  /// `donner/editor/repro/ReproRecorder.h`.
  std::unique_ptr<repro::ReproRecorder> reproRecorder_;
};

}  // namespace donner::editor
