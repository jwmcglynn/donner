#pragma once
/// @file
/// EditorShell's direct-to-framebuffer presentation seam: converting cached
/// GL/WGPU tiles and the immediate overlay snapshot into presented pixels.
/// On Geode/WGPU builds this draws the checkerboard, document tiles, and
/// selection chrome straight onto the window framebuffer (no intermediate
/// texture); the tile-geometry helpers are backend-neutral.

#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/FrameCostBreakdown.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"

#ifdef DONNER_EDITOR_WGPU
#include <memory>

#include "donner/editor/gui/EditorWindow.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/geode/GeodeWgpuUtil.h"
#endif

namespace donner::editor {

/// Tile geometry as the presenter will draw it, folding an active drag
/// preview into the tile's drag-target flag.
PresentedFrameTileGeometry PresentedGeometryFromTileView(
    const GlTextureCache::TileView& tile,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview);

/// Drag baseline shared by the active and displayed previews, or nullopt when
/// they describe different gestures.
std::optional<PresentedDragBaseline> PresentedBaselineFromDragPreviews(
    const std::optional<SelectTool::ActiveDragPreview>& activePreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedPreview);

#ifdef DONNER_EDITOR_WGPU
class FramebufferCheckerboardRenderer {
public:
  explicit FramebufferCheckerboardRenderer(std::shared_ptr<geode::GeodeDevice> device);

  [[nodiscard]] int draw(const gui::EditorWindowWgpuRenderTarget& target,
                         const Box2d& imageClipRect, double devicePixelRatio);

private:
  struct ScissorRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };

  [[nodiscard]] bool ensureResources();
  [[nodiscard]] static ScissorRect ScissorRectFromScreenBox(const Box2d& screenBox,
                                                            double devicePixelRatio,
                                                            const Vector2i& framebufferSizePx);

  // The render pipeline and bind group layout are shared device-lifetime
  // objects owned by GeodeDevice (issue #575); only the per-renderer uniform
  // buffer and bind group live here.
  std::shared_ptr<geode::GeodeDevice> device_;
  geode::ScopedWgpuHandle<wgpu::BindGroup> bindGroup_;
  geode::ScopedWgpuHandle<wgpu::Buffer> uniformBuffer_;
};

/// Draw the checkerboard + presented document tiles directly onto the window
/// framebuffer.
FrameCostBreakdown::DirectPresentation DrawDocumentPresentationToFramebuffer(
    FramebufferCheckerboardRenderer& checkerboardRenderer, svg::RendererGeode& renderer,
    const gui::EditorWindowWgpuRenderTarget& target, const ViewportState& viewport,
    const Box2d& imageClipRect, const std::vector<GlTextureCache::TileView>& overviewTiles,
    const std::vector<GlTextureCache::TileView>& tiles,
    const std::optional<SelectTool::ActiveDragPreview>& activeDragPreview,
    const std::optional<SelectTool::ActiveDragPreview>& displayedDragPreview,
    Entity suppressedLayerEntity, bool suppressDragTargetTiles);

/// Draw the immediate overlay snapshot (selection chrome, pen previews)
/// directly onto the window framebuffer.
void DrawImmediateOverlaySnapshotToFramebuffer(svg::RendererGeode& renderer,
                                               const gui::EditorWindowWgpuRenderTarget& target,
                                               const ViewportState& viewport,
                                               const Box2d& imageClipRect,
                                               const SelectionChromeSnapshot& snapshot);
#endif

}  // namespace donner::editor
