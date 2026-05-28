#pragma once
/// @file

#include <cstdint>

#include "donner/base/Vector2.h"

namespace donner::editor {

/// Per-frame cost counters for editor rendering diagnostics.
struct FrameCostBreakdown {
  /// Cost counters for rasterizing and uploading selection/source-hover chrome.
  struct Overlay {
    /// Milliseconds spent capturing live DOM selection chrome into a snapshot.
    double captureMs = 0.0;
    /// Milliseconds spent drawing the captured chrome snapshot.
    double drawMs = 0.0;
    /// Milliseconds spent ending the renderer frame and acquiring the presentation payload.
    double snapshotMs = 0.0;
    /// Milliseconds spent handing the overlay payload to the presentation texture cache.
    double uploadMs = 0.0;
    /// Approximate overlay payload bytes retained or uploaded for presentation.
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
    /// Document canvas size used for the overlay raster.
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

  Overlay overlay;
  CompositedUpload compositedUpload;
  SourceRopes sourceRopes;

  /// Cumulative number of full-document canvas-size commits since document load.
  std::uint64_t documentCanvasCommitCount = 0;
  /// Most recent full-document canvas size committed through the SVG document.
  Vector2i lastCommittedCanvasSize = Vector2i::Zero();
};

}  // namespace donner::editor
