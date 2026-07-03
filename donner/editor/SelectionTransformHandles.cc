#include "donner/editor/SelectionTransformHandles.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace donner::editor {

namespace {

constexpr double kHandleSizePixels = 9.0;
constexpr double kRotateOuterRadiusPixels = 24.0;
constexpr double kRotateInnerRadiusPixels = 10.0;
constexpr double kMinPixelsPerDocUnit = 1e-9;

std::array<SelectionTransformCorner, 4> AllCorners() {
  return {SelectionTransformCorner::TopLeft, SelectionTransformCorner::TopRight,
          SelectionTransformCorner::BottomRight, SelectionTransformCorner::BottomLeft};
}

bool IsFinite(double value) {
  return std::isfinite(value);
}

bool IsFinite(const Vector2d& value) {
  return IsFinite(value.x) && IsFinite(value.y);
}

bool ResizeBoxContains(const Vector2d& cornerDoc, const Vector2d& pointDoc, double halfHandleDoc) {
  return std::abs(pointDoc.x - cornerDoc.x) <= halfHandleDoc &&
         std::abs(pointDoc.y - cornerDoc.y) <= halfHandleDoc;
}

}  // namespace

Box2d CombinedSelectionBounds(std::span<const Box2d> selectionBoundsDoc) {
  if (selectionBoundsDoc.empty()) {
    return Box2d();
  }

  Box2d combined = selectionBoundsDoc.front();
  for (std::size_t i = 1; i < selectionBoundsDoc.size(); ++i) {
    combined.addBox(selectionBoundsDoc[i]);
  }
  return combined;
}

Vector2d SelectionTransformCornerPoint(const Box2d& bounds, SelectionTransformCorner corner) {
  switch (corner) {
    case SelectionTransformCorner::TopLeft: return bounds.topLeft;
    case SelectionTransformCorner::TopRight:
      return Vector2d(bounds.bottomRight.x, bounds.topLeft.y);
    case SelectionTransformCorner::BottomRight: return bounds.bottomRight;
    case SelectionTransformCorner::BottomLeft:
      return Vector2d(bounds.topLeft.x, bounds.bottomRight.y);
  }
  return bounds.topLeft;
}

SelectionTransformCorner OppositeSelectionTransformCorner(SelectionTransformCorner corner) {
  switch (corner) {
    case SelectionTransformCorner::TopLeft: return SelectionTransformCorner::BottomRight;
    case SelectionTransformCorner::TopRight: return SelectionTransformCorner::BottomLeft;
    case SelectionTransformCorner::BottomRight: return SelectionTransformCorner::TopLeft;
    case SelectionTransformCorner::BottomLeft: return SelectionTransformCorner::TopRight;
  }
  return SelectionTransformCorner::TopLeft;
}

SelectionTransformHandleBoxes SelectionTransformHandleBoxesForBounds(const Box2d& boundsDoc,
                                                                     double pixelsPerDocUnit) {
  const double scale = std::max(std::abs(pixelsPerDocUnit), kMinPixelsPerDocUnit);
  const double halfHandleDoc = (kHandleSizePixels * 0.5) / scale;

  SelectionTransformHandleBoxes result;
  const std::array<SelectionTransformCorner, 4> corners = AllCorners();
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const Vector2d corner = SelectionTransformCornerPoint(boundsDoc, corners[i]);
    result.boxes[i] = Box2d(corner - Vector2d(halfHandleDoc, halfHandleDoc),
                            corner + Vector2d(halfHandleDoc, halfHandleDoc));
  }
  return result;
}

SelectionTransformHandleIntent HitTestSelectionTransformHandles(
    std::span<const Box2d> selectionBoundsDoc, const Vector2d& documentPoint,
    double pixelsPerDocUnit, bool includeRotate) {
  if (selectionBoundsDoc.empty() || !IsFinite(documentPoint)) {
    return {};
  }

  const double scale = std::abs(pixelsPerDocUnit);
  if (scale < kMinPixelsPerDocUnit || !IsFinite(scale)) {
    return {};
  }

  const Box2d bounds = CombinedSelectionBounds(selectionBoundsDoc);
  if (bounds.isEmpty()) {
    return {};
  }

  const double halfHandleDoc = (kHandleSizePixels * 0.5) / scale;
  const std::array<SelectionTransformCorner, 4> corners = AllCorners();
  for (SelectionTransformCorner corner : corners) {
    const Vector2d cornerDoc = SelectionTransformCornerPoint(bounds, corner);
    if (ResizeBoxContains(cornerDoc, documentPoint, halfHandleDoc)) {
      return SelectionTransformHandleIntent{
          .kind = SelectionTransformHandleKind::Resize,
          .corner = corner,
      };
    }
  }

  if (!includeRotate) {
    return {};
  }

  const double innerRadiusDoc = kRotateInnerRadiusPixels / scale;
  const double outerRadiusDoc = kRotateOuterRadiusPixels / scale;
  if (bounds.contains(documentPoint)) {
    return {};
  }

  for (SelectionTransformCorner corner : corners) {
    const Vector2d cornerDoc = SelectionTransformCornerPoint(bounds, corner);
    const double distance = (documentPoint - cornerDoc).length();
    if (distance > innerRadiusDoc && distance <= outerRadiusDoc) {
      return SelectionTransformHandleIntent{
          .kind = SelectionTransformHandleKind::Rotate,
          .corner = corner,
      };
    }
  }

  return {};
}

double PixelsPerDocUnitFromTransform(const Transform2d& canvasFromDoc) {
  return canvasFromDoc.transformVector(Vector2d(1.0, 0.0)).length();
}

}  // namespace donner::editor
