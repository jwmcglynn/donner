#include "Morphology.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

namespace {

// ---------------------------------------------------------------------------
// Van Herk/Gil-Werman 1D morphology with SIMD (Phase 5).
// Uses Vec4u8 for per-pixel min/max across all 4 RGBA channels simultaneously.
// Parameterized by stride for horizontal/vertical passes (Phase 4).
// ---------------------------------------------------------------------------

/// 1D morphology pass over RGBA uint8 data using Vec4u8 SIMD.
/// @param src       Source pixel data.
/// @param dst       Destination pixel data.
/// @param length    Number of pixels along the processing axis.
/// @param count     Number of rows/columns to process.
/// @param pixStride Byte offset between consecutive pixels along the axis (4 for horizontal,
///                  width*4 for vertical).
/// @param rowStride Byte offset between consecutive rows/columns (width*4 for horizontal,
///                  4 for vertical).
/// @param radius    Morphology radius.
/// @param isMax     true for dilate (max), false for erode (min).
void vanHerkPass(const std::uint8_t* src, std::uint8_t* dst, int length, int count,
                 int pixStride, int rowStride, int radius, bool isMax) {
  if (radius <= 0) {
    // Just copy.
    for (int row = 0; row < count; ++row) {
      const std::uint8_t* srcRow = src + static_cast<std::size_t>(row) * rowStride;
      std::uint8_t* dstRow = dst + static_cast<std::size_t>(row) * rowStride;
      for (int x = 0; x < length; ++x) {
        Vec4u8::load(srcRow + static_cast<std::size_t>(x) * pixStride)
            .store(dstRow + static_cast<std::size_t>(x) * pixStride);
      }
    }
    return;
  }

  const int windowSize = 2 * radius + 1;
  // Auxiliary buffers for forward and backward scans.
  std::vector<Vec4u8> fwd(static_cast<std::size_t>(length));
  std::vector<Vec4u8> bwd(static_cast<std::size_t>(length));

  for (int row = 0; row < count; ++row) {
    const std::uint8_t* rowSrc = src + static_cast<std::size_t>(row) * rowStride;
    std::uint8_t* rowDst = dst + static_cast<std::size_t>(row) * rowStride;

    auto loadPixel = [&](int x) -> Vec4u8 {
      return Vec4u8::load(rowSrc + static_cast<std::size_t>(x) * pixStride);
    };

    auto op = [isMax](Vec4u8 a, Vec4u8 b) -> Vec4u8 {
      return isMax ? Vec4u8::max(a, b) : Vec4u8::min(a, b);
    };

    // Forward scan: accumulate op within each block of windowSize.
    for (int blockStart = 0; blockStart < length; blockStart += windowSize) {
      const int blockEnd = std::min(blockStart + windowSize, length);
      fwd[static_cast<std::size_t>(blockStart)] = loadPixel(blockStart);
      for (int x = blockStart + 1; x < blockEnd; ++x) {
        fwd[static_cast<std::size_t>(x)] =
            op(fwd[static_cast<std::size_t>(x - 1)], loadPixel(x));
      }
    }

    // Backward scan.
    for (int blockStart = 0; blockStart < length; blockStart += windowSize) {
      const int blockEnd = std::min(blockStart + windowSize, length);
      const int lastX = blockEnd - 1;
      bwd[static_cast<std::size_t>(lastX)] = loadPixel(lastX);
      for (int x = lastX - 1; x >= blockStart; --x) {
        bwd[static_cast<std::size_t>(x)] =
            op(bwd[static_cast<std::size_t>(x + 1)], loadPixel(x));
      }
    }

    // Merge: result[x] = op(fwd[x+radius], bwd[x-radius]).
    for (int x = 0; x < length; ++x) {
      const int left = x - radius;
      const int right = x + radius;
      Vec4u8 result;

      if (left < 0 && right >= length) {
        result = fwd[static_cast<std::size_t>(std::min(right, length - 1))];
      } else if (left < 0) {
        result = fwd[static_cast<std::size_t>(right)];
      } else if (right >= length) {
        result = bwd[static_cast<std::size_t>(left)];
      } else {
        result = op(fwd[static_cast<std::size_t>(right)], bwd[static_cast<std::size_t>(left)]);
      }

      result.store(rowDst + static_cast<std::size_t>(x) * pixStride);
    }
  }
}

/// 1D horizontal morphology pass over RGBA float data.
template <typename OpFunc>
void vanHerkHorizontalFloat(const float* src, float* dst, int width, int height, int radius,
                            OpFunc op, float identity) {
  if (radius <= 0) {
    const std::size_t rowFloats = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
      std::copy_n(src + y * rowFloats, rowFloats, dst + y * rowFloats);
    }
    return;
  }

  const int windowSize = 2 * radius + 1;
  std::vector<float> fwd(static_cast<std::size_t>(width) * 4);
  std::vector<float> bwd(static_cast<std::size_t>(width) * 4);

  for (int y = 0; y < height; ++y) {
    const float* row = src + static_cast<std::size_t>(y) * width * 4;
    float* out = dst + static_cast<std::size_t>(y) * width * 4;

    // Forward scan.
    for (int blockStart = 0; blockStart < width; blockStart += windowSize) {
      const int blockEnd = std::min(blockStart + windowSize, width);
      const std::size_t si = static_cast<std::size_t>(blockStart) * 4;
      fwd[si + 0] = row[si + 0];
      fwd[si + 1] = row[si + 1];
      fwd[si + 2] = row[si + 2];
      fwd[si + 3] = row[si + 3];
      for (int x = blockStart + 1; x < blockEnd; ++x) {
        const std::size_t ci = static_cast<std::size_t>(x) * 4;
        const std::size_t pi = ci - 4;
        fwd[ci + 0] = op(fwd[pi + 0], row[ci + 0]);
        fwd[ci + 1] = op(fwd[pi + 1], row[ci + 1]);
        fwd[ci + 2] = op(fwd[pi + 2], row[ci + 2]);
        fwd[ci + 3] = op(fwd[pi + 3], row[ci + 3]);
      }
    }

    // Backward scan.
    for (int blockStart = 0; blockStart < width; blockStart += windowSize) {
      const int blockEnd = std::min(blockStart + windowSize, width);
      const int lastX = blockEnd - 1;
      const std::size_t li = static_cast<std::size_t>(lastX) * 4;
      bwd[li + 0] = row[li + 0];
      bwd[li + 1] = row[li + 1];
      bwd[li + 2] = row[li + 2];
      bwd[li + 3] = row[li + 3];
      for (int x = lastX - 1; x >= blockStart; --x) {
        const std::size_t ci = static_cast<std::size_t>(x) * 4;
        const std::size_t ni = ci + 4;
        bwd[ci + 0] = op(bwd[ni + 0], row[ci + 0]);
        bwd[ci + 1] = op(bwd[ni + 1], row[ci + 1]);
        bwd[ci + 2] = op(bwd[ni + 2], row[ci + 2]);
        bwd[ci + 3] = op(bwd[ni + 3], row[ci + 3]);
      }
    }

    // Merge.
    for (int x = 0; x < width; ++x) {
      const int left = x - radius;
      const int right = x + radius;
      const std::size_t oi = static_cast<std::size_t>(x) * 4;

      if (left < 0 && right >= width) {
        const std::size_t fi = static_cast<std::size_t>(std::min(right, width - 1)) * 4;
        out[oi + 0] = fwd[fi + 0];
        out[oi + 1] = fwd[fi + 1];
        out[oi + 2] = fwd[fi + 2];
        out[oi + 3] = fwd[fi + 3];
      } else if (left < 0) {
        const std::size_t fi = static_cast<std::size_t>(right) * 4;
        out[oi + 0] = fwd[fi + 0];
        out[oi + 1] = fwd[fi + 1];
        out[oi + 2] = fwd[fi + 2];
        out[oi + 3] = fwd[fi + 3];
      } else if (right >= width) {
        const std::size_t bi = static_cast<std::size_t>(left) * 4;
        out[oi + 0] = bwd[bi + 0];
        out[oi + 1] = bwd[bi + 1];
        out[oi + 2] = bwd[bi + 2];
        out[oi + 3] = bwd[bi + 3];
      } else {
        const std::size_t fi = static_cast<std::size_t>(right) * 4;
        const std::size_t bi = static_cast<std::size_t>(left) * 4;
        out[oi + 0] = op(fwd[fi + 0], bwd[bi + 0]);
        out[oi + 1] = op(fwd[fi + 1], bwd[bi + 1]);
        out[oi + 2] = op(fwd[fi + 2], bwd[bi + 2]);
        out[oi + 3] = op(fwd[fi + 3], bwd[bi + 3]);
      }
    }
  }
}

// Block transpose for cache-friendly vertical pass.
template <typename T>
void transposeRGBA(const T* src, T* dst, int srcWidth, int srcHeight) {
  constexpr int kBlock = 32;
  for (int by = 0; by < srcHeight; by += kBlock) {
    const int yEnd = std::min(by + kBlock, srcHeight);
    for (int bx = 0; bx < srcWidth; bx += kBlock) {
      const int xEnd = std::min(bx + kBlock, srcWidth);
      for (int y = by; y < yEnd; ++y) {
        for (int x = bx; x < xEnd; ++x) {
          const std::size_t si = static_cast<std::size_t>((y * srcWidth + x) * 4);
          const std::size_t di = static_cast<std::size_t>((x * srcHeight + y) * 4);
          dst[di + 0] = src[si + 0];
          dst[di + 1] = src[si + 1];
          dst[di + 2] = src[si + 2];
          dst[di + 3] = src[si + 3];
        }
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API: separable van Herk/Gil-Werman morphology.
// Phase 4: stride-based vertical pass, no transpose needed.
// Phase 5: SIMD min/max via Vec4u8/Vec4f32.
// ---------------------------------------------------------------------------

void morphology(const Pixmap& src, Pixmap& dst, MorphologyOp op, int radiusX, int radiusY) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());

  if (w <= 0 || h <= 0) {
    return;
  }

  radiusX = std::clamp(radiusX, 0, w);
  radiusY = std::clamp(radiusY, 0, h);

  auto srcData = src.data();
  auto dstData = dst.data();

  const std::size_t totalBytes = static_cast<std::size_t>(w) * h * 4;
  std::vector<std::uint8_t> buffer(totalBytes);
  const bool isMax = (op == MorphologyOp::Dilate);

  const int rowBytes = w * 4;

  // Horizontal pass: pixStride=4, rowStride=w*4.
  vanHerkPass(srcData.data(), buffer.data(), w, h, 4, rowBytes, radiusX, isMax);

  // Vertical pass: pixStride=w*4, rowStride=4. length=h, count=w.
  vanHerkPass(buffer.data(), dstData.data(), h, w, rowBytes, 4, radiusY, isMax);
}

void morphology(const FloatPixmap& src, FloatPixmap& dst, MorphologyOp op, int radiusX,
                int radiusY) {
  const int w = static_cast<int>(src.width());
  const int h = static_cast<int>(src.height());

  if (w <= 0 || h <= 0) {
    return;
  }

  radiusX = std::clamp(radiusX, 0, w);
  radiusY = std::clamp(radiusY, 0, h);

  auto srcData = src.data();
  auto dstData = dst.data();

  const std::size_t totalFloats = static_cast<std::size_t>(w) * h * 4;
  std::vector<float> buffer(srcData.begin(), srcData.end());
  std::vector<float> scratch(totalFloats);

  auto minOp = [](float a, float b) { return std::min(a, b); };
  auto maxOp = [](float a, float b) { return std::max(a, b); };

  if (op == MorphologyOp::Erode) {
    vanHerkHorizontalFloat(buffer.data(), scratch.data(), w, h, radiusX, minOp, 1.0f);
    std::vector<float> transposed(totalFloats);
    transposeRGBA(scratch.data(), transposed.data(), w, h);
    vanHerkHorizontalFloat(transposed.data(), scratch.data(), h, w, radiusY, minOp, 1.0f);
    transposeRGBA(scratch.data(), transposed.data(), h, w);
    std::copy(transposed.begin(), transposed.end(), dstData.begin());
  } else {
    vanHerkHorizontalFloat(buffer.data(), scratch.data(), w, h, radiusX, maxOp, 0.0f);
    std::vector<float> transposed(totalFloats);
    transposeRGBA(scratch.data(), transposed.data(), w, h);
    vanHerkHorizontalFloat(transposed.data(), scratch.data(), h, w, radiusY, maxOp, 0.0f);
    transposeRGBA(scratch.data(), transposed.data(), h, w);
    std::copy(transposed.begin(), transposed.end(), dstData.begin());
  }
}

}  // namespace tiny_skia::filter
