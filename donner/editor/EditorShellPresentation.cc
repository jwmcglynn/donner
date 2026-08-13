/// @file
/// Implementation of EditorShell's direct-to-framebuffer presentation seam.

#include "donner/editor/EditorShellPresentation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "donner/editor/RenderPanePresenter.h"
#include "donner/editor/TracyWrapper.h"

#ifdef DONNER_EDITOR_WGPU
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#endif

namespace donner::editor {

Transform2d PresentedFramebufferFromDocumentTransform(const ViewportState& viewport) {
  const double devicePixelsPerDocUnit = viewport.devicePixelsPerDocUnit();
  const Vector2d framebufferOriginFromDocumentOrigin =
      viewport.panScreenPoint * viewport.devicePixelRatio -
      viewport.panDocPoint * devicePixelsPerDocUnit;

  Transform2d framebufferFromDocument(Transform2d::uninitialized);
  framebufferFromDocument.data[0] = devicePixelsPerDocUnit;
  framebufferFromDocument.data[1] = 0.0;
  framebufferFromDocument.data[2] = 0.0;
  framebufferFromDocument.data[3] = devicePixelsPerDocUnit;
  framebufferFromDocument.data[4] = framebufferOriginFromDocumentOrigin.x;
  framebufferFromDocument.data[5] = framebufferOriginFromDocumentOrigin.y;
  return framebufferFromDocument;
}

SelectionChromeSnapshot ChromePlacedOnPresentedDocument(const ViewportState& presentedViewport,
                                                        SelectionChromeSnapshot snapshot) {
  snapshot.canvasFromDoc = PresentedFramebufferFromDocumentTransform(presentedViewport);
  return snapshot;
}

#ifdef DONNER_EDITOR_WGPU
namespace {

double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace
#endif

PresentedFrameTileGeometry PresentedGeometryFromTileView(
    const GlTextureCache::TileView& tile,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview) {
  PresentedFrameTileGeometry geometry{
      .canvasOffsetDoc = tile.canvasOffsetDoc,
      .bitmapDimsDoc = tile.bitmapDimsDoc,
      .dragTranslationDoc = tile.dragTranslationDoc,
      .documentFromCachedDocument = tile.documentFromCachedDocument,
      .isDragTarget = tile.isDragTarget,
  };
  if (TileMatchesActiveDragPreview(tile, activeDragPreview)) {
    geometry.isDragTarget = true;
  }
  return geometry;
}

std::optional<PresentedDragBaseline> PresentedBaselineFromDragPreviews(
    const std::optional<SelectTool::ActiveDragPreview>& activePreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedPreview) {
  if (!activePreview.has_value() || !displayedPreview.has_value() ||
      activePreview->entity != displayedPreview->entity ||
      activePreview->dragGeneration != displayedPreview->dragGeneration) {
    return std::nullopt;
  }

  return PresentedDragBaseline{
      .entity = activePreview->entity,
      .representedTranslationDoc = displayedPreview->translation,
      .activeTranslationDoc = activePreview->translation,
      .representedDocumentFromCachedDocument = displayedPreview->documentFromCachedDocument,
      .activeDocumentFromCachedDocument = activePreview->documentFromCachedDocument,
  };
}

#ifdef DONNER_EDITOR_WGPU

namespace {

Box2d PresentedTileQuadBounds(const PresentedTileQuad& tileQuad) {
  Box2d bounds = Box2d::CreateEmpty(tileQuad.topLeft);
  bounds.addPoint(tileQuad.topRight);
  bounds.addPoint(tileQuad.bottomRight);
  bounds.addPoint(tileQuad.bottomLeft);
  return bounds;
}

Box2d FramebufferBoxFromScreenBox(const Box2d& screenBox, double devicePixelRatio) {
  return Box2d(screenBox.topLeft * devicePixelRatio, screenBox.bottomRight * devicePixelRatio);
}

std::optional<Transform2d> FramebufferFromTextureTransform(const PresentedTileQuad& tileQuad,
                                                           const Vector2i& textureSizePx) {
  if (textureSizePx.x <= 0 || textureSizePx.y <= 0) {
    return std::nullopt;
  }

  const Vector2d sourceSize(static_cast<double>(textureSizePx.x),
                            static_cast<double>(textureSizePx.y));
  Transform2d framebufferFromTexture(Transform2d::uninitialized);
  framebufferFromTexture.data[0] = (tileQuad.topRight.x - tileQuad.topLeft.x) / sourceSize.x;
  framebufferFromTexture.data[1] = (tileQuad.topRight.y - tileQuad.topLeft.y) / sourceSize.x;
  framebufferFromTexture.data[2] = (tileQuad.bottomLeft.x - tileQuad.topLeft.x) / sourceSize.y;
  framebufferFromTexture.data[3] = (tileQuad.bottomLeft.y - tileQuad.topLeft.y) / sourceSize.y;
  framebufferFromTexture.data[4] = tileQuad.topLeft.x;
  framebufferFromTexture.data[5] = tileQuad.topLeft.y;
  return framebufferFromTexture;
}

}  // namespace

FrameCostBreakdown::DirectPresentation DrawDocumentPresentationToFramebuffer(
    FramebufferCheckerboardRenderer& checkerboardRenderer, svg::RendererGeode& renderer,
    const gui::EditorWindowWgpuRenderTarget& target, const ViewportState& viewport,
    const Box2d& imageClipRect, const std::vector<GlTextureCache::TileView>& overviewTiles,
    const std::vector<GlTextureCache::TileView>& tiles,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview,
    Entity suppressedLayerEntity, bool suppressDragTargetTiles) {
  FrameCostBreakdown::DirectPresentation cost;
  const auto totalStart = std::chrono::steady_clock::now();
  if (!target.texture || target.framebufferSizePx.x <= 0 || target.framebufferSizePx.y <= 0) {
    return cost;
  }

  svg::RenderViewport renderViewport;
  renderViewport.size = Vector2d(static_cast<double>(target.framebufferSizePx.x),
                                 static_cast<double>(target.framebufferSizePx.y));
  renderViewport.devicePixelRatio = 1.0;

  const auto checkerboardStart = std::chrono::steady_clock::now();
  cost.checkerboardDrawCount =
      checkerboardRenderer.draw(target, imageClipRect, viewport.devicePixelRatio);
  cost.checkerboardMs = ElapsedMs(checkerboardStart);

  renderer.setTargetTexture(target.texture);
  renderer.setPreserveTargetOnBeginFrame(true);
  renderer.beginFrame(renderViewport);

  svg::ResolvedClip clip;
  clip.clipRect = FramebufferBoxFromScreenBox(imageClipRect, viewport.devicePixelRatio);
  renderer.pushClip(clip);
  renderer.setTransform(Transform2d());

  const Transform2d framebufferFromCanvasTransform =
      PresentedFramebufferFromDocumentTransform(viewport);
  const std::optional<PresentedDragBaseline> dragBaseline =
      PresentedBaselineFromDragPreviews(activeDragPreview, displayedDragPreview);
  const Box2d framebufferClipRect =
      FramebufferBoxFromScreenBox(imageClipRect, viewport.devicePixelRatio);

  const auto computeTileQuad = [&](const GlTextureCache::TileView& tile) {
    if (tile.textureSnapshot == nullptr ||
        !ShouldPresentCompositedTile(tile, suppressedLayerEntity, suppressDragTargetTiles)) {
      return std::optional<PresentedTileQuad>();
    }
    if (suppressDragTargetTiles && TileMatchesActiveDragPreview(tile, activeDragPreview)) {
      return std::optional<PresentedTileQuad>();
    }

    const std::optional<PresentedTileQuad> tileQuad =
        ComputePresentedTileQuad(PresentedGeometryFromTileView(tile, activeDragPreview),
                                 framebufferFromCanvasTransform, dragBaseline);
    if (!tileQuad.has_value() ||
        !PresentedTileQuadIntersectsScreenRect(*tileQuad, framebufferClipRect)) {
      return std::optional<PresentedTileQuad>();
    }
    return tileQuad;
  };
  const auto drawTile = [&](const GlTextureCache::TileView& tile) {
    const std::optional<PresentedTileQuad> tileQuad = computeTileQuad(tile);
    if (!tileQuad.has_value()) {
      return false;
    }

    const Vector2i textureSizePx = tile.textureSnapshot->dimensions();
    const std::optional<Transform2d> framebufferFromTexture =
        FramebufferFromTextureTransform(*tileQuad, textureSizePx);
    if (!framebufferFromTexture.has_value()) {
      return false;
    }

    renderer.setTransform(*framebufferFromTexture);
    renderer.drawTextureSnapshot(
        *tile.textureSnapshot,
        Box2d(Vector2d::Zero(), Vector2d(static_cast<double>(textureSizePx.x),
                                         static_cast<double>(textureSizePx.y))));
    return true;
  };

  if (!overviewTiles.empty()) {
    const auto overviewStart = std::chrono::steady_clock::now();
    std::vector<Box2d> activeTileBounds;
    activeTileBounds.reserve(tiles.size());
    for (const GlTextureCache::TileView& tile : tiles) {
      const std::optional<PresentedTileQuad> tileQuad = computeTileQuad(tile);
      if (tileQuad.has_value()) {
        activeTileBounds.push_back(PresentedTileQuadBounds(*tileQuad));
      }
      if (TileMatchesActiveDragPreview(tile, activeDragPreview)) {
        const std::optional<PresentedTileQuad> cachedTileQuad =
            ComputePresentedTileQuad(PresentedGeometryFromTileView(tile, std::nullopt),
                                     framebufferFromCanvasTransform, std::nullopt);
        if (cachedTileQuad.has_value() &&
            PresentedTileQuadIntersectsScreenRect(*cachedTileQuad, framebufferClipRect)) {
          activeTileBounds.push_back(PresentedTileQuadBounds(*cachedTileQuad));
        }
      }
    }

    const std::vector<Box2d> overviewClipRects =
        SubtractPresentedTileBoundsFromClip(framebufferClipRect, activeTileBounds);
    for (const Box2d& overviewClipRect : overviewClipRects) {
      svg::ResolvedClip overviewClip;
      overviewClip.clipRect = overviewClipRect;
      // pushClip composes the rect with the CURRENT transform, and drawTile
      // leaves the previous tile's texture transform active - reset to
      // identity so every subtract-rect scissors in framebuffer pixels.
      // Without this only the first rect clips correctly and the remaining
      // overview infill regions show checkerboard (high-zoom zoom/pan
      // clipping).
      renderer.setTransform(Transform2d());
      renderer.pushClip(overviewClip);
      for (const GlTextureCache::TileView& tile : overviewTiles) {
        if (drawTile(tile)) {
          ++cost.overviewTileDrawCount;
        }
      }
      renderer.popClip();
    }
    cost.overviewTilesMs = ElapsedMs(overviewStart);
  }
  const auto activeTilesStart = std::chrono::steady_clock::now();
  for (const GlTextureCache::TileView& tile : tiles) {
    if (drawTile(tile)) {
      ++cost.activeTileDrawCount;
    }
  }
  cost.activeTilesMs = ElapsedMs(activeTilesStart);

  renderer.popClip();
  const auto rendererEndFrameStart = std::chrono::steady_clock::now();
  renderer.endFrame();
  cost.rendererEndFrameMs = ElapsedMs(rendererEndFrameStart);
  renderer.clearTargetTexture();
  cost.totalMs = ElapsedMs(totalStart);
  return cost;
}

double DrawImmediateChromeToFramebuffer(svg::RendererGeode& renderer,
                                        const gui::EditorWindowWgpuRenderTarget& target,
                                        const ViewportState& viewport, const Box2d& paneClipRect,
                                        const SelectionChromeSnapshot& snapshot) {
  const auto start = std::chrono::steady_clock::now();
  if (!target.texture || target.framebufferSizePx.x <= 0 || target.framebufferSizePx.y <= 0) {
    return 0.0;
  }

  svg::RenderViewport renderViewport;
  renderViewport.size = Vector2d(static_cast<double>(target.framebufferSizePx.x),
                                 static_cast<double>(target.framebufferSizePx.y));
  renderViewport.devicePixelRatio = 1.0;

  renderer.setTargetTexture(target.texture);
  renderer.setPreserveTargetOnBeginFrame(true);
  renderer.beginFrame(renderViewport);

  svg::ResolvedClip clip;
  clip.clipRect = FramebufferBoxFromScreenBox(paneClipRect, viewport.devicePixelRatio);
  renderer.setTransform(Transform2d());
  renderer.pushClip(clip);

  // What makes chrome/content desync impossible: chrome is placed with the
  // transform the tiles were placed with this frame, from the same viewport and
  // the same function, not the one capture happened to sample.
  OverlayRenderer::drawChromeFromSnapshot(renderer,
                                          ChromePlacedOnPresentedDocument(viewport, snapshot));

  renderer.popClip();
  renderer.endFrame();
  renderer.clearTargetTexture();
  return ElapsedMs(start);
}

FramebufferCheckerboardRenderer::FramebufferCheckerboardRenderer(
    std::shared_ptr<geode::GeodeDevice> device)
    : device_(std::move(device)) {}

std::optional<geode::CheckerboardScissorPx>
FramebufferCheckerboardRenderer::ScissorRectFromScreenBox(const Box2d& screenBox,
                                                          double devicePixelRatio,
                                                          const Vector2i& framebufferSizePx) {
  if (devicePixelRatio <= 0.0 || framebufferSizePx.x <= 0 || framebufferSizePx.y <= 0) {
    return std::nullopt;
  }

  const double maxX = static_cast<double>(framebufferSizePx.x);
  const double maxY = static_cast<double>(framebufferSizePx.y);
  const double left = std::clamp(std::floor(screenBox.topLeft.x * devicePixelRatio), 0.0, maxX);
  const double top = std::clamp(std::floor(screenBox.topLeft.y * devicePixelRatio), 0.0, maxY);
  const double right = std::clamp(std::ceil(screenBox.bottomRight.x * devicePixelRatio), 0.0, maxX);
  const double bottom =
      std::clamp(std::ceil(screenBox.bottomRight.y * devicePixelRatio), 0.0, maxY);
  if (left >= right || top >= bottom) {
    return std::nullopt;
  }

  return geode::CheckerboardScissorPx{
      .x = static_cast<std::uint32_t>(left),
      .y = static_cast<std::uint32_t>(top),
      .width = static_cast<std::uint32_t>(right - left),
      .height = static_cast<std::uint32_t>(bottom - top),
  };
}

int FramebufferCheckerboardRenderer::draw(const gui::EditorWindowWgpuRenderTarget& target,
                                          const Box2d& imageClipRect, double devicePixelRatio) {
  if (device_ == nullptr || !target.texture) {
    return 0;
  }

  const std::optional<geode::CheckerboardScissorPx> scissor =
      ScissorRectFromScreenBox(imageClipRect, devicePixelRatio, target.framebufferSizePx);
  if (!scissor.has_value()) {
    return 0;
  }

  // Appearance comes from the shared checkerboard constants, not from local
  // literals, so every surface that shows the checkerboard lands on identical
  // cells for the same document.
  static_assert(kFramebufferCheckerboardSize == geode::kTransparencyCheckerboardCellLogicalPx);
  geode::CheckerboardUnderlayParams params;
  params.devicePixelRatio = devicePixelRatio;
  params.cellSizeLogicalPx = kFramebufferCheckerboardSize;
  // The window framebuffer *is* the anchor: the pattern is fixed to the window
  // and the document slides over it, so the target's own origin is the anchor.
  params.originOffsetPx = Vector2d::Zero();
  params.scissorPx = scissor;

  // The document tiles are drawn on top of this in the same frame, so the
  // checkerboard overwrites the scissored region rather than blending under it.
  return checkerboardPass_.draw(*device_, target.texture, target.framebufferSizePx, params,
                                geode::GeodeCheckerboardPipeline::BlendMode::Replace)
             ? 1
             : 0;
}

#endif

}  // namespace donner::editor
