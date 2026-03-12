#include "Morphology.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tiny_skia::filter {

namespace {

// ---------------------------------------------------------------------------
// Van Herk/Gil-Werman 1D morphology — O(n) regardless of radius.
//
// For each position x, computes min/max over the window [x-radius, x+radius].
// Uses two auxiliary arrays (forward scan and backward scan) computed in blocks
// of size (2*radius+1). The result at each position is the op of at most two
// pre-accumulated values.
// ---------------------------------------------------------------------------

/// 1D horizontal morphology pass over RGBA uint8 data.
/// Reads from src, writes to dst. Both have dimensions width×height with stride = width*4.
template <typename OpFunc>
void vanHerkHorizontal(const std::uint8_t* src, std::uint8_t* dst, int width, int height,
                       int radius, OpFunc op, std::uint8_t identity) {
  if (radius <= 0) {
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
      std::copy_n(src + y * rowBytes, rowBytes, dst + y * rowBytes);
    }
    return;
  }

  const int windowSize = 2 * radius + 1;
  // Auxiliary buffers for forward and backward scans (one row, 4 channels).
  std::vector<std::uint8_t> fwd(static_cast<std::size_t>(width) * 4);
  std::vector<std::uint8_t> bwd(static_cast<std::size_t>(width) * 4);

  for (int y = 0; y < height; ++y) {
    const std::uint8_t* row = src + static_cast<std::size_t>(y) * width * 4;
    std::uint8_t* out = dst + static_cast<std::size_t>(y) * width * 4;

    // Forward scan: accumulate op within each block of windowSize.
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

    // Backward scan: accumulate op within each block, scanning right to left.
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

    // Merge: result[x] = op(fwd[x + radius], bwd[x - radius]).
    for (int x = 0; x < width; ++x) {
      const int left = x - radius;
      const int right = x + radius;
      const std::size_t oi = static_cast<std::size_t>(x) * 4;

      if (left < 0 && right >= width) {
        // Window extends past both edges — scan the clamped range.
        const std::size_t fi = static_cast<std::size_t>(std::min(right, width - 1)) * 4;
        out[oi + 0] = fwd[fi + 0];
        out[oi + 1] = fwd[fi + 1];
        out[oi + 2] = fwd[fi + 2];
        out[oi + 3] = fwd[fi + 3];
      } else if (left < 0) {
        // Window extends past left edge only.
        const std::size_t fi = static_cast<std::size_t>(right) * 4;
        out[oi + 0] = fwd[fi + 0];
        out[oi + 1] = fwd[fi + 1];
        out[oi + 2] = fwd[fi + 2];
        out[oi + 3] = fwd[fi + 3];
      } else if (right >= width) {
        // Window extends past right edge only.
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

// ---------------------------------------------------------------------------
// Block transpose for cache-friendly vertical pass (reused from GaussianBlur).
// ---------------------------------------------------------------------------

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
// Horizontal pass, then transpose → horizontal pass → transpose for vertical.
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

  // Allocate working buffers.
  const std::size_t totalBytes = static_cast<std::size_t>(w) * h * 4;
  std::vector<std::uint8_t> buffer(totalBytes);
  std::vector<std::uint8_t> scratch(totalBytes);

  auto minOp = [](std::uint8_t a, std::uint8_t b) { return std::min(a, b); };
  auto maxOp = [](std::uint8_t a, std::uint8_t b) { return std::max(a, b); };

  if (op == MorphologyOp::Erode) {
    // Horizontal pass.
    vanHerkHorizontal(srcData.data(), buffer.data(), w, h, radiusX, minOp, 255);
    // Vertical pass via transpose → horizontal → transpose.
    transposeRGBA(buffer.data(), scratch.data(), w, h);
    vanHerkHorizontal(scratch.data(), buffer.data(), h, w, radiusY, minOp, 255);
    transposeRGBA(buffer.data(), scratch.data(), h, w);
  } else {
    vanHerkHorizontal(srcData.data(), buffer.data(), w, h, radiusX, maxOp, 0);
    transposeRGBA(buffer.data(), scratch.data(), w, h);
    vanHerkHorizontal(scratch.data(), buffer.data(), h, w, radiusY, maxOp, 0);
    transposeRGBA(buffer.data(), scratch.data(), h, w);
  }

  std::copy(scratch.begin(), scratch.end(), dstData.begin());
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
