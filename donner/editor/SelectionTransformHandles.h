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

/// Corner handle square size, in logical UI pixels. Shared by every surface
/// that draws selection transform handles - the select tool's axis-aligned
/// frame, its in-gesture oriented preview, and the text frame (both the
/// editing session's frame and the select tool's oriented `<text>` frame) -
/// so all of them read at the same size. QA-F4: the text frame previously
/// derived its handle size from the device-pixel-scaled stroke width, which
/// rendered visibly larger than the select tool's handles on HiDPI displays.
inline constexpr double kSelectionHandleSizePixels = 9.0;

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

/// Document-space corners (local TL, TR, BR, BL order) of the axis-aligned
/// @p frameLocal mapped through @p documentFromText. An ORIENTED quad: for a
/// rotated element the corners carry the rotation instead of collapsing to
/// the axis-aligned envelope. Shared by the text tool's editing-session frame
/// (W1) and the select tool's oriented `<text>` frame (W12).
[[nodiscard]] std::array<Vector2d, 4> FrameCornersDoc(const Transform2d& documentFromText,
                                                      const Box2d& frameLocal);

/// Hit-test resize/rotate handles for an ORIENTED frame: @p frameLocal is the
/// axis-aligned frame in the element's local space, mapped to document space
/// through @p documentFromText. The pointer is mapped into local space and
/// hit-tested there (where the frame is axis-aligned), so on a rotated frame
/// the handle zones and the rotate ring track the ORIENTED corners rather
/// than the axis-aligned envelope. The returned corner identity is in the
/// frame's local space - the same space a resize gesture operates in.
///
/// Shared by the text tool's editing session (W1) and the select tool's
/// oriented `<text>` selection (W12).
///
/// @param frameLocal Axis-aligned frame in the element's local space.
/// @param documentFromText Local-to-document transform for the element.
/// @param documentPoint Point to hit-test, in document coordinates.
/// @param pixelsPerDocUnit Current viewport scale (pixels per document unit).
/// @param includeRotate Whether rotate ring zones should be considered.
[[nodiscard]] SelectionTransformHandleIntent HitTestOrientedFrameHandles(
    const Box2d& frameLocal, const Transform2d& documentFromText, const Vector2d& documentPoint,
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
