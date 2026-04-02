// SVG feDisplacementMap implementation.
// Reference: https://www.w3.org/TR/filter-effects/#feDisplacementMapElement

#include "tiny_skia/filter/DisplacementMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace tiny_skia::filter {

namespace {

// Extract a channel value from a premultiplied RGBA pixel.
// For displacement, we need the unpremultiplied channel value.
// map pixel is premultiplied: stored_R = R * A / 255.
// To get unpremultiplied: R = stored_R * 255 / A (or 0 if A=0).
double getChannelUnpremultiplied(const uint8_t* pixel, DisplacementChannel ch) {
  const int idx = static_cast<int>(ch);  // R=0, G=1, B=2, A=3
  if (ch == DisplacementChannel::A) {
    // Alpha is not premultiplied.
    return static_cast<double>(pixel[3]) / 255.0;
  }
  const uint8_t a = pixel[3];
  if (a == 0) {
    return 0.0;
  }
  // Unpremultiply: channel = stored * 255 / alpha.
  return std::min(1.0, static_cast<double>(pixel[idx]) * 255.0 / (static_cast<double>(a) * 255.0));
}

// Bilinear sample from a premultiplied RGBA pixmap at floating-point coordinates.
// Returns transparent black for out-of-bounds coordinates.
void sampleBilinear(const Pixmap& src, double fx, double fy, uint8_t* out) {
  const int w = src.width();
  const int h = src.height();

  // Floor coordinates.
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const double fracX = fx - x0;
  const double fracY = fy - y0;

  auto data = src.data();

  // Fetch corner pixels (transparent black if out of bounds).
  auto fetch = [&](int x, int y) -> const uint8_t* {
    static const uint8_t kTransparent[4] = {0, 0, 0, 0};
    if (x < 0 || x >= w || y < 0 || y >= h) {
      return kTransparent;
    }
    return data.data() + (y * w + x) * 4;
  };

  const uint8_t* p00 = fetch(x0, y0);
  const uint8_t* p10 = fetch(x1, y0);
  const uint8_t* p01 = fetch(x0, y1);
  const uint8_t* p11 = fetch(x1, y1);

  // Interpolate each channel (premultiplied, so interpolation is correct).
  for (int c = 0; c < 4; c++) {
    const double top = p00[c] + fracX * (p10[c] - p00[c]);
    const double bot = p01[c] + fracX * (p11[c] - p01[c]);
    const double val = top + fracY * (bot - top);
    out[c] = static_cast<uint8_t>(std::clamp(std::lround(val), 0L, 255L));
  }
}

}  // namespace

void displacementMap(const Pixmap& src, const Pixmap& map, Pixmap& dst, double scale,
                     DisplacementChannel xCh, DisplacementChannel yCh) {
  const int w = dst.width();
  const int h = dst.height();

  if (w <= 0 || h <= 0) {
    return;
  }

  // If scale is 0, the output is just the source image.
  if (scale == 0.0) {
    std::memcpy(dst.data().data(), src.data().data(), dst.data().size());
    return;
  }

  auto mapData = map.data();
  auto dstData = dst.data();

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const int mapOff = (y * w + x) * 4;
      const uint8_t* mapPixel = mapData.data() + mapOff;

      // Get displacement from the map's selected channels (unpremultiplied).
      const double dx = scale * (getChannelUnpremultiplied(mapPixel, xCh) - 0.5);
      const double dy = scale * (getChannelUnpremultiplied(mapPixel, yCh) - 0.5);

      // Sample the source image at the displaced position.
      const double srcX = static_cast<double>(x) + dx;
      const double srcY = static_cast<double>(y) + dy;

      const int dstOff = (y * w + x) * 4;
      sampleBilinear(src, srcX, srcY, dstData.data() + dstOff);
    }
  }
}

void displacementMap(const FloatPixmap& src, const FloatPixmap& map, FloatPixmap& dst, double scale,
                     DisplacementChannel xCh, DisplacementChannel yCh) {
  const int w = dst.width();
  const int h = dst.height();

  if (w <= 0 || h <= 0) {
    return;
  }

  if (scale == 0.0) {
    std::memcpy(dst.data().data(), src.data().data(), dst.data().size() * sizeof(float));
    return;
  }

  const float* mapPtr = map.data().data();
  const float* srcPtr = src.data().data();
  float* dstPtr = dst.data().data();
  const int srcW = src.width();
  const int srcH = src.height();
  const float fscale = static_cast<float>(scale);
  const int xIdx = static_cast<int>(xCh);
  const int yIdx = static_cast<int>(yCh);
  const bool xIsAlpha = (xCh == DisplacementChannel::A);
  const bool yIsAlpha = (yCh == DisplacementChannel::A);

  // Precompute displacement values to avoid per-pixel division for unpremultiply.
  const std::size_t pixelCount = static_cast<std::size_t>(w) * h;
  constexpr std::size_t kMaxPixels = 64 * 1024 * 1024 / sizeof(float);
  if (pixelCount > kMaxPixels) {
    return;  // Allocation too large, skip displacement.
  }
  std::vector<float> dxValues(pixelCount);
  std::vector<float> dyValues(pixelCount);
  for (std::size_t i = 0; i < pixelCount; ++i) {
    const float* mp = mapPtr + i * 4;
    const float alpha = mp[3];
    const float invAlpha = alpha > 0.0f ? 1.0f / alpha : 0.0f;

    dxValues[i] = xIsAlpha ? alpha : std::min(1.0f, mp[xIdx] * invAlpha);
    dyValues[i] = yIsAlpha ? alpha : std::min(1.0f, mp[yIdx] * invAlpha);
  }

  // Transparent pixel constant for out-of-bounds fetches.
  static const float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const std::size_t pixIdx = static_cast<std::size_t>(y * w + x);
      const float fx = static_cast<float>(x) + fscale * (dxValues[pixIdx] - 0.5f);
      const float fy = static_cast<float>(y) + fscale * (dyValues[pixIdx] - 0.5f);

      // Inline bilinear sampling with float math.
      const int x0 = static_cast<int>(std::floor(fx));
      const int y0 = static_cast<int>(std::floor(fy));
      const float fracX = fx - static_cast<float>(x0);
      const float fracY = fy - static_cast<float>(y0);

      // Fast path: all 4 samples are in bounds.
      const std::size_t dstOff = pixIdx * 4;
      if (x0 >= 0 && x0 + 1 < srcW && y0 >= 0 && y0 + 1 < srcH) {
        const float* p00 = srcPtr + static_cast<std::size_t>((y0 * srcW + x0)) * 4;
        const float* p10 = p00 + 4;
        const float* p01 = p00 + static_cast<std::size_t>(srcW) * 4;
        const float* p11 = p01 + 4;

        for (int c = 0; c < 4; c++) {
          const float top = p00[c] + fracX * (p10[c] - p00[c]);
          const float bot = p01[c] + fracX * (p11[c] - p01[c]);
          dstPtr[dstOff + c] = std::clamp(top + fracY * (bot - top), 0.0f, 1.0f);
        }
      } else {
        // Slow path: bounds checking per sample.
        auto fetch = [&](int px, int py) -> const float* {
          if (px < 0 || px >= srcW || py < 0 || py >= srcH) {
            return kTransparent;
          }
          return srcPtr + static_cast<std::size_t>((py * srcW + px)) * 4;
        };

        const float* p00 = fetch(x0, y0);
        const float* p10 = fetch(x0 + 1, y0);
        const float* p01 = fetch(x0, y0 + 1);
        const float* p11 = fetch(x0 + 1, y0 + 1);

        for (int c = 0; c < 4; c++) {
          const float top = p00[c] + fracX * (p10[c] - p00[c]);
          const float bot = p01[c] + fracX * (p11[c] - p01[c]);
          dstPtr[dstOff + c] = std::clamp(top + fracY * (bot - top), 0.0f, 1.0f);
        }
      }
    }
  }
}

}  // namespace tiny_skia::filter
