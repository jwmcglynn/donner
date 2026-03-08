#include "tiny_skia/filter/GaussianBlur.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace tiny_skia::filter {

namespace {

struct ConvolutionKernel {
  std::vector<float> weights;
  std::vector<std::uint32_t> numerators;
  std::uint32_t divisor = 1;
  int origin = 0;
};

ConvolutionKernel makeGaussianKernel(double sigma) {
  if (sigma <= 0.0) {
    return {{1.0f}, {}, 1u, 0};
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

  return {std::move(weights), {}, 1u, radius};
}

ConvolutionKernel makeBoxKernel(int minOffset, int maxOffset) {
  const int width = maxOffset - minOffset + 1;
  std::vector<std::uint32_t> numerators(static_cast<std::size_t>(width), 1u);
  return {{}, std::move(numerators), static_cast<std::uint32_t>(width), -minOffset};
}

ConvolutionKernel convolveKernels(const ConvolutionKernel& lhs, const ConvolutionKernel& rhs) {
  ConvolutionKernel result;
  result.origin = lhs.origin + rhs.origin;
  result.divisor = lhs.divisor * rhs.divisor;

  if (!lhs.numerators.empty() && !rhs.numerators.empty()) {
    result.numerators.assign(lhs.numerators.size() + rhs.numerators.size() - 1u, 0u);
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.numerators.size(); ++lhsIndex) {
      for (std::size_t rhsIndex = 0; rhsIndex < rhs.numerators.size(); ++rhsIndex) {
        result.numerators[lhsIndex + rhsIndex] +=
            lhs.numerators[lhsIndex] * rhs.numerators[rhsIndex];
      }
    }
  } else {
    result.weights.assign(lhs.weights.size() + rhs.weights.size() - 1u, 0.0f);
    for (std::size_t lhsIndex = 0; lhsIndex < lhs.weights.size(); ++lhsIndex) {
      for (std::size_t rhsIndex = 0; rhsIndex < rhs.weights.size(); ++rhsIndex) {
        result.weights[lhsIndex + rhsIndex] += lhs.weights[lhsIndex] * rhs.weights[rhsIndex];
      }
    }
  }

  return result;
}

ConvolutionKernel makeBoxApproximationKernel(double sigma) {
  if (sigma <= 0.0) {
    return {{1.0f}, {}, 1u, 0};
  }

  const double kWindowScale = 3.0 * std::sqrt(2.0 * std::numbers::pi_v<double>) / 4.0;
  const int window = std::max(1, static_cast<int>(std::floor(sigma * kWindowScale + 0.5)));
  if (window <= 1) {
    return {{1.0f}, {}, 1u, 0};
  }

  if ((window & 1) != 0) {
    const int radius = window / 2;
    const ConvolutionKernel box = makeBoxKernel(-radius, radius);
    return convolveKernels(convolveKernels(box, box), box);
  }

  const int half = window / 2;
  const ConvolutionKernel leftShifted = makeBoxKernel(-half, half - 1);
  const ConvolutionKernel rightShifted = makeBoxKernel(-(half - 1), half);
  const ConvolutionKernel centered = makeBoxKernel(-half, half);
  return convolveKernels(convolveKernels(leftShifted, rightShifted), centered);
}

ConvolutionKernel makeBlurKernel(double sigma) {
  if (sigma < 2.0) {
    return makeGaussianKernel(sigma);
  }
  return makeBoxApproximationKernel(sigma);
}

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

void convolveHorizontal(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst,
                        int width, int height, const ConvolutionKernel& kernel,
                        BlurEdgeMode edgeMode) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!kernel.numerators.empty()) {
          std::uint64_t sum = 0;
          for (std::size_t kernelIndex = 0; kernelIndex < kernel.numerators.size(); ++kernelIndex) {
            const int sampleX =
                resolveEdge(x + static_cast<int>(kernelIndex) - kernel.origin, width, edgeMode);
            if (sampleX < 0) {
              continue;
            }

            const std::size_t srcIndex =
                static_cast<std::size_t>((y * width + sampleX) * 4 + channel);
            sum += static_cast<std::uint64_t>(kernel.numerators[kernelIndex]) * src[srcIndex];
          }

          const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
          dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(
              static_cast<long>((sum + kernel.divisor / 2u) / kernel.divisor), 0L, 255L));
          continue;
        }

        float sum = 0.0f;
        for (std::size_t kernelIndex = 0; kernelIndex < kernel.weights.size(); ++kernelIndex) {
          const int sampleX =
              resolveEdge(x + static_cast<int>(kernelIndex) - kernel.origin, width, edgeMode);
          if (sampleX < 0) {
            continue;
          }

          const std::size_t srcIndex =
              static_cast<std::size_t>((y * width + sampleX) * 4 + channel);
          sum += kernel.weights[kernelIndex] * static_cast<float>(src[srcIndex]);
        }

        const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
        dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(std::lround(sum), 0L, 255L));
      }
    }
  }
}

void convolveVertical(const std::vector<std::uint8_t>& src, std::vector<std::uint8_t>& dst,
                      int width, int height, const ConvolutionKernel& kernel,
                      BlurEdgeMode edgeMode) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!kernel.numerators.empty()) {
          std::uint64_t sum = 0;
          for (std::size_t kernelIndex = 0; kernelIndex < kernel.numerators.size(); ++kernelIndex) {
            const int sampleY =
                resolveEdge(y + static_cast<int>(kernelIndex) - kernel.origin, height, edgeMode);
            if (sampleY < 0) {
              continue;
            }

            const std::size_t srcIndex =
                static_cast<std::size_t>((sampleY * width + x) * 4 + channel);
            sum += static_cast<std::uint64_t>(kernel.numerators[kernelIndex]) * src[srcIndex];
          }

          const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
          dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(
              static_cast<long>((sum + kernel.divisor / 2u) / kernel.divisor), 0L, 255L));
          continue;
        }

        float sum = 0.0f;
        for (std::size_t kernelIndex = 0; kernelIndex < kernel.weights.size(); ++kernelIndex) {
          const int sampleY =
              resolveEdge(y + static_cast<int>(kernelIndex) - kernel.origin, height, edgeMode);
          if (sampleY < 0) {
            continue;
          }

          const std::size_t srcIndex =
              static_cast<std::size_t>((sampleY * width + x) * 4 + channel);
          sum += kernel.weights[kernelIndex] * static_cast<float>(src[srcIndex]);
        }

        const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
        dst[dstIndex] = static_cast<std::uint8_t>(std::clamp(std::lround(sum), 0L, 255L));
      }
    }
  }
}

}  // namespace

void gaussianBlur(Pixmap& pixmap, double sigmaX, double sigmaY, BlurEdgeMode edgeMode) {
  const int width = static_cast<int>(pixmap.width());
  const int height = static_cast<int>(pixmap.height());
  if (width <= 0 || height <= 0) {
    return;
  }

  std::vector<std::uint8_t> buffer(pixmap.data().begin(), pixmap.data().end());
  std::vector<std::uint8_t> scratch(buffer.size());

  if (sigmaX > 0.0) {
    const ConvolutionKernel kernel = makeBlurKernel(sigmaX);
    convolveHorizontal(buffer, scratch, width, height, kernel, edgeMode);
    buffer.swap(scratch);
  }

  if (sigmaY > 0.0) {
    const ConvolutionKernel kernel = makeBlurKernel(sigmaY);
    convolveVertical(buffer, scratch, width, height, kernel, edgeMode);
    buffer.swap(scratch);
  }

  std::copy(buffer.begin(), buffer.end(), pixmap.data().begin());
}

namespace {

void convolveHorizontalFloat(const std::vector<float>& src, std::vector<float>& dst, int width,
                             int height, const ConvolutionKernel& kernel, BlurEdgeMode edgeMode) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!kernel.numerators.empty()) {
          double sum = 0.0;
          for (std::size_t kernelIndex = 0; kernelIndex < kernel.numerators.size(); ++kernelIndex) {
            const int sampleX =
                resolveEdge(x + static_cast<int>(kernelIndex) - kernel.origin, width, edgeMode);
            if (sampleX < 0) {
              continue;
            }

            const std::size_t srcIndex =
                static_cast<std::size_t>((y * width + sampleX) * 4 + channel);
            sum += static_cast<double>(kernel.numerators[kernelIndex]) * src[srcIndex];
          }

          const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
          dst[dstIndex] = static_cast<float>(
              std::clamp(sum / static_cast<double>(kernel.divisor), 0.0, 1.0));
          continue;
        }

        float sum = 0.0f;
        for (std::size_t kernelIndex = 0; kernelIndex < kernel.weights.size(); ++kernelIndex) {
          const int sampleX =
              resolveEdge(x + static_cast<int>(kernelIndex) - kernel.origin, width, edgeMode);
          if (sampleX < 0) {
            continue;
          }

          const std::size_t srcIndex =
              static_cast<std::size_t>((y * width + sampleX) * 4 + channel);
          sum += kernel.weights[kernelIndex] * src[srcIndex];
        }

        const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
        dst[dstIndex] = std::clamp(sum, 0.0f, 1.0f);
      }
    }
  }
}

void convolveVerticalFloat(const std::vector<float>& src, std::vector<float>& dst, int width,
                           int height, const ConvolutionKernel& kernel, BlurEdgeMode edgeMode) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!kernel.numerators.empty()) {
          double sum = 0.0;
          for (std::size_t kernelIndex = 0; kernelIndex < kernel.numerators.size(); ++kernelIndex) {
            const int sampleY =
                resolveEdge(y + static_cast<int>(kernelIndex) - kernel.origin, height, edgeMode);
            if (sampleY < 0) {
              continue;
            }

            const std::size_t srcIndex =
                static_cast<std::size_t>((sampleY * width + x) * 4 + channel);
            sum += static_cast<double>(kernel.numerators[kernelIndex]) * src[srcIndex];
          }

          const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
          dst[dstIndex] = static_cast<float>(
              std::clamp(sum / static_cast<double>(kernel.divisor), 0.0, 1.0));
          continue;
        }

        float sum = 0.0f;
        for (std::size_t kernelIndex = 0; kernelIndex < kernel.weights.size(); ++kernelIndex) {
          const int sampleY =
              resolveEdge(y + static_cast<int>(kernelIndex) - kernel.origin, height, edgeMode);
          if (sampleY < 0) {
            continue;
          }

          const std::size_t srcIndex =
              static_cast<std::size_t>((sampleY * width + x) * 4 + channel);
          sum += kernel.weights[kernelIndex] * src[srcIndex];
        }

        const std::size_t dstIndex = static_cast<std::size_t>((y * width + x) * 4 + channel);
        dst[dstIndex] = std::clamp(sum, 0.0f, 1.0f);
      }
    }
  }
}

}  // namespace

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
    const ConvolutionKernel kernel = makeBlurKernel(sigmaX);
    convolveHorizontalFloat(buffer, scratch, width, height, kernel, edgeMode);
    buffer.swap(scratch);
  }

  if (sigmaY > 0.0) {
    const ConvolutionKernel kernel = makeBlurKernel(sigmaY);
    convolveVerticalFloat(buffer, scratch, width, height, kernel, edgeMode);
    buffer.swap(scratch);
  }

  std::copy(buffer.begin(), buffer.end(), pixmap.data().begin());
}

}  // namespace tiny_skia::filter
