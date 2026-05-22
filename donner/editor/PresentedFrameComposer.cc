#include "donner/editor/PresentedFrameComposer.h"

#include <cmath>

#include "donner/base/Box.h"

namespace donner::editor {
namespace {

bool IsFinite(double value) {
  return std::isfinite(value);
}

bool IsFinite(const Vector2d& value) {
  return IsFinite(value.x) && IsFinite(value.y);
}

bool IsFinite(const Transform2d& transform) {
  for (double value : transform.data) {
    if (!IsFinite(value)) {
      return false;
    }
  }
  return true;
}

bool IsValidRect(const Vector2d& topLeft, const Vector2d& bottomRight) {
  return IsFinite(topLeft) && IsFinite(bottomRight) && bottomRight.x > topLeft.x &&
         bottomRight.y > topLeft.y;
}

}  // namespace

Vector2d ResolvePresentedTileDragTranslation(
    const PresentedFrameTileGeometry& tile,
    const std::optional<PresentedDragBaseline>& dragBaseline) {
  if (tile.isDragTarget && dragBaseline.has_value()) {
    const Vector2d unpresentedDragDeltaDoc =
        dragBaseline->activeTranslationDoc - dragBaseline->representedTranslationDoc;
    return tile.dragTranslationDoc + unpresentedDragDeltaDoc;
  }

  return tile.dragTranslationDoc;
}

Vector2d ResolvePresentedOverlayDragTranslation(
    const std::optional<PresentedDragBaseline>& dragBaseline) {
  if (!dragBaseline.has_value()) {
    return Vector2d::Zero();
  }

  return dragBaseline->activeTranslationDoc - dragBaseline->representedTranslationDoc;
}

std::optional<PresentedTileRect> ComputePresentedTileRect(
    const PresentedFrameTileGeometry& tile, const Transform2d& outputFromCanvasTransform,
    const std::optional<PresentedDragBaseline>& dragBaseline) {
  if (!IsFinite(outputFromCanvasTransform) || !IsFinite(tile.canvasOffsetDoc) ||
      !IsFinite(tile.bitmapDimsDoc) || !IsFinite(tile.dragTranslationDoc) ||
      tile.bitmapDimsDoc.x <= 0.0 || tile.bitmapDimsDoc.y <= 0.0) {
    return std::nullopt;
  }

  const Vector2d effectiveDragTranslationDoc =
      ResolvePresentedTileDragTranslation(tile, dragBaseline);
  if (!IsFinite(effectiveDragTranslationDoc)) {
    return std::nullopt;
  }

  const Vector2d originCanvas = tile.canvasOffsetDoc + effectiveDragTranslationDoc;
  const Box2d canvasBox(originCanvas, originCanvas + tile.bitmapDimsDoc);
  const Box2d outputBox = outputFromCanvasTransform.transformBox(canvasBox);
  if (!IsValidRect(outputBox.topLeft, outputBox.bottomRight)) {
    return std::nullopt;
  }

  return PresentedTileRect{
      .topLeft = outputBox.topLeft,
      .bottomRight = outputBox.bottomRight,
      .effectiveDragTranslationDoc = effectiveDragTranslationDoc,
  };
}

std::optional<PresentedPixelRect> RoundPresentedTileRectToPixelRect(const PresentedTileRect& rect) {
  if (!IsValidRect(rect.topLeft, rect.bottomRight)) {
    return std::nullopt;
  }

  const int x = static_cast<int>(std::lround(rect.topLeft.x));
  const int y = static_cast<int>(std::lround(rect.topLeft.y));
  const int width = static_cast<int>(std::lround(rect.bottomRight.x - rect.topLeft.x));
  const int height = static_cast<int>(std::lround(rect.bottomRight.y - rect.topLeft.y));
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }

  return PresentedPixelRect{
      .x = x,
      .y = y,
      .width = width,
      .height = height,
  };
}

}  // namespace donner::editor
