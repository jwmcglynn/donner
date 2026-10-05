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
#include "donner/editor/FramePresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/SelectTool.h"
#include "donner/editor/ViewportState.h"

#ifdef DONNER_EDITOR_WGPU
#include <memory>

#include "donner/editor/gui/EditorWindow.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeCheckerboardPipeline.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#endif

namespace donner::editor {

/// Convert an already resolved tile into the shared quad geometry type.
/// @param tile Tile from a sealed frame.
PresentedFrameTileGeometry PresentedGeometryFromTileView(const GlTextureCache::TileView& tile);

#ifdef DONNER_EDITOR_WGPU
/// Draws the window framebuffer's transparency checkerboard, clipped to the
/// render pane. The pass itself is \ref geode::GeodeCheckerboardPass; this owns
/// the device and turns the pane's screen-space clip rect into its scissor.
class FramebufferCheckerboardRenderer {
public:
  /// @param device Device owning the window framebuffer.
  explicit FramebufferCheckerboardRenderer(std::shared_ptr<geode::GeodeDevice> device);

  /**
   * Draw the checkerboard behind the document, before any tiles land in the target.
   *
   * @param target Window framebuffer for this frame.
   * @param imageClipRect Render-pane rect in screen pixels; the checkerboard is clipped to it.
   * @param framebufferFromLogicalScale Physical framebuffer pixels per ImGui logical pixel.
   * @return Number of draws submitted, for the frame cost breakdown.
   */
  [[nodiscard]] int draw(const gui::EditorWindowWgpuRenderTarget& target,
                         const Box2d& imageClipRect, const Vector2d& framebufferFromLogicalScale);

private:
  /// The device-pixel scissor covering @p screenBox, or nullopt when the box is
  /// degenerate or entirely outside the framebuffer.
  [[nodiscard]] static std::optional<geode::CheckerboardScissorPx> ScissorRectFromScreenBox(
      const Box2d& screenBox, const Vector2d& framebufferFromLogicalScale,
      const Vector2i& framebufferSizePx);

  std::shared_ptr<geode::GeodeDevice> device_;
  geode::GeodeCheckerboardPass checkerboardPass_;
};

/// Draw the checkerboard + presented document tiles directly onto the window
/// framebuffer.
FrameCostBreakdown::DirectPresentation DrawDocumentPresentationToFramebuffer(
    FramebufferCheckerboardRenderer& checkerboard, svg::RendererGeode& renderer,
    const gui::EditorWindowWgpuRenderTarget& target, const FramePresentation& frame);

/// Draw editor chrome straight onto the window framebuffer, above the document
/// tiles and below ImGui.
///
/// Chrome sizes resolve from the frame camera, retaining constant screen size across zoom.
///
/// @param renderer Geode renderer bound to the window framebuffer.
/// @param target Window framebuffer this frame draws into.
/// @param frame Shared geometry, camera and clip used by the document pass.
/// @return Wall time spent drawing chrome, in milliseconds.
double DrawImmediateChromeToFramebuffer(svg::RendererGeode& renderer,
                                        const gui::EditorWindowWgpuRenderTarget& target,
                                        const FramePresentation& frame);
#endif

}  // namespace donner::editor
