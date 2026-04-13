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

Vector2i ViewportState::desiredCanvasSize() const {
  // Document units → device pixels at the current zoom and DPR.
  const Vector2d docSize = documentViewBox.size();
  const double scale = devicePixelsPerDocUnit();
  const auto clampDim = [](double v) {
    if (!(v > 0.0)) {
      return 1;  // Includes NaN / negative / zero.
    }
    const double rounded = std::round(v);
    if (rounded > static_cast<double>(kMaxCanvasDim)) {
      return kMaxCanvasDim;
    }
    if (rounded < 1.0) {
      return 1;
    }
    return static_cast<int>(rounded);
  };
  return Vector2i(clampDim(docSize.x * scale), clampDim(docSize.y * scale));
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
