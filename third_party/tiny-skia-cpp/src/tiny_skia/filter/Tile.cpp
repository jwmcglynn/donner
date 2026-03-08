#include "Tile.h"

#include <algorithm>
#include <cstring>

namespace tiny_skia::filter {

void tile(const Pixmap& src, Pixmap& dst, int tileX, int tileY, int tileW, int tileH) {
  const int w = static_cast<int>(dst.width());
  const int h = static_cast<int>(dst.height());

  if (tileW <= 0 || tileH <= 0 || w <= 0 || h <= 0) {
    return;
  }

  auto srcData = src.data();
  auto dstData = dst.data();

  for (int y = 0; y < h; ++y) {
    // Compute which row of the tile this maps to.
    // Use modulo that handles negative values correctly.
    int ty = ((y - tileY) % tileH);
    if (ty < 0) {
      ty += tileH;
    }
    ty += tileY;
    // Clamp to pixmap bounds.
    if (ty < 0 || ty >= static_cast<int>(src.height())) {
      // This tile row is outside src — leave dst row transparent.
      std::fill_n(dstData.data() + y * w * 4, w * 4, std::uint8_t{0});
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
        dstData[dstIdx + 0] = 0;
        dstData[dstIdx + 1] = 0;
        dstData[dstIdx + 2] = 0;
        dstData[dstIdx + 3] = 0;
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
