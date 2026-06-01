#include "donner/editor/OverlayRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/SelectionTransformHandles.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGGraphicsElement.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"

namespace donner::editor {

namespace {

/// Desired on-screen stroke thickness for selection chrome (path outlines
/// and AABBs), in canvas pixels. World-space stroke width is scaled down
/// by `canvasFromDoc`'s linear factor so the overlay stays 1 px
/// regardless of zoom.
constexpr double kSelectionStrokePixels = 1.0;

/// Desired on-screen stroke thickness for source-hover chrome.
constexpr double kHoverStrokePixels = 1.5;

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

svg::PaintParams MakeDisplayNoneSelectionStrokePaint(double worldStrokeWidth) {
  svg::PaintParams paint = MakeSelectionStrokePaint(worldStrokeWidth);
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0x5f, 0x9a, 0xb2, 0xff)));
  return paint;
}

svg::PaintParams MakeHandlePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
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

svg::PaintParams MakeSourceHoverShapePaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::Solid(css::Color(css::RGBA(0x00, 0xc8, 0xff, 0x30)));
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0xff, 0xff, 0xd0)));
  paint.fillOpacity = 1.0;
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Round;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Round;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

svg::PaintParams MakeSourceHoverBoundsPaint(double worldStrokeWidth) {
  svg::PaintParams paint;
  paint.fill = svg::PaintServer::None{};
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0x00, 0xc8, 0xff, 0xc8)));
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Round;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Round;
  paint.strokeParams.miterLimit = 4.0;
  return paint;
}

/// Desired on-screen stroke thickness for the locked-rejection flash outline.
/// Slightly heavier than the selection stroke so the rejection reads clearly.
constexpr double kLockedFlashStrokePixels = 2.0;

/// Red stroke, no fill, for the "this element is locked, you can't select it" flash. The stroke's
/// alpha is the flash `intensity` (1 → 0 as it fades) scaled into the 0–255 channel range.
svg::PaintParams MakeLockedFlashStrokePaint(double worldStrokeWidth, float intensity) {
  svg::PaintParams paint;
  const float clampedIntensity = std::clamp(intensity, 0.0f, 1.0f);
  const uint8_t alpha = static_cast<uint8_t>(std::lround(clampedIntensity * 255.0f));
  paint.stroke = svg::PaintServer::Solid(css::Color(css::RGBA(0xff, 0x1a, 0x1a, alpha)));
  paint.fill = svg::PaintServer::None{};
  paint.strokeOpacity = 1.0;
  paint.strokeParams.strokeWidth = worldStrokeWidth;
  paint.strokeParams.lineCap = svg::StrokeLinecap::Round;
  paint.strokeParams.lineJoin = svg::StrokeLinejoin::Round;
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

std::array<Vector2d, 4> TransformedBoxCorners(const Box2d& box,
                                              const Transform2d& documentFromBoxDocument) {
  const std::array<Vector2d, 4> corners{
      box.topLeft,
      Vector2d(box.bottomRight.x, box.topLeft.y),
      box.bottomRight,
      Vector2d(box.topLeft.x, box.bottomRight.y),
  };

  std::array<Vector2d, 4> transformed;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    transformed[i] = documentFromBoxDocument.transformPosition(corners[i]);
  }
  return transformed;
}

Box2d HandleBoxForCorner(const Vector2d& cornerDoc, double scale) {
  const SelectionTransformHandleBoxes handleBoxes =
      SelectionTransformHandleBoxesForBounds(Box2d(cornerDoc, cornerDoc), scale);
  return handleBoxes.boxes.front();
}

Path PathForCorners(const std::array<Vector2d, 4>& corners) {
  PathBuilder builder;
  builder.moveTo(corners[0]);
  builder.lineTo(corners[1]);
  builder.lineTo(corners[2]);
  builder.lineTo(corners[3]);
  builder.closePath();
  return builder.build();
}

Path TransformPath(const Path& path, const Transform2d& destFromSource) {
  PathBuilder builder;
  path.forEach([&](Path::Verb verb, std::span<const Vector2d> points) {
    switch (verb) {
      case Path::Verb::MoveTo: builder.moveTo(destFromSource.transformPosition(points[0])); break;
      case Path::Verb::LineTo: builder.lineTo(destFromSource.transformPosition(points[0])); break;
      case Path::Verb::QuadTo:
        builder.quadTo(destFromSource.transformPosition(points[0]),
                       destFromSource.transformPosition(points[1]));
        break;
      case Path::Verb::CurveTo:
        builder.curveTo(destFromSource.transformPosition(points[0]),
                        destFromSource.transformPosition(points[1]),
                        destFromSource.transformPosition(points[2]));
        break;
      case Path::Verb::ClosePath: builder.closePath(); break;
    }
  });
  return builder.build();
}

Path TransformPathToDocument(const Path& path, const Transform2d& documentFromElement) {
  return TransformPath(path, documentFromElement);
}

bool ElementDisplayNone(const svg::SVGElement& element) {
  return element.getComputedStyle().display.get().value() == svg::Display::None;
}

bool HasDisplayNoneInAncestorChain(const svg::SVGElement& element) {
  svg::SVGElement current = element;
  while (true) {
    if (ElementDisplayNone(current)) {
      return true;
    }

    std::optional<svg::SVGElement> parent = current.parentElement();
    if (!parent.has_value()) {
      return false;
    }
    current = *parent;
  }
}

bool BoxesIntersect(const Box2d& lhs, const Box2d& rhs) {
  return lhs.bottomRight.x >= rhs.topLeft.x && lhs.topLeft.x <= rhs.bottomRight.x &&
         lhs.bottomRight.y >= rhs.topLeft.y && lhs.topLeft.y <= rhs.bottomRight.y;
}

bool BoxIntersectsCullRect(const Box2d& box, const std::optional<Box2d>& cullRectDoc) {
  return !cullRectDoc.has_value() || BoxesIntersect(box, *cullRectDoc);
}

void AddBoxToOptional(std::optional<Box2d>* target, const Box2d& box) {
  if (target->has_value()) {
    (*target)->addBox(box);
  } else {
    *target = box;
  }
}

struct AppendChromeItemsOptions {
  bool includePaths = true;
  bool includePerElementAabbs = true;
  Transform2d representedDocumentFromLiveDocument = Transform2d();
};

std::optional<Box2d> AppendChromeItems(std::span<const svg::SVGElement> elements,
                                       const std::optional<Box2d>& cullRectDoc,
                                       std::vector<SelectionChromeSnapshot::PathItem>* outPaths,
                                       std::vector<Box2d>* outAabbs,
                                       AppendChromeItemsOptions options = {}) {
  std::optional<Box2d> combinedBounds;
  for (const auto& element : elements) {
    element.withWriteAccess([&element, &cullRectDoc, outPaths, outAabbs, options, &combinedBounds](
                                svg::DocumentWriteAccess&, EntityHandle) {
      std::optional<Box2d> mergedBounds;
      for (const auto& geometry : CollectRenderableGeometry(element)) {
        const std::optional<Box2d> worldBoundsDoc = geometry.worldBounds();
        const std::optional<Box2d> representedBoundsDoc =
            worldBoundsDoc.has_value()
                ? std::make_optional(
                      options.representedDocumentFromLiveDocument.transformBox(*worldBoundsDoc))
                : std::nullopt;
        if (representedBoundsDoc.has_value()) {
          AddBoxToOptional(&mergedBounds, *representedBoundsDoc);
        }

        if (representedBoundsDoc.has_value() &&
            !BoxIntersectsCullRect(*representedBoundsDoc, cullRectDoc)) {
          continue;
        }

        if (options.includePaths) {
          const auto spline = geometry.computedSpline();
          if (!spline.has_value()) {
            continue;
          }

          SelectionChromeSnapshot::PathItem item;
          const Transform2d documentFromElement = geometry.elementFromWorld();
          item.pathDoc = TransformPath(TransformPathToDocument(*spline, documentFromElement),
                                       options.representedDocumentFromLiveDocument);
          item.displayNone = HasDisplayNoneInAncestorChain(geometry);
          outPaths->push_back(std::move(item));
        }
      }

      if (mergedBounds.has_value() && BoxIntersectsCullRect(*mergedBounds, cullRectDoc)) {
        AddBoxToOptional(&combinedBounds, *mergedBounds);
        if (options.includePerElementAabbs) {
          outAabbs->push_back(*mergedBounds);
        }
      }
    });
  }
  return combinedBounds;
}

void CullBoxesInPlace(std::vector<Box2d>* boxes, const std::optional<Box2d>& cullRectDoc) {
  if (!cullRectDoc.has_value()) {
    return;
  }

  std::erase_if(*boxes, [&](const Box2d& box) { return !BoxesIntersect(box, *cullRectDoc); });
}

std::optional<SelectionChromeSnapshot::OrientedBox> CullOrientedBox(
    const SelectionChromeSnapshot::OrientedBox& orientedBox,
    const std::optional<Box2d>& cullRectDoc) {
  if (!cullRectDoc.has_value()) {
    return orientedBox;
  }

  Box2d bounds = Box2d::CreateEmpty(orientedBox.cornersDoc.front());
  for (const Vector2d& corner : orientedBox.cornersDoc) {
    bounds.addPoint(corner);
  }

  if (!BoxesIntersect(bounds, *cullRectDoc)) {
    return std::nullopt;
  }
  return orientedBox;
}

}  // namespace

void OverlayRenderer::drawChrome(svg::RendererInterface& renderer, const EditorApp& editor) {
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

void OverlayRenderer::drawChrome(svg::RendererInterface& renderer,
                                 const std::optional<svg::SVGElement>& selection) {
  drawChromeWithTransform(renderer, selection, Transform2d());
}

void OverlayRenderer::drawChromeWithTransform(svg::RendererInterface& renderer,
                                              const std::optional<svg::SVGElement>& selection,
                                              const Transform2d& canvasFromDoc) {
  if (!selection.has_value()) {
    return;
  }
  std::array<svg::SVGElement, 1> single{*selection};
  drawChromeWithTransform(renderer, std::span<const svg::SVGElement>(single), canvasFromDoc);
}

void OverlayRenderer::drawChromeWithTransform(svg::RendererInterface& renderer,
                                              std::span<const svg::SVGElement> selection,
                                              const Transform2d& canvasFromDoc) {
  drawChromeWithTransform(renderer, selection, /*marqueeRectDoc=*/std::nullopt, canvasFromDoc);
}

SelectionChromeSnapshot OverlayRenderer::captureChromeSnapshot(
    std::span<const svg::SVGElement> selection, const std::optional<Box2d>& marqueeRectDoc,
    const Transform2d& canvasFromDoc,
    const std::optional<SelectionChromeBoundsPreview>& activeBoundsPreview,
    std::span<const svg::SVGElement> sourceHover, const std::optional<Box2d>& cullRectDoc,
    SelectionChromeDetail selectionDetail, const Transform2d& representedDocumentFromLiveDocument,
    const std::optional<LockedRejectionFlashInput>& lockedFlash) {
  SelectionChromeSnapshot snapshot;
  snapshot.canvasFromDoc = canvasFromDoc;
  snapshot.marqueeDoc = marqueeRectDoc;

  // Locked-rejection flash: capture the rejected element's document-space outline (merged across
  // its renderable geometry leaves, same path-build path as selection chrome) so the draw phase can
  // stroke it red without touching the registry. Captured unconditionally of selection state — a
  // locked click never selects, so the flashed element is typically NOT in `selection`.
  if (lockedFlash.has_value() && lockedFlash->intensity > 0.0f) {
    std::vector<SelectionChromeSnapshot::PathItem> flashPaths;
    std::vector<Box2d> flashAabbs;
    std::array<svg::SVGElement, 1> flashElements{lockedFlash->element};
    AppendChromeItems(std::span<const svg::SVGElement>(flashElements), /*cullRectDoc=*/std::nullopt,
                      &flashPaths, &flashAabbs);
    if (!flashPaths.empty()) {
      PathBuilder mergedFlashPath;
      for (const SelectionChromeSnapshot::PathItem& item : flashPaths) {
        mergedFlashPath.addPath(item.pathDoc);
      }
      snapshot.lockedFlash = SelectionChromeSnapshot::LockedFlash{
          .pathDoc = mergedFlashPath.build(),
          .intensity = lockedFlash->intensity,
      };
    }
  }

  const double scale = LinearScale(canvasFromDoc);
  const auto pixelToWorld = [scale](double pixels) {
    return scale > 1e-9 ? pixels / scale : pixels;
  };
  snapshot.selectionStrokeWidthWorld = pixelToWorld(kSelectionStrokePixels);
  snapshot.hoverStrokeWidthWorld = pixelToWorld(kHoverStrokePixels);
  snapshot.marqueeStrokeWidthWorld = pixelToWorld(kMarqueeStrokePixels);

  if (!sourceHover.empty()) {
    AppendChromeItems(sourceHover, cullRectDoc, &snapshot.hoverPaths, &snapshot.hoverAabbsDoc);
  }

  if (selection.empty()) {
    return snapshot;
  }

  // Per-element path data + transforms. `computedSpline` and
  // `elementFromWorld` both read registry state — done here, before
  // returning, so the post-return snapshot is fully self-contained.
  const bool combinedBoundsOnly = selectionDetail == SelectionChromeDetail::CombinedBoundsOnly;
  const std::optional<Box2d> combinedSelectionBounds = AppendChromeItems(
      selection, cullRectDoc, &snapshot.paths, &snapshot.aabbsDoc,
      AppendChromeItemsOptions{
          .includePaths = !combinedBoundsOnly,
          .includePerElementAabbs = !combinedBoundsOnly,
          .representedDocumentFromLiveDocument = representedDocumentFromLiveDocument,
      });
  if (combinedBoundsOnly && combinedSelectionBounds.has_value()) {
    snapshot.aabbsDoc.push_back(*combinedSelectionBounds);
  }

  // AABBs are computed inline from the selection's current DOM transforms
  // so they track the same frame as the per-element path outlines above.
  // Historically these came from a `SelectionBoundsCache` promoted by the
  // main loop — but that cache lagged the live DOM by 1–2 frames during a
  // drag, producing a visible shear between the path outline (live) and
  // the AABB (cached). Path outline + AABB are now sampled from the same
  // DOM snapshot. The cache is still useful for main-loop selection-
  // changed detection; it's just no longer gating the overlay's bounds.
  if (activeBoundsPreview.has_value() && !snapshot.aabbsDoc.empty()) {
    const auto corners = TransformedBoxCorners(activeBoundsPreview->startBoundsDoc,
                                               activeBoundsPreview->documentFromStartDocument);
    std::array<Vector2d, 4> representedCorners;
    for (std::size_t i = 0; i < corners.size(); ++i) {
      representedCorners[i] = representedDocumentFromLiveDocument.transformPosition(corners[i]);
    }
    snapshot.orientedBoundsDoc = CullOrientedBox(
        SelectionChromeSnapshot::OrientedBox{.cornersDoc = representedCorners}, cullRectDoc);
    if (!snapshot.orientedBoundsDoc.has_value()) {
      snapshot.aabbsDoc.clear();
    }
    snapshot.handleBoxesDoc.reserve(representedCorners.size());
    for (const Vector2d& corner : representedCorners) {
      snapshot.handleBoxesDoc.push_back(HandleBoxForCorner(corner, scale));
    }
  } else if (!snapshot.aabbsDoc.empty()) {
    const Box2d combinedBounds = CombinedSelectionBounds(snapshot.aabbsDoc);
    const SelectionTransformHandleBoxes handleBoxes =
        SelectionTransformHandleBoxesForBounds(combinedBounds, scale);
    snapshot.handleBoxesDoc.assign(handleBoxes.boxes.begin(), handleBoxes.boxes.end());
  }
  CullBoxesInPlace(&snapshot.handleBoxesDoc, cullRectDoc);
  return snapshot;
}

void OverlayRenderer::drawChromeFromSnapshot(svg::RendererInterface& renderer,
                                             const SelectionChromeSnapshot& snapshot) {
  ZoneScopedN("OverlayRenderer::drawChromeFromSnapshot");
  if (snapshot.paths.empty() && snapshot.hoverPaths.empty() && snapshot.aabbsDoc.empty() &&
      snapshot.hoverAabbsDoc.empty() && !snapshot.orientedBoundsDoc.has_value() &&
      snapshot.handleBoxesDoc.empty() && !snapshot.marqueeDoc.has_value() &&
      !snapshot.lockedFlash.has_value()) {
    return;
  }

  if (!snapshot.hoverPaths.empty()) {
    const svg::PaintParams hoverShapePaint =
        MakeSourceHoverShapePaint(snapshot.hoverStrokeWidthWorld);
    renderer.setPaint(hoverShapePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const auto& item : snapshot.hoverPaths) {
      svg::PathShape shape;
      shape.path = item.pathDoc;
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, hoverShapePaint.strokeParams);
    }
  } else if (!snapshot.hoverAabbsDoc.empty()) {
    const svg::PaintParams hoverBoundsPaint =
        MakeSourceHoverBoundsPaint(snapshot.hoverStrokeWidthWorld);
    renderer.setPaint(hoverBoundsPaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const Box2d& aabb : snapshot.hoverAabbsDoc) {
      renderer.drawRect(aabb, hoverBoundsPaint.strokeParams);
    }
  }

  const svg::PaintParams selectionStrokePaint =
      MakeSelectionStrokePaint(snapshot.selectionStrokeWidthWorld);
  const svg::PaintParams displayNoneSelectionStrokePaint =
      MakeDisplayNoneSelectionStrokePaint(snapshot.selectionStrokeWidthWorld);

  // Per-element path outlines first — the user sees the exact shape of
  // every selected element regardless of how many are picked.
  if (!snapshot.paths.empty()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    for (const auto& item : snapshot.paths) {
      const svg::PaintParams& paint =
          item.displayNone ? displayNoneSelectionStrokePaint : selectionStrokePaint;
      renderer.setPaint(paint);
      svg::PathShape shape;
      shape.path = item.pathDoc;
      shape.parentFromEntity = Transform2d();
      renderer.drawPath(shape, paint.strokeParams);
    }
  }

  // Selection AABBs: one rectangle per element, plus a single combined
  // envelope when there are multiple elements. Drawn in document space
  // with `canvasFromDoc` applied so they line up with the content
  // bitmap the compositor produced for the same frame.
  if (snapshot.orientedBoundsDoc.has_value()) {
    renderer.setPaint(selectionStrokePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    svg::PathShape shape;
    shape.path = PathForCorners(snapshot.orientedBoundsDoc->cornersDoc);
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, selectionStrokePaint.strokeParams);
  } else if (!snapshot.aabbsDoc.empty()) {
    renderer.setPaint(selectionStrokePaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    const Box2d combinedBounds = CombinedSelectionBounds(snapshot.aabbsDoc);
    for (const Box2d& aabb : snapshot.aabbsDoc) {
      renderer.drawRect(aabb, selectionStrokePaint.strokeParams);
    }
    if (snapshot.aabbsDoc.size() > 1) {
      renderer.drawRect(combinedBounds, selectionStrokePaint.strokeParams);
    }
  }

  if (!snapshot.handleBoxesDoc.empty()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    const svg::PaintParams handlePaint = MakeHandlePaint(snapshot.selectionStrokeWidthWorld);
    renderer.setPaint(handlePaint);
    for (const Box2d& handleBox : snapshot.handleBoxesDoc) {
      renderer.drawRect(handleBox, handlePaint.strokeParams);
    }
  }

  // Marquee: translucent cyan fill + solid white outline. Two passes
  // (fill then stroke) — different fill/stroke paints per the legacy
  // ImGui styling.
  if (snapshot.marqueeDoc.has_value()) {
    renderer.setTransform(snapshot.canvasFromDoc);
    const svg::PaintParams marqueeFill = MakeMarqueeFillPaint();
    renderer.setPaint(marqueeFill);
    renderer.drawRect(*snapshot.marqueeDoc, marqueeFill.strokeParams);

    const svg::PaintParams marqueeStroke = MakeMarqueeStrokePaint(snapshot.marqueeStrokeWidthWorld);
    renderer.setPaint(marqueeStroke);
    renderer.drawRect(*snapshot.marqueeDoc, marqueeStroke.strokeParams);
  }

  // Locked-rejection flash: a red outline of the rejected (locked) element's path, drawn last so it
  // reads on top of all other chrome. The stroke alpha fades with `intensity`. Stroke width tracks
  // the snapshot's pixel-to-world scale just like the selection outline.
  if (snapshot.lockedFlash.has_value() && snapshot.lockedFlash->intensity > 0.0f) {
    const double lockedFlashStrokeWidthWorld =
        snapshot.selectionStrokeWidthWorld > 0.0
            ? snapshot.selectionStrokeWidthWorld *
                  (kLockedFlashStrokePixels / kSelectionStrokePixels)
            : kLockedFlashStrokePixels;
    const svg::PaintParams lockedFlashPaint =
        MakeLockedFlashStrokePaint(lockedFlashStrokeWidthWorld, snapshot.lockedFlash->intensity);
    renderer.setPaint(lockedFlashPaint);
    renderer.setTransform(snapshot.canvasFromDoc);
    svg::PathShape shape;
    shape.path = snapshot.lockedFlash->pathDoc;
    shape.parentFromEntity = Transform2d();
    renderer.drawPath(shape, lockedFlashPaint.strokeParams);
  }
}

void OverlayRenderer::drawChromeWithTransform(
    svg::RendererInterface& renderer, std::span<const svg::SVGElement> selection,
    const std::optional<Box2d>& marqueeRectDoc, const Transform2d& canvasFromDoc,
    const std::optional<SelectionChromeBoundsPreview>& activeBoundsPreview,
    std::span<const svg::SVGElement> sourceHover, const std::optional<Box2d>& cullRectDoc,
    SelectionChromeDetail selectionDetail, const Transform2d& representedDocumentFromLiveDocument) {
  ZoneScopedN("OverlayRenderer::drawChrome");
  // Route the live path through capture + draw so M7's snapshot
  // implementation is the single source of truth. Same output, same
  // performance characteristics (the capture is straight-line registry
  // reads + a small allocation).
  const SelectionChromeSnapshot snapshot = captureChromeSnapshot(
      selection, marqueeRectDoc, canvasFromDoc, activeBoundsPreview, sourceHover, cullRectDoc,
      selectionDetail, representedDocumentFromLiveDocument);
  drawChromeFromSnapshot(renderer, snapshot);
}

}  // namespace donner::editor
