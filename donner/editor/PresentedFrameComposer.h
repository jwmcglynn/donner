#pragma once
/// @file

#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::editor {

/// Backend-neutral geometry for one presented composited tile.
struct PresentedFrameTileGeometry {
  /// Tile origin in canvas/document coordinates.
  Vector2d canvasOffsetDoc = Vector2d::Zero();
  /// Tile size in document units.
  Vector2d bitmapDimsDoc = Vector2d::Zero();
  /// Cached drag translation captured with the tile.
  Vector2d dragTranslationDoc = Vector2d::Zero();
  /// Cached affine transform from the tile's rasterized document placement
  /// to its presented document placement.
  Transform2d documentFromCachedDocument = Transform2d();
  /// Whether this tile represents the current drag target.
  bool isDragTarget = false;
};

/// Active drag baseline needed to resolve a presented tile offset.
struct PresentedDragBaseline {
  /// Entity whose cached layer is being previewed.
  Entity entity = entt::null;
  /// Document-space translation already represented by the presented tiles.
  Vector2d representedTranslationDoc = Vector2d::Zero();
  /// Current active drag translation in document space.
  Vector2d activeTranslationDoc = Vector2d::Zero();
  /// Document transform represented by the presented tiles.
  Transform2d representedDocumentFromCachedDocument = Transform2d();
  /// Current active document transform.
  Transform2d activeDocumentFromCachedDocument = Transform2d();
};

/// Output-space quad for one presented tile.
struct PresentedTileQuad {
  /// Top-left corner in output coordinates.
  Vector2d topLeft = Vector2d::Zero();
  /// Top-right corner in output coordinates.
  Vector2d topRight = Vector2d::Zero();
  /// Bottom-right corner in output coordinates.
  Vector2d bottomRight = Vector2d::Zero();
  /// Bottom-left corner in output coordinates.
  Vector2d bottomLeft = Vector2d::Zero();
  /// Affine transform used to derive this quad.
  Transform2d effectiveDocumentFromCachedDocument = Transform2d();
  /// Drag translation used to derive this quad.
  Vector2d effectiveDragTranslationDoc = Vector2d::Zero();
};

/// Output-space rectangle for one presented tile.
struct PresentedTileRect {
  /// Top-left corner in output coordinates.
  Vector2d topLeft = Vector2d::Zero();
  /// Bottom-right corner in output coordinates.
  Vector2d bottomRight = Vector2d::Zero();
  /// Drag translation used to derive this rectangle.
  Vector2d effectiveDragTranslationDoc = Vector2d::Zero();

  friend bool operator==(const PresentedTileRect&, const PresentedTileRect&) = default;
};

/// Integer output-space rectangle used by headless bitmap presentation.
struct PresentedPixelRect {
  /// Top-left x coordinate in pixels.
  int x = 0;
  /// Top-left y coordinate in pixels.
  int y = 0;
  /// Width in pixels.
  int width = 0;
  /// Height in pixels.
  int height = 0;

  friend bool operator==(const PresentedPixelRect&, const PresentedPixelRect&) = default;
};

/**
 * Resolve the drag translation to apply while presenting a cached tile.
 *
 * @param tile Geometry for the tile being presented.
 * @param dragBaseline Active drag baseline represented by the presented tiles.
 * @return Translation to add to the tile's canvas offset.
 */
[[nodiscard]] Vector2d ResolvePresentedTileDragTranslation(
    const PresentedFrameTileGeometry& tile,
    const std::optional<PresentedDragBaseline>& dragBaseline);

/**
 * Resolve the affine transform to apply while presenting a cached tile.
 *
 * @param tile Geometry for the tile being presented.
 * @param dragBaseline Active drag baseline represented by the presented tiles.
 * @return Transform from cached document placement to presented document placement.
 */
[[nodiscard]] Transform2d ResolvePresentedTileDocumentTransform(
    const PresentedFrameTileGeometry& tile,
    const std::optional<PresentedDragBaseline>& dragBaseline);

/**
 * Resolve the screen-space-independent translation for a full-canvas overlay texture.
 *
 * @param dragBaseline Active drag baseline represented by the overlay texture.
 * @return Translation to add to the overlay texture's canvas-space placement.
 */
[[nodiscard]] Vector2d ResolvePresentedOverlayDragTranslation(
    const std::optional<PresentedDragBaseline>& dragBaseline);

/**
 * Resolve the affine transform from displayed overlay placement to active overlay placement.
 *
 * @param dragBaseline Active drag baseline represented by the overlay texture.
 * @return Transform from displayed document placement to active document placement.
 */
[[nodiscard]] Transform2d ResolvePresentedOverlayDocumentTransform(
    const std::optional<PresentedDragBaseline>& dragBaseline);

/**
 * Compute the output-space quad for a presented tile.
 *
 * @param tile Geometry for the tile being presented.
 * @param outputFromCanvasTransform Transform from canvas/document coordinates to output space.
 * @param dragBaseline Active drag baseline represented by the presented tiles.
 * @return Output quad, or \c std::nullopt when tile or transform geometry is invalid.
 */
[[nodiscard]] std::optional<PresentedTileQuad> ComputePresentedTileQuad(
    const PresentedFrameTileGeometry& tile, const Transform2d& outputFromCanvasTransform,
    const std::optional<PresentedDragBaseline>& dragBaseline);

/**
 * Compute the output-space rectangle for a presented tile.
 *
 * @param tile Geometry for the tile being presented.
 * @param outputFromCanvasTransform Transform from canvas/document coordinates to output space.
 * @param dragBaseline Active drag baseline represented by the presented tiles.
 * @return Output rectangle, or \c std::nullopt when tile or transform geometry is invalid.
 */
[[nodiscard]] std::optional<PresentedTileRect> ComputePresentedTileRect(
    const PresentedFrameTileGeometry& tile, const Transform2d& outputFromCanvasTransform,
    const std::optional<PresentedDragBaseline>& dragBaseline);

/**
 * Round a floating-point presented rectangle to integer pixel coordinates.
 *
 * @param rect Presented tile rectangle in output coordinates.
 * @return Pixel rectangle, or \c std::nullopt when the rectangle is invalid or rounds to no area.
 */
[[nodiscard]] std::optional<PresentedPixelRect> RoundPresentedTileRectToPixelRect(
    const PresentedTileRect& rect);

}  // namespace donner::editor
