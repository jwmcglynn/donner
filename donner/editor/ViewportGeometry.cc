#include "donner/editor/ViewportGeometry.h"

#include <cmath>

namespace donner::editor {

namespace {

constexpr double kEpsilon = 1e-9;

}  // namespace

bool DrawingViewportLayout::hasImage() const {
  return imageSize.x > kEpsilon && imageSize.y > kEpsilon;
}

bool DrawingViewportLayout::containsScreenPoint(const Vector2d& screenPoint) const {
  return hasImage() && screenPoint.x >= imageOrigin.x &&
         screenPoint.x <= imageOrigin.x + imageSize.x && screenPoint.y >= imageOrigin.y &&
         screenPoint.y <= imageOrigin.y + imageSize.y;
}

std::optional<Vector2d> DrawingViewportLayout::screenToDocument(
    const Vector2d& screenPoint) const {
  if (!hasImage()) {
    return std::nullopt;
  }

  const double normalizedX = (screenPoint.x - imageOrigin.x) / imageSize.x;
  const double normalizedY = (screenPoint.y - imageOrigin.y) / imageSize.y;

  return Vector2d(documentViewBox.topLeft.x + normalizedX * documentViewBox.width(),
                  documentViewBox.topLeft.y + normalizedY * documentViewBox.height());
}

std::optional<Vector2d> DrawingViewportLayout::documentToScreen(
    const Vector2d& documentPoint) const {
  if (!hasImage() || std::abs(documentViewBox.width()) <= kEpsilon ||
      std::abs(documentViewBox.height()) <= kEpsilon) {
    return std::nullopt;
  }

  const double normalizedX =
      (documentPoint.x - documentViewBox.topLeft.x) / documentViewBox.width();
  const double normalizedY =
      (documentPoint.y - documentViewBox.topLeft.y) / documentViewBox.height();

  return Vector2d(imageOrigin.x + normalizedX * imageSize.x,
                  imageOrigin.y + normalizedY * imageSize.y);
}

DrawingViewportLayout ComputeDrawingViewportLayout(const Vector2d& contentOrigin,
                                                   const Vector2d& availableRegionSize,
                                                   const Vector2d& imageSize,
                                                   const Vector2d& panOffset,
                                                   const Box2d& documentViewBox) {
  DrawingViewportLayout result;
  result.imageSize = imageSize;
  result.documentViewBox = documentViewBox;
  result.imageOrigin = Vector2d(
      contentOrigin.x + (availableRegionSize.x - imageSize.x) * 0.5 + panOffset.x,
      contentOrigin.y + (availableRegionSize.y - imageSize.y) * 0.5 + panOffset.y);
  return result;
}

}  // namespace donner::editor
