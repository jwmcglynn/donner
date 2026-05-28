#include "donner/editor/ViewportState.h"

#include <algorithm>
#include <cmath>

namespace donner::editor {

namespace {

/// Tolerance below which `pixelsPerDocUnit()` is treated as zero
/// (degenerate viewport — pane collapsed, zoom clamped, etc.). All
/// transform helpers fall back to a no-op identity in this case so
/// callers don't have to special-case it.
constexpr double kEpsilonScale = 1e-12;

int ClampRasterDim(double v) {
  if (!(v > 0.0)) {
    return 1;  // Includes NaN / negative / zero.
  }
  const double rounded = std::round(v);
  if (rounded > static_cast<double>(ViewportState::kMaxCanvasDim)) {
    return ViewportState::kMaxCanvasDim;
  }
  if (rounded < 1.0) {
    return 1;
  }
  return static_cast<int>(rounded);
}

Transform2d OutputFromDocumentTransform(const Vector2d& documentTopLeft, double scale) {
  return Transform2d::Translate(-documentTopLeft) * Transform2d::Scale(scale);
}

}  // namespace

Vector2d ViewportState::documentToScreen(const Vector2d& docPoint) const {
  const double s = pixelsPerDocUnit();
  return panScreenPoint + (docPoint - panDocPoint) * s;
}

Vector2d ViewportState::screenToDocument(const Vector2d& screenPoint) const {
  const double s = pixelsPerDocUnit();
  if (std::abs(s) < kEpsilonScale) {
    return panDocPoint;
  }
  return panDocPoint + (screenPoint - panScreenPoint) / s;
}

Box2d ViewportState::documentToScreen(const Box2d& docBox) const {
  // Affine: AABB of the transformed corners. The four-corner sweep
  // is overkill for an axis-aligned scale+translate, but it's the
  // safe form if the transform ever grows a rotation.
  Box2d result(documentToScreen(docBox.topLeft), documentToScreen(docBox.topLeft));
  result.addPoint(documentToScreen(Vector2d(docBox.bottomRight.x, docBox.topLeft.y)));
  result.addPoint(documentToScreen(Vector2d(docBox.topLeft.x, docBox.bottomRight.y)));
  result.addPoint(documentToScreen(docBox.bottomRight));
  return result;
}

Box2d ViewportState::screenToDocument(const Box2d& screenBox) const {
  Box2d result(screenToDocument(screenBox.topLeft), screenToDocument(screenBox.topLeft));
  result.addPoint(screenToDocument(Vector2d(screenBox.bottomRight.x, screenBox.topLeft.y)));
  result.addPoint(screenToDocument(Vector2d(screenBox.topLeft.x, screenBox.bottomRight.y)));
  result.addPoint(screenToDocument(screenBox.bottomRight));
  return result;
}

EditorRasterViewport ViewportState::rasterViewport() const {
  // Document units → device pixels at the current zoom and DPR.
  const Vector2d docSize = documentViewBox.size();
  const double scale = devicePixelsPerDocUnit();

  const Vector2d fullTarget(docSize.x * scale, docSize.y * scale);
  EditorRasterViewport result;
  result.documentRect = documentViewBox;
  result.semanticCanvasSizePx =
      Vector2i(ClampRasterDim(fullTarget.x), ClampRasterDim(fullTarget.y));
  result.outputSizePx = result.semanticCanvasSizePx;
  result.outputFromDocument = OutputFromDocumentTransform(documentViewBox.topLeft, scale);

  if (!(fullTarget.x > 0.0) || !(fullTarget.y > 0.0)) {
    return result;
  }

  Vector2d maxTarget(static_cast<double>(kMaxCanvasDim), static_cast<double>(kMaxCanvasDim));
  if (paneSize.x > 0.0 && paneSize.y > 0.0 && devicePixelRatio > 0.0) {
    const double marginDevicePx =
        static_cast<double>(kHighZoomRasterMarginScreenPx) * devicePixelRatio;
    maxTarget.x = std::min(maxTarget.x, paneSize.x * devicePixelRatio + 2.0 * marginDevicePx);
    maxTarget.y = std::min(maxTarget.y, paneSize.y * devicePixelRatio + 2.0 * marginDevicePx);
  }

  const bool shouldViewportBound = paneSize.x > 0.0 && paneSize.y > 0.0 && devicePixelRatio > 0.0 &&
                                   (fullTarget.x > maxTarget.x || fullTarget.y > maxTarget.y);
  if (!shouldViewportBound) {
    return result;
  }

  const double marginScreenPx = static_cast<double>(kHighZoomRasterMarginScreenPx);
  const Vector2d outputScreenTopLeft = paneOrigin - Vector2d(marginScreenPx, marginScreenPx);
  const Vector2i outputSizePx(
      ClampRasterDim(paneSize.x * devicePixelRatio + 2.0 * marginScreenPx * devicePixelRatio),
      ClampRasterDim(paneSize.y * devicePixelRatio + 2.0 * marginScreenPx * devicePixelRatio));
  const Vector2d documentTopLeft = screenToDocument(outputScreenTopLeft);
  const Vector2d documentSize(static_cast<double>(outputSizePx.x) / scale,
                              static_cast<double>(outputSizePx.y) / scale);

  result.documentRect = Box2d(documentTopLeft, documentTopLeft + documentSize);
  result.outputSizePx = outputSizePx;
  result.outputFromDocument = OutputFromDocumentTransform(documentTopLeft, scale);
  result.viewportBounded = true;
  return result;
}

Vector2i ViewportState::desiredCanvasSize() const {
  return rasterViewport().outputSizePx;
}

void ViewportState::zoomAround(double newZoom, const Vector2d& focalScreen) {
  // Snapshot the document point currently under `focalScreen` *before*
  // updating zoom — `screenToDocument` reads `pixelsPerDocUnit()`.
  const Vector2d focalDoc = screenToDocument(focalScreen);
  zoom = std::clamp(newZoom, kMinZoom, kMaxZoom);
  // Re-anchor: the same document point is still glued to the same
  // screen point. By construction `documentToScreen(focalDoc)` now
  // returns `focalScreen` exactly.
  panDocPoint = focalDoc;
  panScreenPoint = focalScreen;
}

void ViewportState::panBy(const Vector2d& screenDelta) {
  panScreenPoint += screenDelta;
}

void ViewportState::resetTo100Percent() {
  zoom = 1.0;
  panDocPoint = documentViewBoxCenter();
  panScreenPoint = paneCenter();
}

}  // namespace donner::editor
