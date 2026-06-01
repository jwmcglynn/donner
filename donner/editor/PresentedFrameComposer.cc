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

bool IsValidQuad(const PresentedTileQuad& quad) {
  return IsFinite(quad.topLeft) && IsFinite(quad.topRight) && IsFinite(quad.bottomRight) &&
         IsFinite(quad.bottomLeft);
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
    // The represented→active baseline correction slides the cached drag bitmap
    // from the published drag transform to the live one. It is only
    // geometrically valid when the published bitmap is a translation of the
    // cached pixels — i.e. `represented` is a pure translation. That covers the
    // translation-reuse fast path (cheap `canvasFromBitmap` offset) and the
    // selection-prewarm preview (identity `represented`, where the live affine
    // stretches the cached identity bitmap as an instant preview).
    //
    // For an affine (rotate/scale) drag the compositor RE-RASTERIZES the bitmap
    // at the represented transform: the rotation is baked into the pixels about
    // the shape center and `canvasFromBitmap` collapses to ~identity. Composing
    // `represented⁻¹ * active` onto that already-correct bitmap re-rotates it
    // about the canvas origin — swinging the shape off-center, then snapping it
    // back as the next capture lands (the rotate/scale "lag then reset" QA
    // regression). The re-rasterized bitmap is authoritative, so present it
    // as-is; the capture scheduler keeps it within a frame of the live
    // transform. Pinned by `AffineRepresentedPreviewPresentsReRasterizedBitmapAsIs`
    // and `IdentityRepresentedAffineActiveStillStretchesPrewarmBitmap`.
    if (!representedDocumentFromCachedDocument.isTranslation()) {
      return tileDocumentFromCachedDocument;
    }
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

}  // namespace donner::editor
