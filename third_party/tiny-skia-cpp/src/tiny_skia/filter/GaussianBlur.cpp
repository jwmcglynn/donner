#include "tiny_skia/filter/GaussianBlur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "tiny_skia/filter/SimdVec.h"

namespace tiny_skia::filter {

namespace {

// ---------------------------------------------------------------------------
// Weighted Gaussian kernel for small sigma (σ < 2.0).
// ---------------------------------------------------------------------------

struct WeightedKernel {
  std::vector<float> weights;
  int radius = 0;
};

WeightedKernel makeGaussianKernel(double sigma) {
  if (sigma <= 0.0) {
    return {{1.0f}, 0};
  }

  const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
  std::vector<float> weights(static_cast<std::size_t>(radius * 2 + 1));

  const double twoSigmaSquared = 2.0 * sigma * sigma;
  double sum = 0.0;
  for (int i = -radius; i <= radius; ++i) {
    const double weight = std::exp(-(i * i) / twoSigmaSquared);
    weights[static_cast<std::size_t>(i + radius)] = static_cast<float>(weight);
    sum += weight;
  }

  for (float& weight : weights) {
    weight = static_cast<float>(weight / sum);
  }

  return {std::move(weights), radius};
}

// ---------------------------------------------------------------------------
// Box blur pass parameters for large sigma (σ ≥ 2.0).
// ---------------------------------------------------------------------------

struct BoxPass {
  int left;
  int right;
  int width() const { return left + right + 1; }
};

struct BoxBlurPlan {
  BoxPass passes[4];
  int numPasses = 0;
};

BoxBlurPlan computeBoxPasses(double sigma) {
  const double kWindowScale = 3.0 * std::sqrt(2.0 * std::numbers::pi_v<double>) / 4.0;
  const int window = std::max(1, static_cast<int>(std::floor(sigma * kWindowScale + 0.5)));

  BoxBlurPlan plan;
  if (window <= 1) {
    return plan;
  }

  if ((window & 1) != 0) {
    const int radius = window / 2;
    for (int i = 0; i < 3; ++i) {
      plan.passes[i] = {radius, radius};
    }
    plan.numPasses = 3;
  } else {
    const int half = window / 2;
    plan.passes[0] = {half, half - 1};
    plan.passes[1] = {half - 1, half};
    plan.passes[2] = {half, half};
    plan.numPasses = 3;
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Cache-friendly RGBA pixel transposition for vertical blur optimization.
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

// ---------------------------------------------------------------------------
// Edge mode resolution.
// ---------------------------------------------------------------------------

int resolveEdge(int coord, int size, BlurEdgeMode edgeMode) {
  if (coord >= 0 && coord < size) {
    return coord;
  }
  switch (edgeMode) {
    case BlurEdgeMode::Duplicate: return std::clamp(coord, 0, size - 1);
    case BlurEdgeMode::Wrap: return ((coord % size) + size) % size;
    case BlurEdgeMode::None: return -1;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Weighted convolution (σ < 2.0) — uint8 path.
// ---------------------------------------------------------------------------

void convolveHorizontalWeighted(const std::uint8_t* src, std::uint8_t* dst, int width, int height,
                                const WeightedKernel& kernel, BlurEdgeMode edgeMode) {
  const int kernelSize = static_cast<int>(kernel.weights.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
      for (int k = 0; k < kernelSize; ++k) {
        const int sampleX = resolveEdge(x + k - kernel.radius, width, edgeMode);
        if (sampleX < 0) {
          continue;
        }
        const float w = kernel.weights[static_cast<std::size_t>(k)];
        const std::size_t si = static_cast<std::size_t>((y * width + sampleX) * 4);
        sum0 += w * static_cast<float>(src[si + 0]);
        sum1 += w * static_cast<float>(src[si + 1]);
        sum2 += w * static_cast<float>(src[si + 2]);
        sum3 += w * static_cast<float>(src[si + 3]);
      }
      const std::size_t di = static_cast<std::size_t>((y * width + x) * 4);
      dst[di + 0] = static_cast<std::uint8_t>(std::clamp(std::lround(sum0), 0L, 255L));
      dst[di + 1] = static_cast<std::uint8_t>(std::clamp(std::lround(sum1), 0L, 255L));
      dst[di + 2] = static_cast<std::uint8_t>(std::clamp(std::lround(sum2), 0L, 255L));
      dst[di + 3] = static_cast<std::uint8_t>(std::clamp(std::lround(sum3), 0L, 255L));
    }
  }
}

// ---------------------------------------------------------------------------
// Running-sum box blur (σ ≥ 2.0): O(1) per pixel per pass.
// Uses Vec4u32 SIMD + ScaledDivider (Phases 1+2).
// Phase 7: Loop splitting for EdgeMode::None — eliminates edge checks in bulk middle.
// ---------------------------------------------------------------------------

void boxBlurHorizontal(const std::uint8_t* src, std::uint8_t* dst, int width, int height,
                       const BoxPass& pass, BlurEdgeMode edgeMode) {
  const int kernelWidth = pass.width();
  const ScaledDivider divider(static_cast<std::uint32_t>(kernelWidth));

  for (int y = 0; y < height; ++y) {
    const std::uint8_t* srcRow = src + static_cast<std::size_t>(y) * width * 4;
    std::uint8_t* dstRow = dst + static_cast<std::size_t>(y) * width * 4;

    if (edgeMode == BlurEdgeMode::None) {
      // Fast path: out-of-bounds = zero. Split into 3 sections to eliminate edge checks.
      Vec4u32 sum;

      // Initialize: accumulate window for x=0.
      const int initEnd = std::min(pass.right, width - 1);
      for (int k = 0; k <= initEnd; ++k) {
        sum += Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(k) * 4);
      }
      sum.scaledDivide(divider).storeToU8(dstRow);

      // Section 1: Left edge — leaving pixel is out of bounds (x < pass.left + 1).
      const int leftEdgeEnd = std::min(pass.left + 1, width);
      for (int x = 1; x < leftEdgeEnd; ++x) {
        const int enterX = x + pass.right;
        if (enterX < width) {
          sum += Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(enterX) * 4);
        }
        // leaveX = x - pass.left - 1 < 0, no subtraction needed.
        sum.scaledDivide(divider).storeToU8(dstRow + static_cast<std::size_t>(x) * 4);
      }

      // Section 2: Bulk middle — both entering and leaving pixels are in bounds.
      const int bulkEnd = std::min(width - pass.right, width);
      for (int x = leftEdgeEnd; x < bulkEnd; ++x) {
        sum += Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(x + pass.right) * 4);
        sum -= Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(x - pass.left - 1) * 4);
        sum.scaledDivide(divider).storeToU8(dstRow + static_cast<std::size_t>(x) * 4);
      }

      // Section 3: Right edge — entering pixel is out of bounds.
      for (int x = std::max(leftEdgeEnd, bulkEnd); x < width; ++x) {
        // enterX = x + pass.right >= width, no addition needed.
        const int leaveX = x - pass.left - 1;
        if (leaveX >= 0) {
          sum -= Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(leaveX) * 4);
        }
        sum.scaledDivide(divider).storeToU8(dstRow + static_cast<std::size_t>(x) * 4);
      }
    } else {
      // General path with resolveEdge.
      Vec4u32 sum;
      for (int k = -pass.left; k <= pass.right; ++k) {
        const int sx = resolveEdge(k, width, edgeMode);
        if (sx >= 0) {
          sum += Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(sx) * 4);
        }
      }
      sum.scaledDivide(divider).storeToU8(dstRow);

      for (int x = 1; x < width; ++x) {
        const int enterX = resolveEdge(x + pass.right, width, edgeMode);
        if (enterX >= 0) {
          sum += Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(enterX) * 4);
        }
        const int leaveX = resolveEdge(x - pass.left - 1, width, edgeMode);
        if (leaveX >= 0) {
          sum -= Vec4u32::loadFromU8(srcRow + static_cast<std::size_t>(leaveX) * 4);
        }
        sum.scaledDivide(divider).storeToU8(dstRow + static_cast<std::size_t>(x) * 4);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Float box blur — uses Vec4f32 SIMD for all paths (Phases 2+6).
// Phase 7: Loop splitting for EdgeMode::None.
// ---------------------------------------------------------------------------

void convolveHorizontalWeightedFloat(const float* src, float* dst, int width, int height,
                                     const WeightedKernel& kernel, BlurEdgeMode edgeMode) {
  const int kernelSize = static_cast<int>(kernel.weights.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      Vec4f32 sum;
      for (int k = 0; k < kernelSize; ++k) {
        const int sampleX = resolveEdge(x + k - kernel.radius, width, edgeMode);
        if (sampleX < 0) {
          continue;
        }
        const Vec4f32 w = Vec4f32::splat(kernel.weights[static_cast<std::size_t>(k)]);
        sum += Vec4f32::load(&src[(y * width + sampleX) * 4]) * w;
      }
      sum.clamp01().store(&dst[(y * width + x) * 4]);
    }
  }
}

void boxBlurHorizontalFloat(const float* src, float* dst, int width, int height,
                            const BoxPass& pass, BlurEdgeMode edgeMode) {
  const Vec4f32 invWidth = Vec4f32::splat(1.0f / static_cast<float>(pass.width()));

  for (int y = 0; y < height; ++y) {
    const float* srcRow = src + static_cast<std::size_t>(y) * width * 4;
    float* dstRow = dst + static_cast<std::size_t>(y) * width * 4;

    if (edgeMode == BlurEdgeMode::None) {
      Vec4f32 sum;
      const int initEnd = std::min(pass.right, width - 1);
      for (int k = 0; k <= initEnd; ++k) {
        sum += Vec4f32::load(srcRow + static_cast<std::size_t>(k) * 4);
      }
      (sum * invWidth).store(dstRow);

      // Left edge.
      const int leftEdgeEnd = std::min(pass.left + 1, width);
      for (int x = 1; x < leftEdgeEnd; ++x) {
        const int enterX = x + pass.right;
        if (enterX < width) {
          sum += Vec4f32::load(srcRow + static_cast<std::size_t>(enterX) * 4);
        }
        (sum * invWidth).store(dstRow + static_cast<std::size_t>(x) * 4);
      }

      // Bulk middle — no edge checks.
      const int bulkEnd = std::min(width - pass.right, width);
      for (int x = leftEdgeEnd; x < bulkEnd; ++x) {
        sum += Vec4f32::load(srcRow + static_cast<std::size_t>(x + pass.right) * 4);
        sum -= Vec4f32::load(srcRow + static_cast<std::size_t>(x - pass.left - 1) * 4);
        (sum * invWidth).store(dstRow + static_cast<std::size_t>(x) * 4);
      }

      // Right edge.
      for (int x = std::max(leftEdgeEnd, bulkEnd); x < width; ++x) {
        const int leaveX = x - pass.left - 1;
        if (leaveX >= 0) {
          sum -= Vec4f32::load(srcRow + static_cast<std::size_t>(leaveX) * 4);
        }
        (sum * invWidth).store(dstRow + static_cast<std::size_t>(x) * 4);
      }
    } else {
      Vec4f32 sum;
      for (int k = -pass.left; k <= pass.right; ++k) {
        const int sx = resolveEdge(k, width, edgeMode);
        if (sx >= 0) {
          sum += Vec4f32::load(srcRow + static_cast<std::size_t>(sx) * 4);
        }
      }
      (sum * invWidth).store(dstRow);

      for (int x = 1; x < width; ++x) {
        const int enterX = resolveEdge(x + pass.right, width, edgeMode);
        if (enterX >= 0) {
          sum += Vec4f32::load(srcRow + static_cast<std::size_t>(enterX) * 4);
        }
        const int leaveX = resolveEdge(x - pass.left - 1, width, edgeMode);
        if (leaveX >= 0) {
          sum -= Vec4f32::load(srcRow + static_cast<std::size_t>(leaveX) * 4);
        }
        (sum * invWidth).store(dstRow + static_cast<std::size_t>(x) * 4);
      }
    }
  }
}


}  // namespace

// ---------------------------------------------------------------------------
// Public API: uint8 blur.
// Vertical pass uses transpose + horizontal (cache-friendly for 3 box passes).
// ---------------------------------------------------------------------------

void gaussianBlur(Pixmap& pixmap, double sigmaX, double sigmaY, BlurEdgeMode edgeMode) {
  const int width = static_cast<int>(pixmap.width());
  const int height = static_cast<int>(pixmap.height());
  if (width <= 0 || height <= 0) {
    return;
  }

  // Guard against OOM: each buffer is width*height*4 bytes.
  constexpr std::size_t kMaxAllocationBytes = 256 * 1024 * 1024;
  const std::size_t bufferSize = static_cast<std::size_t>(width) * height * 4;
  if (bufferSize > kMaxAllocationBytes) {
    return;
  }

  std::vector<std::uint8_t> buffer(pixmap.data().begin(), pixmap.data().end());
  std::vector<std::uint8_t> scratch(buffer.size());

  if (sigmaX > 0.0) {
    if (sigmaX < 2.0) {
      const WeightedKernel kernel = makeGaussianKernel(sigmaX);
      convolveHorizontalWeighted(buffer.data(), scratch.data(), width, height, kernel, edgeMode);
      buffer.swap(scratch);
    } else {
      const BoxBlurPlan plan = computeBoxPasses(sigmaX);
      for (int i = 0; i < plan.numPasses; ++i) {
        boxBlurHorizontal(buffer.data(), scratch.data(), width, height, plan.passes[i], edgeMode);
        buffer.swap(scratch);
      }
    }
  }

  if (sigmaY > 0.0) {
    // Transpose for cache-friendly vertical blur.
    transposeRGBA(buffer.data(), scratch.data(), width, height);
    buffer.swap(scratch);

    if (sigmaY < 2.0) {
      const WeightedKernel kernel = makeGaussianKernel(sigmaY);
      convolveHorizontalWeighted(buffer.data(), scratch.data(), height, width, kernel, edgeMode);
      buffer.swap(scratch);
    } else {
      const BoxBlurPlan plan = computeBoxPasses(sigmaY);
      for (int i = 0; i < plan.numPasses; ++i) {
        boxBlurHorizontal(buffer.data(), scratch.data(), height, width, plan.passes[i], edgeMode);
        buffer.swap(scratch);
      }
    }

    // Transpose back.
    transposeRGBA(buffer.data(), scratch.data(), height, width);
    buffer.swap(scratch);
  }

  std::copy(buffer.begin(), buffer.end(), pixmap.data().begin());
}

// ---------------------------------------------------------------------------
// Public API: float blur.
// ---------------------------------------------------------------------------

void gaussianBlur(FloatPixmap& pixmap, double sigmaX, double sigmaY, BlurEdgeMode edgeMode) {
  const int width = static_cast<int>(pixmap.width());
  const int height = static_cast<int>(pixmap.height());
  if (width <= 0 || height <= 0) {
    return;
  }

  // Guard against OOM: each buffer is width*height*4 floats.
  constexpr std::size_t kMaxAllocationBytes = 256 * 1024 * 1024;
  const std::size_t bufferSize = static_cast<std::size_t>(width) * height * 4 * sizeof(float);
  if (bufferSize > kMaxAllocationBytes) {
    return;
  }

  auto srcSpan = pixmap.data();
  std::vector<float> buffer(srcSpan.begin(), srcSpan.end());
  std::vector<float> scratch(buffer.size());

  if (sigmaX > 0.0) {
    if (sigmaX < 2.0) {
      const WeightedKernel kernel = makeGaussianKernel(sigmaX);
      convolveHorizontalWeightedFloat(buffer.data(), scratch.data(), width, height, kernel,
                                      edgeMode);
      buffer.swap(scratch);
    } else {
      const BoxBlurPlan plan = computeBoxPasses(sigmaX);
      for (int i = 0; i < plan.numPasses; ++i) {
        boxBlurHorizontalFloat(buffer.data(), scratch.data(), width, height, plan.passes[i],
                               edgeMode);
        buffer.swap(scratch);
      }
    }
  }

  if (sigmaY > 0.0) {
    transposeRGBA(buffer.data(), scratch.data(), width, height);
    buffer.swap(scratch);

    if (sigmaY < 2.0) {
      const WeightedKernel kernel = makeGaussianKernel(sigmaY);
      convolveHorizontalWeightedFloat(buffer.data(), scratch.data(), height, width, kernel,
                                      edgeMode);
      buffer.swap(scratch);
    } else {
      const BoxBlurPlan plan = computeBoxPasses(sigmaY);
      for (int i = 0; i < plan.numPasses; ++i) {
        boxBlurHorizontalFloat(buffer.data(), scratch.data(), height, width, plan.passes[i],
                               edgeMode);
        buffer.swap(scratch);
      }
    }

    transposeRGBA(buffer.data(), scratch.data(), height, width);
    buffer.swap(scratch);
  }

  std::copy(buffer.begin(), buffer.end(), pixmap.data().begin());
}

}  // namespace tiny_skia::filter
