#include "Morphology.h"

#include <algorithm>
#include <cstdint>

namespace tiny_skia::filter {

void morphology(const Pixmap& src, Pixmap& dst, MorphologyOp op, int radiusX, int radiusY) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());

  if (w <= 0 || h <= 0) {
    return;
  }

  // Clamp negative radii to 0. A radius of 0 means a 1-pixel window (no effect in that
  // direction). The caller is responsible for producing transparent output when both radii
  // are zero or either is negative.
  radiusX = std::max(0, radiusX);
  radiusY = std::max(0, radiusY);

  // Cap radius to image dimensions to avoid O(w*h*r^2) blowup on huge values.
  radiusX = std::min(radiusX, w);
  radiusY = std::min(radiusY, h);

  auto srcData = src.data();
  auto dstData = dst.data();

  for (int y = 0; y < h; ++y) {
    const int y0 = std::max(0, y - radiusY);
    const int y1 = std::min(h - 1, y + radiusY);

    for (int x = 0; x < w; ++x) {
      const int x0 = std::max(0, x - radiusX);
      const int x1 = std::min(w - 1, x + radiusX);

      std::uint8_t bestR, bestG, bestB, bestA;
      if (op == MorphologyOp::Erode) {
        bestR = bestG = bestB = bestA = 255;
      } else {
        bestR = bestG = bestB = bestA = 0;
      }

      for (int wy = y0; wy <= y1; ++wy) {
        for (int wx = x0; wx <= x1; ++wx) {
          const int idx = (wy * w + wx) * 4;
          if (op == MorphologyOp::Erode) {
            bestR = std::min(bestR, srcData[idx + 0]);
            bestG = std::min(bestG, srcData[idx + 1]);
            bestB = std::min(bestB, srcData[idx + 2]);
            bestA = std::min(bestA, srcData[idx + 3]);
          } else {
            bestR = std::max(bestR, srcData[idx + 0]);
            bestG = std::max(bestG, srcData[idx + 1]);
            bestB = std::max(bestB, srcData[idx + 2]);
            bestA = std::max(bestA, srcData[idx + 3]);
          }
        }
      }

      const int dstIdx = (y * w + x) * 4;
      dstData[dstIdx + 0] = bestR;
      dstData[dstIdx + 1] = bestG;
      dstData[dstIdx + 2] = bestB;
      dstData[dstIdx + 3] = bestA;
    }
  }
}

}  // namespace tiny_skia::filter
