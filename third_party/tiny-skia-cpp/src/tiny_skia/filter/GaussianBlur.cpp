#include "tiny_skia/filter/GaussianBlur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

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

/// Parameters for a single box blur pass (asymmetric window).
struct BoxPass {
  int left;   ///< Samples to the left of center.
  int right;  ///< Samples to the right of center.
  int width() const { return left + right + 1; }
};

/// Sequence of box passes that approximate a Gaussian blur.
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
    // Odd window: 3 passes of the same symmetric box.
    const int radius = window / 2;
    for (int i = 0; i < 3; ++i) {
      plan.passes[i] = {radius, radius};
    }
    plan.numPasses = 3;
  } else {
    // Even window: 3 passes with shifted/centered boxes.
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
// Instead of column-wise blur (cache miss per row), transpose → row-wise blur → transpose.
// ---------------------------------------------------------------------------

/// Transpose an RGBA image: dst[x * srcHeight + y] = src[y * srcWidth + x] for each pixel.
/// Uses block tiling for cache friendliness.
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

/// Resolve an out-of-bounds coordinate based on edge mode.
/// Returns the resolved coordinate, or -1 if the pixel should be treated as transparent black.
int resolveEdge(int coord, int size, BlurEdgeMode edgeMode) {
  if (coord >= 0 && coord < size) {
    return coord;
  }
  switch (edgeMode) {
    case BlurEdgeMode::Duplicate: return std::clamp(coord, 0, size - 1);
    case BlurEdgeMode::Wrap: return ((coord % size) + size) % size;
    case BlurEdgeMode::None: return -1;  // transparent black
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Weighted convolution (σ < 2.0): process all 4 channels per pixel.
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
// ---------------------------------------------------------------------------

void boxBlurHorizontal(const std::uint8_t* src, std::uint8_t* dst, int width, int height,
                       const BoxPass& pass, BlurEdgeMode edgeMode) {
  const int kernelWidth = pass.width();
  const std::uint32_t halfKernel = static_cast<std::uint32_t>(kernelWidth) / 2u;

  for (int y = 0; y < height; ++y) {
    // Initialize running sums for the first pixel's window.
    std::uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int k = -pass.left; k <= pass.right; ++k) {
      const int sx = resolveEdge(k, width, edgeMode);
      if (sx >= 0) {
        const std::size_t si = static_cast<std::size_t>((y * width + sx) * 4);
        s0 += src[si + 0];
        s1 += src[si + 1];
        s2 += src[si + 2];
        s3 += src[si + 3];
      }
    }

    const std::size_t di0 = static_cast<std::size_t>(y * width * 4);
    dst[di0 + 0] = static_cast<std::uint8_t>((s0 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));
    dst[di0 + 1] = static_cast<std::uint8_t>((s1 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));
    dst[di0 + 2] = static_cast<std::uint8_t>((s2 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));
    dst[di0 + 3] = static_cast<std::uint8_t>((s3 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));

    // Slide the window across the row.
    for (int x = 1; x < width; ++x) {
      // Add entering pixel.
      const int enterX = resolveEdge(x + pass.right, width, edgeMode);
      if (enterX >= 0) {
        const std::size_t ei = static_cast<std::size_t>((y * width + enterX) * 4);
        s0 += src[ei + 0];
        s1 += src[ei + 1];
        s2 += src[ei + 2];
        s3 += src[ei + 3];
      }
      // Remove leaving pixel.
      const int leaveX = resolveEdge(x - pass.left - 1, width, edgeMode);
      if (leaveX >= 0) {
        const std::size_t li = static_cast<std::size_t>((y * width + leaveX) * 4);
        s0 -= src[li + 0];
        s1 -= src[li + 1];
        s2 -= src[li + 2];
        s3 -= src[li + 3];
      }

      const std::size_t di = static_cast<std::size_t>((y * width + x) * 4);
      dst[di + 0] = static_cast<std::uint8_t>((s0 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));
      dst[di + 1] = static_cast<std::uint8_t>((s1 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));
      dst[di + 2] = static_cast<std::uint8_t>((s2 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));
      dst[di + 3] = static_cast<std::uint8_t>((s3 + halfKernel) / static_cast<std::uint32_t>(kernelWidth));
    }
  }
}

// Vertical box blur removed — uses transpose + horizontal blur + transpose instead.

// ---------------------------------------------------------------------------
// Float variants.
// ---------------------------------------------------------------------------

void convolveHorizontalWeightedFloat(const float* src, float* dst, int width, int height,
                                     const WeightedKernel& kernel, BlurEdgeMode edgeMode) {
  const int kernelSize = static_cast<int>(kernel.weights.size());
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
      float32x4_t sum = vdupq_n_f32(0.0f);
      for (int k = 0; k < kernelSize; ++k) {
        const int sampleX = resolveEdge(x + k - kernel.radius, width, edgeMode);
        if (sampleX < 0) {
          continue;
        }
        const float32x4_t w = vdupq_n_f32(kernel.weights[static_cast<std::size_t>(k)]);
        const std::size_t si = static_cast<std::size_t>((y * width + sampleX) * 4);
        sum = vmlaq_f32(sum, vld1q_f32(&src[si]), w);
      }
      const std::size_t di = static_cast<std::size_t>((y * width + x) * 4);
      const float32x4_t zero = vdupq_n_f32(0.0f);
      const float32x4_t one = vdupq_n_f32(1.0f);
      vst1q_f32(&dst[di], vminq_f32(vmaxq_f32(sum, zero), one));
#else
      float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
      for (int k = 0; k < kernelSize; ++k) {
        const int sampleX = resolveEdge(x + k - kernel.radius, width, edgeMode);
        if (sampleX < 0) {
          continue;
        }
        const float w = kernel.weights[static_cast<std::size_t>(k)];
        const std::size_t si = static_cast<std::size_t>((y * width + sampleX) * 4);
        sum0 += w * src[si + 0];
        sum1 += w * src[si + 1];
        sum2 += w * src[si + 2];
        sum3 += w * src[si + 3];
      }
      const std::size_t di = static_cast<std::size_t>((y * width + x) * 4);
      dst[di + 0] = std::clamp(sum0, 0.0f, 1.0f);
      dst[di + 1] = std::clamp(sum1, 0.0f, 1.0f);
      dst[di + 2] = std::clamp(sum2, 0.0f, 1.0f);
      dst[di + 3] = std::clamp(sum3, 0.0f, 1.0f);
#endif
    }
  }
}

void boxBlurHorizontalFloat(const float* src, float* dst, int width, int height,
                            const BoxPass& pass, BlurEdgeMode edgeMode) {
  const float invWidth = 1.0f / static_cast<float>(pass.width());

  for (int y = 0; y < height; ++y) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    float32x4_t sum = vdupq_n_f32(0.0f);
    for (int k = -pass.left; k <= pass.right; ++k) {
      const int sx = resolveEdge(k, width, edgeMode);
      if (sx >= 0) {
        const std::size_t si = static_cast<std::size_t>((y * width + sx) * 4);
        sum = vaddq_f32(sum, vld1q_f32(&src[si]));
      }
    }

    const float32x4_t vInvWidth = vdupq_n_f32(invWidth);
    const std::size_t di0 = static_cast<std::size_t>(y * width * 4);
    vst1q_f32(&dst[di0], vmulq_f32(sum, vInvWidth));

    for (int x = 1; x < width; ++x) {
      const int enterX = resolveEdge(x + pass.right, width, edgeMode);
      if (enterX >= 0) {
        const std::size_t ei = static_cast<std::size_t>((y * width + enterX) * 4);
        sum = vaddq_f32(sum, vld1q_f32(&src[ei]));
      }
      const int leaveX = resolveEdge(x - pass.left - 1, width, edgeMode);
      if (leaveX >= 0) {
        const std::size_t li = static_cast<std::size_t>((y * width + leaveX) * 4);
        sum = vsubq_f32(sum, vld1q_f32(&src[li]));
      }

      const std::size_t di = static_cast<std::size_t>((y * width + x) * 4);
      vst1q_f32(&dst[di], vmulq_f32(sum, vInvWidth));
    }
#else
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (int k = -pass.left; k <= pass.right; ++k) {
      const int sx = resolveEdge(k, width, edgeMode);
      if (sx >= 0) {
        const std::size_t si = static_cast<std::size_t>((y * width + sx) * 4);
        s0 += src[si + 0];
        s1 += src[si + 1];
        s2 += src[si + 2];
        s3 += src[si + 3];
      }
    }

    const std::size_t di0 = static_cast<std::size_t>(y * width * 4);
    dst[di0 + 0] = static_cast<float>(s0 * invWidth);
    dst[di0 + 1] = static_cast<float>(s1 * invWidth);
    dst[di0 + 2] = static_cast<float>(s2 * invWidth);
    dst[di0 + 3] = static_cast<float>(s3 * invWidth);

    for (int x = 1; x < width; ++x) {
      const int enterX = resolveEdge(x + pass.right, width, edgeMode);
      if (enterX >= 0) {
        const std::size_t ei = static_cast<std::size_t>((y * width + enterX) * 4);
        s0 += src[ei + 0];
        s1 += src[ei + 1];
        s2 += src[ei + 2];
        s3 += src[ei + 3];
      }
      const int leaveX = resolveEdge(x - pass.left - 1, width, edgeMode);
      if (leaveX >= 0) {
        const std::size_t li = static_cast<std::size_t>((y * width + leaveX) * 4);
        s0 -= src[li + 0];
        s1 -= src[li + 1];
        s2 -= src[li + 2];
        s3 -= src[li + 3];
      }

      const std::size_t di = static_cast<std::size_t>((y * width + x) * 4);
      dst[di + 0] = static_cast<float>(s0 * invWidth);
      dst[di + 1] = static_cast<float>(s1 * invWidth);
      dst[di + 2] = static_cast<float>(s2 * invWidth);
      dst[di + 3] = static_cast<float>(s3 * invWidth);
    }
#endif
  }
}

// Vertical float blur removed — uses transpose + horizontal blur + transpose instead.

}  // namespace

// ---------------------------------------------------------------------------
// Public API: uint8 blur.
// ---------------------------------------------------------------------------

void gaussianBlur(Pixmap& pixmap, double sigmaX, double sigmaY, BlurEdgeMode edgeMode) {
  const int width = static_cast<int>(pixmap.width());
  const int height = static_cast<int>(pixmap.height());
  if (width <= 0 || height <= 0) {
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
    // Transpose so that vertical blur becomes a cache-friendly horizontal blur.
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

    // Transpose back to original orientation.
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
    // Transpose so that vertical blur becomes a cache-friendly horizontal blur.
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

    // Transpose back to original orientation.
    transposeRGBA(buffer.data(), scratch.data(), height, width);
    buffer.swap(scratch);
  }

  std::copy(buffer.begin(), buffer.end(), pixmap.data().begin());
}

}  // namespace tiny_skia::filter
