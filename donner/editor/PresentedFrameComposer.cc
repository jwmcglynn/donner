#include "donner/editor/PresentedFrameComposer.h"

#include <algorithm>
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

bool IsFinite(const Transform2d& candidateDocumentFromDocument) {
  for (double value : candidateDocumentFromDocument.data) {
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

bool IsValidBox(const Box2d& box) {
  return IsValidRect(box.topLeft, box.bottomRight);
}

bool IsValidQuad(const PresentedTileQuad& quad) {
  return IsFinite(quad.topLeft) && IsFinite(quad.topRight) && IsFinite(quad.bottomRight) &&
         IsFinite(quad.bottomLeft);
}

std::optional<Box2d> IntersectBoxes(const Box2d& a, const Box2d& b) {
  const Box2d intersection(
      Vector2d(std::max(a.topLeft.x, b.topLeft.x), std::max(a.topLeft.y, b.topLeft.y)),
      Vector2d(std::min(a.bottomRight.x, b.bottomRight.x),
               std::min(a.bottomRight.y, b.bottomRight.y)));
  if (!IsValidBox(intersection)) {
    return std::nullopt;
  }

  return intersection;
}

void PushValidBox(std::vector<Box2d>* boxes, const Vector2d& topLeft, const Vector2d& bottomRight) {
  if (boxes == nullptr) {
    return;
  }

  const Box2d box(topLeft, bottomRight);
  if (IsValidBox(box)) {
    boxes->push_back(box);
  }
}

Transform2d DocumentFromCachedWithTranslationFallback(const Transform2d& documentFromCachedDocument,
                                                      const Vector2d& translationDoc) {
  if (documentFromCachedDocument.isIdentity() && translationDoc != Vector2d::Zero()) {
    return Transform2d::Translate(translationDoc);
  }
  return documentFromCachedDocument;
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

Transform2d ResolvePresentedTileDocumentTransform(
    const PresentedFrameTileGeometry& tile,
    const std::optional<PresentedDragBaseline>& dragBaseline) {
  const Transform2d tileDocumentFromCachedDocument = DocumentFromCachedWithTranslationFallback(
      tile.documentFromCachedDocument, tile.dragTranslationDoc);
  if (tile.isDragTarget && dragBaseline.has_value()) {
    const Transform2d representedDocumentFromCachedDocument =
        DocumentFromCachedWithTranslationFallback(
            dragBaseline->representedDocumentFromCachedDocument,
            dragBaseline->representedTranslationDoc);
    const Transform2d activeDocumentFromCachedDocument = DocumentFromCachedWithTranslationFallback(
        dragBaseline->activeDocumentFromCachedDocument, dragBaseline->activeTranslationDoc);
    // Re-base the cached bitmap onto the live `active` transform. Transform2d
    // composes in the same order points move through transforms here: first the
    // tile's cached-to-document placement, then the delta from represented to
    // active. Reversing this order conjugates scale/rotate around the wrong
    // frame when a crisp drag recapture lands.
    return tileDocumentFromCachedDocument * representedDocumentFromCachedDocument.inverse() *
           activeDocumentFromCachedDocument;
  }

  return tileDocumentFromCachedDocument;
}

std::optional<PresentedTileQuad> ComputePresentedTileQuad(
    const PresentedFrameTileGeometry& tile, const Transform2d& outputFromCanvasTransform,
    const std::optional<PresentedDragBaseline>& dragBaseline) {
  if (!IsFinite(outputFromCanvasTransform) || !IsFinite(tile.canvasOffsetDoc) ||
      !IsFinite(tile.bitmapDimsDoc) || !IsFinite(tile.dragTranslationDoc) ||
      !IsFinite(tile.documentFromCachedDocument) || tile.bitmapDimsDoc.x <= 0.0 ||
      tile.bitmapDimsDoc.y <= 0.0) {
    return std::nullopt;
  }

  const Vector2d effectiveDragTranslationDoc =
      ResolvePresentedTileDragTranslation(tile, dragBaseline);
  const Transform2d effectiveDocumentFromCachedDocument =
      ResolvePresentedTileDocumentTransform(tile, dragBaseline);
  if (!IsFinite(effectiveDragTranslationDoc) || !IsFinite(effectiveDocumentFromCachedDocument)) {
    return std::nullopt;
  }

  const Box2d canvasBox(tile.canvasOffsetDoc, tile.canvasOffsetDoc + tile.bitmapDimsDoc);
  const Vector2d cachedTopLeft = canvasBox.topLeft;
  const Vector2d cachedTopRight(canvasBox.bottomRight.x, canvasBox.topLeft.y);
  const Vector2d cachedBottomRight = canvasBox.bottomRight;
  const Vector2d cachedBottomLeft(canvasBox.topLeft.x, canvasBox.bottomRight.y);

  const auto presentPoint = [&](const Vector2d& cachedPoint) {
    return outputFromCanvasTransform.transformPosition(
        effectiveDocumentFromCachedDocument.transformPosition(cachedPoint));
  };
  PresentedTileQuad quad{
      .topLeft = presentPoint(cachedTopLeft),
      .topRight = presentPoint(cachedTopRight),
      .bottomRight = presentPoint(cachedBottomRight),
      .bottomLeft = presentPoint(cachedBottomLeft),
      .effectiveDocumentFromCachedDocument = effectiveDocumentFromCachedDocument,
      .effectiveDragTranslationDoc = effectiveDragTranslationDoc,
  };
  if (!IsValidQuad(quad)) {
    return std::nullopt;
  }
  return quad;
}

std::optional<PresentedTileRect> ComputePresentedTileRect(
    const PresentedFrameTileGeometry& tile, const Transform2d& outputFromCanvasTransform,
    const std::optional<PresentedDragBaseline>& dragBaseline) {
  const std::optional<PresentedTileQuad> quad =
      ComputePresentedTileQuad(tile, outputFromCanvasTransform, dragBaseline);
  if (!quad.has_value()) {
    return std::nullopt;
  }

  Box2d outputBox = Box2d::CreateEmpty(quad->topLeft);
  outputBox.addPoint(quad->topRight);
  outputBox.addPoint(quad->bottomRight);
  outputBox.addPoint(quad->bottomLeft);
  if (!IsValidRect(outputBox.topLeft, outputBox.bottomRight)) {
    return std::nullopt;
  }

  return PresentedTileRect{
      .topLeft = outputBox.topLeft,
      .bottomRight = outputBox.bottomRight,
      .effectiveDragTranslationDoc = quad->effectiveDragTranslationDoc,
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

std::vector<Box2d> SubtractPresentedTileBoundsFromClip(const Box2d& clipRect,
                                                       std::span<const Box2d> coveredRects) {
  if (!IsValidBox(clipRect)) {
    return {};
  }

  std::vector<Box2d> remaining{clipRect};
  for (const Box2d& coveredRect : coveredRects) {
    if (!IsValidBox(coveredRect) || remaining.empty()) {
      continue;
    }

    std::vector<Box2d> next;
    next.reserve(remaining.size() * 4u);
    for (const Box2d& rect : remaining) {
      const std::optional<Box2d> intersection = IntersectBoxes(rect, coveredRect);
      if (!intersection.has_value()) {
        next.push_back(rect);
        continue;
      }

      PushValidBox(&next, rect.topLeft, Vector2d(rect.bottomRight.x, intersection->topLeft.y));
      PushValidBox(&next, Vector2d(rect.topLeft.x, intersection->bottomRight.y), rect.bottomRight);
      PushValidBox(&next, Vector2d(rect.topLeft.x, intersection->topLeft.y),
                   Vector2d(intersection->topLeft.x, intersection->bottomRight.y));
      PushValidBox(&next, Vector2d(intersection->bottomRight.x, intersection->topLeft.y),
                   Vector2d(rect.bottomRight.x, intersection->bottomRight.y));
    }
    remaining = std::move(next);
  }

  return remaining;
}

}  // namespace donner::editor
