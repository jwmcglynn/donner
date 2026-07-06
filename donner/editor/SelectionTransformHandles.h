#pragma once
/// @file

#include <array>
#include <span>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::editor {

/// Corner handle identity for selection resize/rotate gestures.
enum class SelectionTransformCorner {
  TopLeft,
  TopRight,
  BottomRight,
  BottomLeft,
};

/// Interaction kind under the pointer near selection transform handles.
enum class SelectionTransformHandleKind {
  None,
  Resize,
  Rotate,
};

/// Hit-test result for selection transform handles.
struct SelectionTransformHandleIntent {
  SelectionTransformHandleKind kind = SelectionTransformHandleKind::None;
  SelectionTransformCorner corner = SelectionTransformCorner::TopLeft;

  friend bool operator==(const SelectionTransformHandleIntent&,
                         const SelectionTransformHandleIntent&) = default;
};

/// Visual handle boxes for one selection envelope, in document coordinates.
struct SelectionTransformHandleBoxes {
  std::array<Box2d, 4> boxes;
};

/// Return a combined AABB for a selection-bounds span.
[[nodiscard]] Box2d CombinedSelectionBounds(std::span<const Box2d> selectionBoundsDoc);

/// Return the point for a corner of @p bounds.
[[nodiscard]] Vector2d SelectionTransformCornerPoint(const Box2d& bounds,
                                                     SelectionTransformCorner corner);

/// Return the opposite corner from @p corner.
[[nodiscard]] SelectionTransformCorner OppositeSelectionTransformCorner(
    SelectionTransformCorner corner);

/// Build document-space handle boxes with screen-stable size under @p pixelsPerDocUnit.
[[nodiscard]] SelectionTransformHandleBoxes SelectionTransformHandleBoxesForBounds(
    const Box2d& boundsDoc, double pixelsPerDocUnit);

/// Hit-test resize and rotate corner zones using document-space bounds and scale.
///
/// Resize wins over rotate when zones overlap. Rotate is intentionally outside
/// the square handle, in a small ring around each corner.
///
/// @param selectionBoundsDoc Selection bounds in document coordinates.
/// @param documentPoint Point to hit test in document coordinates.
/// @param pixelsPerDocUnit Current viewport scale.
/// @param includeRotate Whether rotate ring zones should be considered.
[[nodiscard]] SelectionTransformHandleIntent HitTestSelectionTransformHandles(
    std::span<const Box2d> selectionBoundsDoc, const Vector2d& documentPoint,
    double pixelsPerDocUnit, bool includeRotate = true);

/// Return pixels per document unit from a document-to-canvas transform.
[[nodiscard]] double PixelsPerDocUnitFromTransform(const Transform2d& canvasFromDoc);

/// Compose @p centeredDocumentFromDocument so it acts around
/// @p fixedDocumentPoint instead of the document origin: translate the fixed
/// point to the origin, apply the transform, translate back. Shared by the
/// select tool's resize/rotate gestures and the text tool's frame rotate.
[[nodiscard]] Transform2d TransformDocumentAroundPoint(
    const Vector2d& fixedDocumentPoint, const Transform2d& centeredDocumentFromDocument);

/// Angle of @p point around @p center in radians, measured from +x.
[[nodiscard]] double AngleFromCenter(const Vector2d& center, const Vector2d& point);

}  // namespace donner::editor
