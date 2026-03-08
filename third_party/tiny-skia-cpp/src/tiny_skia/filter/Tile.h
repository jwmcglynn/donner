#pragma once

#include "tiny_skia/Pixmap.h"
#include "tiny_skia/filter/FloatPixmap.h"

namespace tiny_skia::filter {

/**
 * Tile the content of \p src within the rectangle (tileX, tileY, tileW, tileH) across
 * the entire \p dst pixmap.
 *
 * The tile rectangle defines which portion of the source pixmap is used as the repeating tile.
 * This tile is then repeated in all directions to fill the destination.
 *
 * @param src Source pixmap containing the tile content.
 * @param dst Destination pixmap to fill with tiled content. Must be same size as src.
 * @param tileX Left edge of the tile rectangle in pixels.
 * @param tileY Top edge of the tile rectangle in pixels.
 * @param tileW Width of the tile rectangle in pixels.
 * @param tileH Height of the tile rectangle in pixels.
 */
void tile(const Pixmap& src, Pixmap& dst, int tileX, int tileY, int tileW, int tileH);

/// Float-precision version of tile.
void tile(const FloatPixmap& src, FloatPixmap& dst, int tileX, int tileY, int tileW, int tileH);

}  // namespace tiny_skia::filter
