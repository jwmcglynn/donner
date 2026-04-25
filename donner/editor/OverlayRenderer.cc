#include "donner/editor/OverlayRenderer.h"

#include <array>
#include <utility>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/editor/backend_lib/EditorApp.h"
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

/// On-screen resize handle size in canvas pixels.
constexpr double kSelectionHandleSizePixels = 6.0;

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

svg::PaintParams MakeSelectionHandlePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  // White fill with the same cyan outline as the rest of selection chrome.
  paint.fill = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0xff, 0xff, 0xff)));
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0x00, 0xc8, 0xff, 0xff)));
  paint.fillOpacity = 1.0;
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

Path TransformPath(const Path& path, const Transform2d& transform) {
  PathBuilder builder;
  const auto& points = path.points();

  for (const auto& command : path.commands()) {
    switch (command.verb) {
      case Path::Verb::MoveTo:
        builder.moveTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::LineTo:
        builder.lineTo(transform.transformPosition(points[command.pointIndex]));
        break;
      case Path::Verb::QuadTo:
        builder.quadTo(transform.transformPosition(points[command.pointIndex]),
                       transform.transformPosition(points[command.pointIndex + 1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(transform.transformPosition(points[command.pointIndex]),
                        transform.transformPosition(points[command.pointIndex + 1]),
                        transform.transformPosition(points[command.pointIndex + 2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  }

  return builder.build();
}

/// Inner helper that draws *just* the path outline for a single
/// geometry element. The AABB is drawn separately by the caller —
/// per the design doc, multi-select chrome shows one combined AABB
/// across the whole selection, not one box per element.
void DrawElementPathOutline(svg::Renderer& renderer, const svg::SVGGeometryElement& geometry,
                            const Transform2d& canvasFromDoc, const svg::PaintParams& paint) {
  // The spline is in the element's local coordinate space. Transform
  // it into document space first, then draw it under only
  // `canvasFromDoc`. Keeping the element's own transform out of the
  // renderer state is important for editor chrome: the overlay stroke
  // must stay one screen pixel wide during object resize, and curve
  // flattening should run against the resized document-space curve
  // instead of flattening a tiny local curve and magnifying the
  // segments.
  //
  // An earlier version of this code had an extra `.inverse()` which
  // silently flipped the sign of every element transform. That
  // regression is still covered by overlay tests; this path-space
  // transform preserves the same local→world behavior without
  // inheriting object scale into stroke width.
  if (const auto spline = geometry.computedSpline(); spline.has_value()) {
    Path documentPath = TransformPath(*spline, geometry.elementFromWorld());
    renderer.setTransform(canvasFromDoc);
    svg::PathShape shape;
    shape.path = std::move(documentPath);
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, paint.strokeParams);
  }
}

void DrawSelectionHandles(svg::Renderer& renderer, const Box2d& aabb,
                          const svg::PaintParams& handlePaint, double handleSizeWorld) {
  if (aabb.topLeft.x >= aabb.bottomRight.x || aabb.topLeft.y >= aabb.bottomRight.y) {
    return;
  }

  const double cx = (aabb.topLeft.x + aabb.bottomRight.x) * 0.5;
  const double cy = (aabb.topLeft.y + aabb.bottomRight.y) * 0.5;
  const std::array<Vector2d, 8> handles = {
      Vector2d(aabb.topLeft.x, aabb.topLeft.y),         Vector2d(cx, aabb.topLeft.y),
      Vector2d(aabb.bottomRight.x, aabb.topLeft.y),     Vector2d(aabb.bottomRight.x, cy),
      Vector2d(aabb.bottomRight.x, aabb.bottomRight.y), Vector2d(cx, aabb.bottomRight.y),
      Vector2d(aabb.topLeft.x, aabb.bottomRight.y),     Vector2d(aabb.topLeft.x, cy),
  };
  const double halfHandle = handleSizeWorld * 0.5;
  for (const Vector2d& center : handles) {
    renderer.drawRect(Box2d(Vector2d(center.x - halfHandle, center.y - halfHandle),
                            Vector2d(center.x + halfHandle, center.y + halfHandle)),
                      handlePaint.strokeParams);
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
  const double selectionHandleSize = pixelToWorld(kSelectionHandleSizePixels);
  const svg::PaintParams selectionStrokePaint = MakeSelectionStrokePaint(selectionStrokeWidth);
  const svg::PaintParams selectionHandlePaint = MakeSelectionHandlePaint(selectionStrokeWidth);

  // Per-element path outlines first — the user sees the exact shape of
  // every selected element regardless of how many are picked. Group-like
  // selections (e.g. `<g filter>`) are expanded into their renderable
  // geometry descendants so the user still sees the leaf shapes that
  // make up the group, not just its AABB. All outlines share the same
  // cyan stroke, so we set the paint once and reuse it across the loop.
  if (!selection.empty()) {
    renderer.setPaint(selectionStrokePaint);
    for (const auto& element : selection) {
      for (const auto& geometry : CollectRenderableGeometry(element)) {
        DrawElementPathOutline(renderer, geometry, canvasFromDoc, selectionStrokePaint);
      }
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

    renderer.setPaint(selectionHandlePaint);
    for (const Box2d& aabb : selectionBoundsDoc) {
      DrawSelectionHandles(renderer, aabb, selectionHandlePaint, selectionHandleSize);
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
