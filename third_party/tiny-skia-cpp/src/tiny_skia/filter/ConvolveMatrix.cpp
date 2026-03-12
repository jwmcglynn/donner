#include "ConvolveMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

namespace {

/// Fetch a premultiplied pixel from the source with edge mode handling.
/// Returns premultiplied RGBA as doubles in [0, 1].
void fetchPixel(const std::span<const std::uint8_t>& srcData, int w, int h, int x, int y,
                ConvolveEdgeMode edgeMode, double& r, double& g, double& b, double& a) {
  if (x < 0 || x >= w || y < 0 || y >= h) {
    switch (edgeMode) {
      case ConvolveEdgeMode::Duplicate:
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        break;
      case ConvolveEdgeMode::Wrap:
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
        break;
      case ConvolveEdgeMode::None: r = g = b = a = 0.0; return;
    }
  }

  const int idx = (y * w + x) * 4;
  r = srcData[idx + 0] / 255.0;
  g = srcData[idx + 1] / 255.0;
  b = srcData[idx + 2] / 255.0;
  a = srcData[idx + 3] / 255.0;
}

}  // namespace

void convolveMatrix(const Pixmap& src, Pixmap& dst, const ConvolveParams& params) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());

  if (w <= 0 || h <= 0 || params.orderX <= 0 || params.orderY <= 0) {
    return;
  }

  if (static_cast<int>(params.kernel.size()) < params.orderX * params.orderY) {
    return;
  }

  auto srcData = src.data();
  auto dstData = dst.data();

  const double invDivisor = 1.0 / params.divisor;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumA = 0.0;

      for (int ky = 0; ky < params.orderY; ++ky) {
        for (int kx = 0; kx < params.orderX; ++kx) {
          const int srcX = x - params.targetX + kx;
          const int srcY = y - params.targetY + ky;
          const double weight = params.kernel[ky * params.orderX + kx];

          double pr, pg, pb, pa;
          fetchPixel(srcData, w, h, srcX, srcY, params.edgeMode, pr, pg, pb, pa);

          sumR += weight * pr;
          sumG += weight * pg;
          sumB += weight * pb;
          sumA += weight * pa;
        }
      }

      // Per spec: bias is multiplied by the source alpha at (x,y).
      // The formula is: result = sum/divisor + bias * ALPHA(x,y)
      const int centerIdx = (y * w + x) * 4;
      const double srcAlpha = srcData[centerIdx + 3] / 255.0;

      // The convolution operates on premultiplied values.
      double outR = sumR * invDivisor + params.bias * srcAlpha;
      double outG = sumG * invDivisor + params.bias * srcAlpha;
      double outB = sumB * invDivisor + params.bias * srcAlpha;
      double outA = sumA * invDivisor + params.bias * srcAlpha;

      if (params.preserveAlpha) {
        outA = srcAlpha;
      }

      // Clamp to [0, 1].
      outA = std::clamp(outA, 0.0, 1.0);
      outR = std::clamp(outR, 0.0, outA);
      outG = std::clamp(outG, 0.0, outA);
      outB = std::clamp(outB, 0.0, outA);

      // Write premultiplied output.
      const int dstIdx = (y * w + x) * 4;
      dstData[dstIdx + 0] = static_cast<std::uint8_t>(std::round(outR * 255.0));
      dstData[dstIdx + 1] = static_cast<std::uint8_t>(std::round(outG * 255.0));
      dstData[dstIdx + 2] = static_cast<std::uint8_t>(std::round(outB * 255.0));
      dstData[dstIdx + 3] = static_cast<std::uint8_t>(std::round(outA * 255.0));
    }
  }
}

namespace {

/// Fetch a premultiplied pixel from a float source with edge mode handling.
/// Returns premultiplied RGBA as doubles in [0, 1].
void fetchPixelFloat(const std::span<const float>& srcData, int w, int h, int x, int y,
                     ConvolveEdgeMode edgeMode, double& r, double& g, double& b, double& a) {
  if (x < 0 || x >= w || y < 0 || y >= h) {
    switch (edgeMode) {
      case ConvolveEdgeMode::Duplicate:
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        break;
      case ConvolveEdgeMode::Wrap:
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
        break;
      case ConvolveEdgeMode::None: r = g = b = a = 0.0; return;
    }
  }

  const int idx = (y * w + x) * 4;
  r = srcData[idx + 0];
  g = srcData[idx + 1];
  b = srcData[idx + 2];
  a = srcData[idx + 3];
}

}  // namespace

void convolveMatrix(const FloatPixmap& src, FloatPixmap& dst, const ConvolveParams& params) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());

  if (w <= 0 || h <= 0 || params.orderX <= 0 || params.orderY <= 0) {
    return;
  }

  if (static_cast<int>(params.kernel.size()) < params.orderX * params.orderY) {
    return;
  }

  auto srcData = src.data();
  auto dstData = dst.data();

  const double invDivisor = 1.0 / params.divisor;

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumA = 0.0;

      for (int ky = 0; ky < params.orderY; ++ky) {
        for (int kx = 0; kx < params.orderX; ++kx) {
          const int srcX = x - params.targetX + kx;
          const int srcY = y - params.targetY + ky;
          const double weight = params.kernel[ky * params.orderX + kx];

          double pr, pg, pb, pa;
          fetchPixelFloat(srcData, w, h, srcX, srcY, params.edgeMode, pr, pg, pb, pa);

          sumR += weight * pr;
          sumG += weight * pg;
          sumB += weight * pb;
          sumA += weight * pa;
        }
      }

      const int centerIdx = (y * w + x) * 4;
      const double srcAlpha = srcData[centerIdx + 3];

      double outR = sumR * invDivisor + params.bias * srcAlpha;
      double outG = sumG * invDivisor + params.bias * srcAlpha;
      double outB = sumB * invDivisor + params.bias * srcAlpha;
      double outA = sumA * invDivisor + params.bias * srcAlpha;

      if (params.preserveAlpha) {
        outA = srcAlpha;
      }

      // Clamp to [0, 1].
      outA = std::clamp(outA, 0.0, 1.0);
      outR = std::clamp(outR, 0.0, outA);
      outG = std::clamp(outG, 0.0, outA);
      outB = std::clamp(outB, 0.0, outA);

      const int dstIdx = (y * w + x) * 4;
      dstData[dstIdx + 0] = static_cast<float>(outR);
      dstData[dstIdx + 1] = static_cast<float>(outG);
      dstData[dstIdx + 2] = static_cast<float>(outB);
      dstData[dstIdx + 3] = static_cast<float>(outA);
    }
  }
}

}  // namespace tiny_skia::filter
