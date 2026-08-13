#pragma once
/// @file
/// EditorShell's direct-to-framebuffer presentation seam: converting cached
/// GL/WGPU tiles and the immediate chrome snapshot into presented pixels.
/// On Geode/WGPU builds this draws the checkerboard, document tiles, and
/// selection chrome straight onto the window framebuffer, in that order, in
/// one frame and with one transform; the tile-geometry helpers are
/// backend-neutral.

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

/**
 * Transform from document coordinates into window-framebuffer device pixels for
 * one presented frame.
 *
 * The single source of the presented frame's placement: document tiles and
 * editor chrome both derive their transform from this, from the same
 * `ViewportState`, in the same frame. Nothing else may compute it, or the two
 * can disagree and the chrome annotates pixels that moved.
 *
 * @param viewport Viewport this frame's document pixels are placed with.
 */
[[nodiscard]] Transform2d PresentedFramebufferFromDocumentTransform(const ViewportState& viewport);

/**
 * Return @p snapshot re-pointed at the transform @p presentedViewport places
 * this frame's document pixels with.
 *
 * Capture stamps the transform it happened to sample against, which during a
 * gesture is one or more frames ahead of the pixels on screen. Re-pointing here
 * is what collapses that gap: the chrome cannot be drawn with any transform
 * other than the presented one.
 *
 * @param presentedViewport Viewport this frame's document pixels landed in.
 * @param snapshot Captured chrome geometry.
 */
[[nodiscard]] SelectionChromeSnapshot ChromePlacedOnPresentedDocument(
    const ViewportState& presentedViewport, SelectionChromeSnapshot snapshot);

/// Everything the immediate chrome pass needs to draw one frame of editor
/// chrome. Captured by value because the direct-render callback runs later in
/// the frame, after the shell's borrowed snapshot reference would go stale.
struct ImmediateChromePlan {
  /// Viewport this frame's document pixels were placed with. The chrome is
  /// drawn with the transform derived from this, which is the same transform
  /// \ref DrawDocumentPresentationToFramebuffer placed the tiles with.
  ViewportState viewport;
  /// Render-pane rect in screen pixels; chrome is clipped to it.
  Box2d paneClipRect;
  /// Chrome geometry, anchored in document space and sized at draw time.
  SelectionChromeSnapshot snapshot;
};

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

/// Draw editor chrome straight onto the window framebuffer, above the document
/// tiles and below ImGui.
///
/// The snapshot is re-pointed at @p viewport before drawing, so the chrome uses
/// the exact transform \ref DrawDocumentPresentationToFramebuffer placed this
/// frame's tiles with. Chrome sizes resolve from that same transform, which is
/// why handles keep a constant screen size across zoom.
///
/// @param renderer Geode renderer bound to the window framebuffer.
/// @param target Window framebuffer this frame draws into.
/// @param viewport Viewport this frame's document pixels were placed with.
/// @param paneClipRect Render-pane rect in screen pixels; chrome is clipped to it.
/// @param snapshot Chrome geometry captured from the DOM.
/// @return Wall time spent drawing chrome, in milliseconds.
double DrawImmediateChromeToFramebuffer(svg::RendererGeode& renderer,
                                        const gui::EditorWindowWgpuRenderTarget& target,
                                        const ViewportState& viewport, const Box2d& paneClipRect,
                                        const SelectionChromeSnapshot& snapshot);
#endif

}  // namespace donner::editor
