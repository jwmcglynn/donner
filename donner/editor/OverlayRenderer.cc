#include "donner/editor/OverlayRenderer.h"

#include <array>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"

namespace donner::editor {

namespace {

/// Desired on-screen stroke thickness for selection chrome (path outlines
/// and AABBs), in canvas pixels. World-space stroke width is scaled down
/// by `canvasFromDoc`'s linear factor so the overlay stays 1 px
/// regardless of zoom.
constexpr double kSelectionStrokePixels = 1.0;

/// Marquee stroke thickness — matches the prior ImGui chrome exactly.
constexpr double kMarqueeStrokePixels = 1.5;

svg::PaintParams MakeSelectionStrokePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  // Bright cyan stroke, no fill.
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0x00, 0xc8, 0xff, 0xff)));
  paint.fill = svg::PaintServer::None{};
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Butt;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Miter;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

/// Translucent cyan fill, no stroke — used for the marquee fill pass.
/// Alpha 0x33 matches the prior `IM_COL32(0x00, 0xc8, 0xff, 0x33)` in
/// `RenderPanePresenter`.
svg::PaintParams MakeMarqueeFillPaint() {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid(css::Color(css::RGBA(0x00, 0xc8, 0xff, 0x33)));
  paint.stroke = svg::PaintServer::None{};
  paint.fillOpacity = 1.0;
  return paint;
}

/// Solid white stroke, no fill — the marquee's outer outline. Matches
/// the prior `IM_COL32(0xff, 0xff, 0xff, 0xff)` in `RenderPanePresenter`.
svg::PaintParams MakeMarqueeStrokePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0xff, 0xff, 0xff)));
  paint.fill = svg::PaintServer::None{};
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Butt;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Miter;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

/// Linear scale factor baked into `canvasFromDoc`. Both axes are
/// identical under `preserveAspectRatio="xMid* meet|slice"`, which is
/// the only case the editor renders today.
double LinearScale(const Transform2d& canvasFromDoc) {
  return canvasFromDoc.transformVector(Vector2d(1.0, 0.0)).length();
}

/// Inner helper that draws *just* the path outline for a single
/// geometry element. The AABB is drawn separately by the caller —
/// per the design doc, multi-select chrome shows one combined AABB
/// across the whole selection, not one box per element.
void DrawElementPathOutline(svg::Renderer& renderer, const svg::SVGElement& selected,
                            const Transform2d& canvasFromDoc, const svg::PaintParams& paint) {
  if (!selected.isa<svg::SVGGeometryElement>()) {
    return;
  }
  const auto geometry = selected.cast<svg::SVGGeometryElement>();

  // The spline is in the element's local coordinate space. To draw
  // it at the right on-screen position we chain local → world →
  // canvas. Per Donner's row-vector `A * B = apply A first, then B`
  // semantics, that's `elementFromWorld * canvasFromDoc` (the name
  // `elementFromWorld` is a legacy misnomer — the actual behavior,
  // verified by reading `RendererDriver::traverse`, is local→world,
  // same transform the main renderer feeds into `setTransform` for
  // each element's own draw calls). The composition's destFromSource
  // name is `canvasFromElement`.
  //
  // An earlier version of this code had an extra `.inverse()` which
  // silently flipped the sign of every element transform — dragging
  // a selected element right moved the path outline left. The AABB
  // path below didn't have the bug because it goes through
  // `worldBounds()` which already returns world-space coordinates.
  if (const auto spline = geometry.computedSpline(); spline.has_value()) {
    const Transform2d canvasFromElement = geometry.elementFromWorld() * canvasFromDoc;
    renderer.setTransform(canvasFromElement);
    svg::PathShape shape;
    shape.path = *spline;
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, paint.strokeParams);
  }
}

}  // namespace

void OverlayRenderer::drawChrome(svg::Renderer& renderer, const EditorApp& editor) {
  if (!editor.hasDocument()) {
    return;
  }

  // Map from document (viewBox) coordinates into canvas pixels. The
  // document has already been rendered with this transform baked into
  // every draw call; we have to apply the same one to the overlay or
  // the chrome drifts off the content as soon as the canvas and
  // viewBox have different aspect ratios.
  const Transform2d canvasFromDoc = editor.document().document().canvasFromDocumentTransform();
  drawChromeWithTransform(renderer, editor.selectedElements(), canvasFromDoc);
}

void OverlayRenderer::drawChrome(svg::Renderer& renderer,
                                 const std::optional<svg::SVGElement>& selection) {
  drawChromeWithTransform(renderer, selection, Transform2d());
}

void OverlayRenderer::drawChromeWithTransform(svg::Renderer& renderer,
                                              const std::optional<svg::SVGElement>& selection,
                                              const Transform2d& canvasFromDoc) {
  if (!selection.has_value()) {
    return;
  }
  std::array<svg::SVGElement, 1> single{*selection};
  drawChromeWithTransform(renderer, std::span<const svg::SVGElement>(single), canvasFromDoc);
}

void OverlayRenderer::drawChromeWithTransform(svg::Renderer& renderer,
                                              std::span<const svg::SVGElement> selection,
                                              const Transform2d& canvasFromDoc) {
  drawChromeWithTransform(renderer, selection, /*marqueeRectDoc=*/std::nullopt, canvasFromDoc);
}

void OverlayRenderer::drawChromeWithTransform(svg::Renderer& renderer,
                                              std::span<const svg::SVGElement> selection,
                                              const std::optional<Box2d>& marqueeRectDoc,
                                              const Transform2d& canvasFromDoc) {
  ZoneScopedN("OverlayRenderer::drawChrome");
  if (selection.empty() && !marqueeRectDoc.has_value()) {
    return;
  }

  // AABBs are computed inline from the selection's current DOM transforms
  // so they track the same frame as the per-element path outlines above.
  // Historically these came from a `SelectionBoundsCache` promoted by the
  // main loop — but that cache lagged the live DOM by 1–2 frames during a
  // drag, producing a visible shear between the path outline (live) and
  // the AABB (cached). Path outline + AABB are now sampled from the same
  // DOM snapshot. The cache is still useful for main-loop selection-
  // changed detection; it's just no longer gating the overlay's bounds.
  const std::vector<Box2d> selectionBoundsDoc = SnapshotSelectionWorldBounds(selection);

  // Compensate for the canvasFromDoc scale so strokes stay a fixed
  // canvas-pixel width regardless of zoom. Computed once and shared
  // across every chrome element.
  const double scale = LinearScale(canvasFromDoc);
  const auto pixelToWorld = [scale](double pixels) {
    return scale > 1e-9 ? pixels / scale : pixels;
  };
  const double selectionStrokeWidth = pixelToWorld(kSelectionStrokePixels);
  const double marqueeStrokeWidth = pixelToWorld(kMarqueeStrokePixels);
  const svg::PaintParams selectionStrokePaint = MakeSelectionStrokePaint(selectionStrokeWidth);

  // Per-element path outlines first — the user sees the exact shape of
  // every selected element regardless of how many are picked. All
  // outlines share the same cyan stroke, so we set the paint once and
  // reuse it across the loop.
  if (!selection.empty()) {
    renderer.setPaint(selectionStrokePaint);
    for (const auto& element : selection) {
      DrawElementPathOutline(renderer, element, canvasFromDoc, selectionStrokePaint);
    }
  }

  // Selection AABBs: one rectangle per element, plus a single combined
  // envelope when there are multiple elements (matches the legacy
  // `ComputeSelectionAabbScreenRects` output so multi-select chrome
  // still shows "per-element + combined"). Drawn in document space with
  // `canvasFromDoc` applied so they line up with the content bitmap
  // the compositor produced for the same frame.
  if (!selectionBoundsDoc.empty()) {
    renderer.setPaint(selectionStrokePaint);
    renderer.setTransform(canvasFromDoc);
    for (const Box2d& aabb : selectionBoundsDoc) {
      renderer.drawRect(aabb, selectionStrokePaint.strokeParams);
    }
    if (selectionBoundsDoc.size() > 1) {
      Box2d combined = selectionBoundsDoc.front();
      for (std::size_t i = 1; i < selectionBoundsDoc.size(); ++i) {
        combined.addBox(selectionBoundsDoc[i]);
      }
      renderer.drawRect(combined, selectionStrokePaint.strokeParams);
    }
  }

  // Marquee: translucent cyan fill + solid white outline. Two passes
  // (fill then stroke) because `drawRect` uses the current paint for
  // both, and we want different fill/stroke paints per the legacy
  // ImGui styling.
  if (marqueeRectDoc.has_value()) {
    renderer.setTransform(canvasFromDoc);
    const svg::PaintParams marqueeFill = MakeMarqueeFillPaint();
    renderer.setPaint(marqueeFill);
    renderer.drawRect(*marqueeRectDoc, marqueeFill.strokeParams);

    const svg::PaintParams marqueeStroke = MakeMarqueeStrokePaint(marqueeStrokeWidth);
    renderer.setPaint(marqueeStroke);
    renderer.drawRect(*marqueeRectDoc, marqueeStroke.strokeParams);
  }
}

}  // namespace donner::editor
