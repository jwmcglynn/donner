#include "Tile.h"

#include <algorithm>
#include <cstring>

namespace tiny_skia::filter {

void tile(const Pixmap& src, Pixmap& dst, int tileX, int tileY, int tileW, int tileH) {
  const int w = static_cast<int>(dst.width());
  const int h = static_cast<int>(dst.height());
  const int srcW = static_cast<int>(src.width());
  const int srcH = static_cast<int>(src.height());

  if (tileW <= 0 || tileH <= 0 || w <= 0 || h <= 0) {
    return;
  }

  auto srcData = src.data();
  auto dstData = dst.data();

  // Build one full-width tile row, then memcpy it for repeated Y tiles.
  // This avoids per-pixel modulo arithmetic.
  for (int y = 0; y < h; ++y) {
    int ty = ((y - tileY) % tileH);
    if (ty < 0) {
      ty += tileH;
    }
    ty += tileY;

    auto* dstRow = dstData.data() + static_cast<std::size_t>(y) * w * 4;

    if (ty < 0 || ty >= srcH) {
      std::memset(dstRow, 0, static_cast<std::size_t>(w) * 4);
      continue;
    }

    const auto* srcRow = srcData.data() + static_cast<std::size_t>(ty) * srcW * 4;

    // Build the row by copying tile-width chunks.
    // First, determine the tile source range clamped to src bounds.
    const int clampedTileX = std::max(0, tileX);
    const int clampedTileR = std::min(srcW, tileX + tileW);
    const int clampedW = clampedTileR - clampedTileX;

    if (clampedW <= 0) {
      std::memset(dstRow, 0, static_cast<std::size_t>(w) * 4);
      continue;
    }

    // Compute the starting x offset within the tile for x=0.
    int startTx = ((-tileX) % tileW);
    if (startTx < 0) {
      startTx += tileW;
    }
    // startTx is now the offset into the tile for dst x=0.
    // Map that back to src coordinates.
    int x = 0;

    // Handle initial partial tile.
    int offsetInTile = startTx;
    if (offsetInTile >= clampedW) {
      // Initial offset lands in the transparent region before tile wraps.
      int skip = tileW - offsetInTile;
      if (skip > 0 && x < w) {
        int clearLen = std::min(skip, w - x);
        std::memset(dstRow + x * 4, 0, static_cast<std::size_t>(clearLen) * 4);
        x += clearLen;
      }
      offsetInTile = 0;
    }

    // Copy the remainder of the first partial tile.
    if (offsetInTile > 0 && x < w) {
      int copyLen = std::min(clampedW - offsetInTile, w - x);
      if (copyLen > 0) {
        std::memcpy(dstRow + x * 4, srcRow + (clampedTileX + offsetInTile) * 4,
                     static_cast<std::size_t>(copyLen) * 4);
        x += copyLen;
      }
      // Clear any transparent gap at end of tile.
      int gap = tileW - clampedW;
      if (gap > 0 && x < w) {
        int clearLen = std::min(gap, w - x);
        std::memset(dstRow + x * 4, 0, static_cast<std::size_t>(clearLen) * 4);
        x += clearLen;
      }
    }

    // Copy full tile repeats.
    const auto* tileSrc = srcRow + clampedTileX * 4;
    const int tileGap = tileW - clampedW;
    while (x + tileW <= w) {
      std::memcpy(dstRow + x * 4, tileSrc, static_cast<std::size_t>(clampedW) * 4);
      x += clampedW;
      if (tileGap > 0) {
        std::memset(dstRow + x * 4, 0, static_cast<std::size_t>(tileGap) * 4);
        x += tileGap;
      }
    }

    // Copy final partial tile.
    if (x < w) {
      int remain = w - x;
      int copyLen = std::min(clampedW, remain);
      std::memcpy(dstRow + x * 4, tileSrc, static_cast<std::size_t>(copyLen) * 4);
      x += copyLen;
      if (x < w) {
        std::memset(dstRow + x * 4, 0, static_cast<std::size_t>(w - x) * 4);
      }
    }
  }
}

void tile(const FloatPixmap& src, FloatPixmap& dst, int tileX, int tileY, int tileW, int tileH) {
  const int w = static_cast<int>(dst.width());
  const int h = static_cast<int>(dst.height());

  if (tileW <= 0 || tileH <= 0 || w <= 0 || h <= 0) {
    return;
  }

  auto srcData = src.data();
  auto dstData = dst.data();

  for (int y = 0; y < h; ++y) {
    // Compute which row of the tile this maps to.
    int ty = ((y - tileY) % tileH);
    if (ty < 0) {
      ty += tileH;
    }
    ty += tileY;
    // Clamp to pixmap bounds.
    if (ty < 0 || ty >= static_cast<int>(src.height())) {
      // This tile row is outside src -- leave dst row transparent.
      std::fill_n(dstData.data() + y * w * 4, w * 4, 0.0f);
      continue;
    }

    for (int x = 0; x < w; ++x) {
      int tx = ((x - tileX) % tileW);
      if (tx < 0) {
        tx += tileW;
      }
      tx += tileX;
      // Clamp to pixmap bounds.
      if (tx < 0 || tx >= static_cast<int>(src.width())) {
        const int dstIdx = (y * w + x) * 4;
        dstData[dstIdx + 0] = 0.0f;
        dstData[dstIdx + 1] = 0.0f;
        dstData[dstIdx + 2] = 0.0f;
        dstData[dstIdx + 3] = 0.0f;
        continue;
      }

      const int srcIdx = (ty * static_cast<int>(src.width()) + tx) * 4;
      const int dstIdx = (y * w + x) * 4;
      dstData[dstIdx + 0] = srcData[srcIdx + 0];
      dstData[dstIdx + 1] = srcData[srcIdx + 1];
      dstData[dstIdx + 2] = srcData[srcIdx + 2];
      dstData[dstIdx + 3] = srcData[srcIdx + 3];
    }
  }
}

}  // namespace tiny_skia::filter
