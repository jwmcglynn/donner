#pragma once
/// @file

#include <cstddef>
#include <cstdint>

#include "donner/base/Vector2.h"

namespace donner::editor {

/// Per-frame cost counters for editor rendering diagnostics.
struct FrameCostBreakdown {
  /// Editor-shell work that runs on the UI thread during one `runFrame`.
  struct MainFrame {
    /// Early per-frame bookkeeping before polling renderer results.
    double preparationMs = 0.0;
    /// Time spent accepting a completed async render result.
    double renderPollMs = 0.0;
    /// Time spent flushing queued DOM/source mutations.
    double documentFlushMs = 0.0;
    /// Time spent refreshing overlay chrome outside normal pane rendering.
    double overlayRefreshMs = 0.0;
    /// Time spent syncing parse markers and pending source writebacks.
    double documentSyncMs = 0.0;
    /// Time spent computing editor pane layout and viewport layout.
    double layoutMs = 0.0;
    /// Time spent handling global keyboard shortcuts.
    double shortcutsMs = 0.0;
    /// Time spent rendering the menu bar and modal dialogs.
    double menusDialogsMs = 0.0;
    /// Time spent rendering the XML source pane, including source-focus ropes.
    double sourcePaneMs = 0.0;
    /// Time spent rendering the central document pane widgets.
    double renderPaneMs = 0.0;
    /// Time spent rendering tree/layer/inspector sidebars.
    double sidebarsMs = 0.0;
    /// Time spent rendering pane splitters and the floating layer panel.
    double splittersMs = 0.0;
    /// Time spent issuing an end-of-frame document render request.
    double endRenderRequestMs = 0.0;
  };

  /// Host-window work bracketing the editor frame.
  struct HostFrame {
    /// Time spent starting the current ImGui frame.
    double beginFrameMs = 0.0;
    /// Total time spent ending and presenting the previous host frame.
    double previousEndFrameMs = 0.0;
    /// Previous end-frame time spent in `ImGui::Render`.
    double previousImguiRenderMs = 0.0;
    /// Previous end-frame time spent acquiring the WGPU surface texture.
    double previousSurfaceAcquireMs = 0.0;
    /// Previous end-frame time spent in the document underlay direct pass.
    double previousUnderlayMs = 0.0;
    /// Previous end-frame time spent issuing ImGui backend draw commands.
    double previousImguiDrawMs = 0.0;
    /// Previous end-frame time spent in the overlay/direct append pass.
    double previousDirectMs = 0.0;
    /// Previous end-frame time spent reading the framebuffer back to the CPU.
    double previousReadbackMs = 0.0;
    /// Previous end-frame time spent presenting or swapping the surface.
    double previousPresentMs = 0.0;
  };

  /// Direct WGPU document presentation work completed in the previous host frame.
  struct DirectPresentation {
    /// Total time spent inside the document underlay callback.
    double totalMs = 0.0;
    /// Time spent drawing the transparent-canvas checkerboard.
    double checkerboardMs = 0.0;
    /// Time spent drawing retained overview tiles under active tiles.
    double overviewTilesMs = 0.0;
    /// Time spent drawing active document tiles.
    double activeTilesMs = 0.0;
    /// Time spent ending the Geode renderer frame.
    double rendererEndFrameMs = 0.0;
    /// GPU draws submitted for the transparent-canvas checkerboard.
    int checkerboardDrawCount = 0;
    /// Overview tiles drawn.
    int overviewTileDrawCount = 0;
    /// Active document tiles drawn.
    int activeTileDrawCount = 0;
  };

  /// Cost counters for capturing and presenting selection/source-hover chrome.
  struct Overlay {
    /// Milliseconds spent capturing live DOM selection chrome into a snapshot.
    double captureMs = 0.0;
    /// Milliseconds spent drawing the captured chrome snapshot into a retained overlay payload.
    /// Zero for the editor's immediate screen-space overlay path.
    double drawMs = 0.0;
    /// Milliseconds spent ending the renderer frame and acquiring a retained overlay payload. Zero
    /// for the editor's immediate screen-space overlay path.
    double snapshotMs = 0.0;
    /// Milliseconds spent handing a retained overlay payload to the presentation texture cache.
    /// Zero for the editor's immediate screen-space overlay path.
    double uploadMs = 0.0;
    /// Approximate retained overlay payload bytes. Zero for immediate screen-space presentation.
    std::uint64_t payloadBytes = 0;
    /// Document elements selected when the overlay snapshot was captured.
    int selectedElementCount = 0;
    /// Source-hover elements included in the overlay snapshot.
    int sourceHoverElementCount = 0;
    /// Selection path items captured from geometry leaves.
    int pathCount = 0;
    /// Source-hover path items captured from geometry leaves.
    int hoverPathCount = 0;
    /// Selection axis-aligned bounding boxes captured.
    int aabbCount = 0;
    /// Source-hover axis-aligned bounding boxes captured.
    int hoverAabbCount = 0;
    /// Selection transform handles captured.
    int handleCount = 0;
    /// True when the overlay snapshot included a marquee rectangle.
    bool hasMarquee = false;
    /// True when selected chrome used the large-selection combined-bounds LOD path.
    bool selectionBoundsOnly = false;
    /// True when a live drag preview was available while capturing the overlay.
    bool hasLiveDragPreview = false;
    /// True when overlay chrome was projected to a represented drag preview for presentation.
    bool hasRepresentedDragPreview = false;
    /// Live document-space drag translation at overlay capture time.
    Vector2d liveDragTranslationDoc = Vector2d::Zero();
    /// Document-space drag translation represented by the overlay presented this frame.
    Vector2d representedDragTranslationDoc = Vector2d::Zero();
    /// Viewport overlay size that would have been rasterized before immediate presentation.
    Vector2i canvasSize = Vector2i::Zero();
  };

  /// Cost counters for uploading compositor tiles into presentation textures.
  struct CompositedUpload {
    /// Milliseconds spent in the presentation texture cache upload path.
    double uploadMs = 0.0;
    /// Approximate bytes carried by tiles with fresh bitmap or GPU texture payloads.
    std::uint64_t payloadBytes = 0;
    /// Pixel area carried by tiles with fresh bitmap or GPU texture payloads.
    std::uint64_t payloadPixelArea = 0;
    /// Pixel area represented by every tile, including metadata-only reuse.
    std::uint64_t tilePixelArea = 0;
    /// Number of compositor tiles in the uploaded preview.
    int tileCount = 0;
    /// Number of tiles with a fresh bitmap or GPU texture payload.
    int payloadTileCount = 0;
    /// Number of tiles with a fresh CPU bitmap payload.
    int bitmapPayloadTileCount = 0;
    /// Number of tiles with a fresh backend texture payload.
    int texturePayloadTileCount = 0;
    /// Number of metadata-only tiles that reused an existing presentation texture.
    int metadataOnlyTileCount = 0;
    /// Number of transient immediate-mode compositor tiles.
    int immediateTileCount = 0;
  };

  /// Worker-side compositor raster costs for the render result that landed this UI frame.
  struct CompositedRender {
    /// Milliseconds spent rendering transient immediate-mode spans.
    double immediateMs = 0.0;
    /// Milliseconds spent rendering retained cached segment/layer tiles.
    double cachedMs = 0.0;
    /// Immediate-mode static spans rendered this frame.
    int immediateTileCount = 0;
    /// Cached segment/layer tiles rendered this frame.
    int cachedTileCount = 0;
  };

  /// Cost counters for source-focus reference rope layout, simulation, and drawing.
  struct SourceRopes {
    /// Milliseconds spent mapping source reference links to screen-space connector layout.
    double layoutMs = 0.0;
    /// Milliseconds spent updating rope simulation/path/hit-test state.
    double updateMs = 0.0;
    /// Milliseconds spent issuing ImGui draw commands for source reference links.
    double drawMs = 0.0;
    /// Number of links considered from the active focus partition.
    int candidateCount = 0;
    /// Number of links with a visible screen-space connector layout.
    int laidOutCount = 0;
    /// Number of links culled before draw because their route is outside the visible text region.
    int culledCount = 0;
    /// Number of links drawn this frame.
    int drawnCount = 0;
    /// Number of overflow links drawn as static connectors instead of animated ropes.
    int staticDrawnCount = 0;
    /// Number of active rope simulation states retained after pruning.
    int activeStateCount = 0;
  };

  MainFrame mainFrame;
  HostFrame hostFrame;
  DirectPresentation directPresentation;
  Overlay overlay;
  CompositedUpload compositedUpload;
  CompositedRender compositedRender;
  SourceRopes sourceRopes;

  /// Cumulative number of full-document canvas-size commits since document load.
  std::uint64_t documentCanvasCommitCount = 0;
  /// Cumulative number of render requests posted to the async worker since
  /// document load. Lets tests assert gesture-time scheduling behavior.
  std::uint64_t renderRequestsPosted = 0;
  /// Most recent full-document canvas size committed through the SVG document.
  Vector2i lastCommittedCanvasSize = Vector2i::Zero();
};

}  // namespace donner::editor
