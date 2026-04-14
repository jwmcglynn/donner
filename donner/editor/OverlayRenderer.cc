#include "donner/editor/OverlayRenderer.h"

#include <array>

#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"

namespace donner::editor {

namespace {

/// Desired on-screen stroke thickness for selection chrome, in canvas
/// pixels. World-space stroke width is scaled down by `canvasFromDoc`'s
/// linear factor so the overlay stays 1 px regardless of zoom.
constexpr double kSelectionStrokePixels = 1.0;

svg::PaintParams MakeSelectionPaint(double worldStrokeWidth) {
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
    shape.entityFromParent = Transform2d();
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
  if (selection.empty()) {
    return;
  }

  // Compensate for the canvasFromDoc scale so the stroke is always a
  // fixed canvas-pixel width regardless of zoom. Computed once and
  // shared across all elements.
  const double scale = LinearScale(canvasFromDoc);
  const double worldStrokeWidth =
      scale > 1e-9 ? kSelectionStrokePixels / scale : kSelectionStrokePixels;
  const svg::PaintParams selectionPaint = MakeSelectionPaint(worldStrokeWidth);

  // Per-element path outlines first — the user sees the exact shape
  // of every selected element regardless of how many are picked.
  renderer.setPaint(selectionPaint);
  for (const auto& element : selection) {
    DrawElementPathOutline(renderer, element, canvasFromDoc, selectionPaint);
  }
}

}  // namespace donner::editor
