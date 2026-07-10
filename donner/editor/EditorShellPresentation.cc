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

Transform2d FramebufferFromDocumentTransform(const ViewportState& viewport) {
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

  const Transform2d framebufferFromCanvasTransform = FramebufferFromDocumentTransform(viewport);
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

void DrawImmediateOverlaySnapshotToFramebuffer(svg::RendererGeode& renderer,
                                               const gui::EditorWindowWgpuRenderTarget& target,
                                               const ViewportState& viewport,
                                               const Box2d& imageClipRect,
                                               const SelectionChromeSnapshot& snapshot) {
  if (!target.texture || target.framebufferSizePx.x <= 0 || target.framebufferSizePx.y <= 0) {
    return;
  }

  SelectionChromeSnapshot framebufferSnapshot = snapshot;
  framebufferSnapshot.canvasFromDoc = FramebufferFromDocumentTransform(viewport);

  svg::RenderViewport renderViewport;
  renderViewport.size = Vector2d(static_cast<double>(target.framebufferSizePx.x),
                                 static_cast<double>(target.framebufferSizePx.y));
  renderViewport.devicePixelRatio = 1.0;

  renderer.setTargetTexture(target.texture);
  renderer.setPreserveTargetOnBeginFrame(true);
  renderer.beginFrame(renderViewport);
  svg::ResolvedClip clip;
  clip.clipRect = FramebufferBoxFromScreenBox(imageClipRect, viewport.devicePixelRatio);
  renderer.pushClip(clip);
  OverlayRenderer::drawChromeFromSnapshot(renderer, framebufferSnapshot);
  renderer.popClip();
  renderer.endFrame();
  renderer.clearTargetTexture();
}

FramebufferCheckerboardRenderer::FramebufferCheckerboardRenderer(
    std::shared_ptr<geode::GeodeDevice> device)
    : device_(std::move(device)) {}

FramebufferCheckerboardRenderer::ScissorRect
FramebufferCheckerboardRenderer::ScissorRectFromScreenBox(const Box2d& screenBox,
                                                          double devicePixelRatio,
                                                          const Vector2i& framebufferSizePx) {
  if (devicePixelRatio <= 0.0 || framebufferSizePx.x <= 0 || framebufferSizePx.y <= 0) {
    return ScissorRect{};
  }

  const double maxX = static_cast<double>(framebufferSizePx.x);
  const double maxY = static_cast<double>(framebufferSizePx.y);
  const double left = std::clamp(std::floor(screenBox.topLeft.x * devicePixelRatio), 0.0, maxX);
  const double top = std::clamp(std::floor(screenBox.topLeft.y * devicePixelRatio), 0.0, maxY);
  const double right = std::clamp(std::ceil(screenBox.bottomRight.x * devicePixelRatio), 0.0, maxX);
  const double bottom =
      std::clamp(std::ceil(screenBox.bottomRight.y * devicePixelRatio), 0.0, maxY);
  if (left >= right || top >= bottom) {
    return ScissorRect{};
  }

  return ScissorRect{
      .x = static_cast<std::uint32_t>(left),
      .y = static_cast<std::uint32_t>(top),
      .width = static_cast<std::uint32_t>(right - left),
      .height = static_cast<std::uint32_t>(bottom - top),
  };
}

bool FramebufferCheckerboardRenderer::ensureResources() {
  if (bindGroup_) {
    return true;
  }
  if (device_ == nullptr) {
    return false;
  }

  // The pipeline and bind group layout are compiled once per GeodeDevice and
  // shared across consumers (issue #575); only the uniform buffer and its
  // bind group are per-renderer.
  const geode::GeodeCheckerboardPipeline& checkerboard = device_->checkerboardPipeline();
  if (!checkerboard.valid()) {
    return false;
  }

  const wgpu::Device& device = device_->device();
  wgpu::BufferDescriptor bufferDesc = {};
  bufferDesc.label = geode::wgpuLabel("EditorFramebufferCheckerboardUniforms");
  bufferDesc.size = sizeof(geode::GeodeCheckerboardPipeline::Uniforms);
  bufferDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  uniformBuffer_.reset(device.createBuffer(bufferDesc));
  device_->countBuffer();
  if (!uniformBuffer_) {
    return false;
  }

  wgpu::BindGroupEntry bindGroupEntry = {};
  bindGroupEntry.binding = 0;
  bindGroupEntry.buffer = uniformBuffer_.get();
  bindGroupEntry.offset = 0;
  bindGroupEntry.size = sizeof(geode::GeodeCheckerboardPipeline::Uniforms);

  wgpu::BindGroupDescriptor bindGroupDesc = {};
  bindGroupDesc.label = geode::wgpuLabel("EditorFramebufferCheckerboardBG");
  bindGroupDesc.layout = checkerboard.bindGroupLayout();
  bindGroupDesc.entryCount = 1;
  bindGroupDesc.entries = &bindGroupEntry;
  bindGroup_.reset(device.createBindGroup(bindGroupDesc));
  device_->countBindGroup();
  return static_cast<bool>(bindGroup_);
}

int FramebufferCheckerboardRenderer::draw(const gui::EditorWindowWgpuRenderTarget& target,
                                          const Box2d& imageClipRect, double devicePixelRatio) {
  if (!target.texture || target.framebufferSizePx.x <= 0 || target.framebufferSizePx.y <= 0 ||
      device_ == nullptr || !ensureResources()) {
    return 0;
  }

  const ScissorRect scissor =
      ScissorRectFromScreenBox(imageClipRect, devicePixelRatio, target.framebufferSizePx);
  if (scissor.width == 0 || scissor.height == 0) {
    return 0;
  }

  const geode::GeodeCheckerboardPipeline::Uniforms uniforms{
      .targetSize = {static_cast<float>(target.framebufferSizePx.x),
                     static_cast<float>(target.framebufferSizePx.y)},
      .devicePixelRatio = static_cast<float>(devicePixelRatio),
      .checkerSize = static_cast<float>(kFramebufferCheckerboardSize),
      .darkColor = {40.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 1.0f},
      .lightColor = {60.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f, 1.0f},
  };
  device_->queue().writeBuffer(uniformBuffer_.get(), 0, &uniforms, sizeof(uniforms));

  geode::ScopedWgpuHandle<wgpu::TextureView> view(target.texture.createView());
  if (!view) {
    return 0;
  }

  wgpu::CommandEncoderDescriptor encoderDesc = {};
  encoderDesc.label = geode::wgpuLabel("EditorFramebufferCheckerboardEncoder");
  geode::ScopedWgpuHandle<wgpu::CommandEncoder> encoder(
      device_->device().createCommandEncoder(encoderDesc));
  if (!encoder) {
    return 0;
  }

  wgpu::RenderPassColorAttachment color = {};
  color.view = view.get();
  color.loadOp = wgpu::LoadOp::Load;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.0, 0.0, 0.0, 0.0};
  color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

  wgpu::RenderPassDescriptor passDesc = {};
  passDesc.label = geode::wgpuLabel("EditorFramebufferCheckerboardPass");
  passDesc.colorAttachmentCount = 1;
  passDesc.colorAttachments = &color;
  geode::ScopedWgpuHandle<wgpu::RenderPassEncoder> pass(encoder.get().beginRenderPass(passDesc));
  if (!pass) {
    return 0;
  }

  pass.get().setScissorRect(scissor.x, scissor.y, scissor.width, scissor.height);
  pass.get().setPipeline(device_->checkerboardPipeline().pipeline());
  pass.get().setBindGroup(0, bindGroup_.get(), 0, nullptr);
  pass.get().draw(3, 1, 0, 0);
  device_->countPipelineSwitch();
  device_->countDraw();
  pass.get().end();
  pass.reset();

  geode::ScopedWgpuHandle<wgpu::CommandBuffer> commands(encoder.get().finish());
  if (!commands) {
    return 0;
  }
  device_->queue().submit(1, &commands.get());
  device_->countSubmit();
  return 1;
}

#endif

}  // namespace donner::editor
