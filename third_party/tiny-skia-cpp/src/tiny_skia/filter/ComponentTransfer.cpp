#include "tiny_skia/filter/ComponentTransfer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace tiny_skia::filter {

namespace {

/// Build a 256-entry LUT for a transfer function.
std::array<std::uint8_t, 256> buildLut(const TransferFunc& func) {
  std::array<std::uint8_t, 256> lut{};

  for (int i = 0; i < 256; ++i) {
    const double c = i / 255.0;
    double result = c;

    switch (func.type) {
      case TransferFuncType::Identity:
        result = c;
        break;
      case TransferFuncType::Table: {
        const std::size_t n = func.tableValues.size();
        if (n == 0) {
          result = c;
        } else if (n == 1) {
          result = func.tableValues[0];
        } else {
          const double pos = c * static_cast<double>(n - 1);
          const std::size_t k = static_cast<std::size_t>(std::floor(pos));
          const std::size_t kClamped = std::min(k, n - 2);
          const double frac = pos - static_cast<double>(kClamped);
          result = func.tableValues[kClamped] * (1.0 - frac) + func.tableValues[kClamped + 1] * frac;
        }
        break;
      }
      case TransferFuncType::Discrete: {
        const std::size_t n = func.tableValues.size();
        if (n == 0) {
          result = c;
        } else {
          const std::size_t k =
              std::min(static_cast<std::size_t>(std::floor(c * static_cast<double>(n))), n - 1);
          result = func.tableValues[k];
        }
        break;
      }
      case TransferFuncType::Linear:
        result = func.slope * c + func.intercept;
        break;
      case TransferFuncType::Gamma:
        result = func.amplitude * std::pow(c, func.exponent) + func.offset;
        break;
    }

    result = std::clamp(result, 0.0, 1.0);
    lut[i] = static_cast<std::uint8_t>(std::round(result * 255.0));
  }

  return lut;
}

}  // namespace

void componentTransfer(Pixmap& pixmap, const TransferFunc& funcR, const TransferFunc& funcG,
                       const TransferFunc& funcB, const TransferFunc& funcA) {
  // Pre-compute LUTs for each channel.
  const auto lutR = buildLut(funcR);
  const auto lutG = buildLut(funcG);
  const auto lutB = buildLut(funcB);
  const auto lutA = buildLut(funcA);

  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;
    const std::uint8_t a = data[off + 3];

    if (a == 0) {
      // Transparent pixel: unpremultiplied RGB is 0. Apply all transfer functions.
      const std::uint8_t newR = lutR[0];
      const std::uint8_t newG = lutG[0];
      const std::uint8_t newB = lutB[0];
      const std::uint8_t newA = lutA[0];
      if (newA == 0) {
        // Still transparent — leave as zeros.
      } else {
        // Re-premultiply.
        const double alphaFrac = newA / 255.0;
        data[off + 0] =
            static_cast<std::uint8_t>(std::clamp(std::round(newR * alphaFrac), 0.0, 255.0));
        data[off + 1] =
            static_cast<std::uint8_t>(std::clamp(std::round(newG * alphaFrac), 0.0, 255.0));
        data[off + 2] =
            static_cast<std::uint8_t>(std::clamp(std::round(newB * alphaFrac), 0.0, 255.0));
        data[off + 3] = newA;
      }
      continue;
    }

    // Unpremultiply.
    std::uint8_t r, g, b;
    if (a == 255) {
      r = data[off + 0];
      g = data[off + 1];
      b = data[off + 2];
    } else {
      const double invAlpha = 255.0 / a;
      r = static_cast<std::uint8_t>(
          std::clamp(std::round(data[off + 0] * invAlpha), 0.0, 255.0));
      g = static_cast<std::uint8_t>(
          std::clamp(std::round(data[off + 1] * invAlpha), 0.0, 255.0));
      b = static_cast<std::uint8_t>(
          std::clamp(std::round(data[off + 2] * invAlpha), 0.0, 255.0));
    }

    // Apply transfer functions via LUT.
    const std::uint8_t newR = lutR[r];
    const std::uint8_t newG = lutG[g];
    const std::uint8_t newB = lutB[b];
    const std::uint8_t newA = lutA[a];

    // Re-premultiply.
    if (newA == 255) {
      data[off + 0] = newR;
      data[off + 1] = newG;
      data[off + 2] = newB;
      data[off + 3] = 255;
    } else if (newA == 0) {
      data[off + 0] = 0;
      data[off + 1] = 0;
      data[off + 2] = 0;
      data[off + 3] = 0;
    } else {
      const double alphaFrac = newA / 255.0;
      data[off + 0] =
          static_cast<std::uint8_t>(std::clamp(std::round(newR * alphaFrac), 0.0, 255.0));
      data[off + 1] =
          static_cast<std::uint8_t>(std::clamp(std::round(newG * alphaFrac), 0.0, 255.0));
      data[off + 2] =
          static_cast<std::uint8_t>(std::clamp(std::round(newB * alphaFrac), 0.0, 255.0));
      data[off + 3] = newA;
    }
  }
}

namespace {

/// Evaluate a transfer function directly for a float value in [0,1].
double evalTransferFunc(const TransferFunc& func, double c) {
  double result = c;

  switch (func.type) {
    case TransferFuncType::Identity:
      result = c;
      break;
    case TransferFuncType::Table: {
      const std::size_t n = func.tableValues.size();
      if (n == 0) {
        result = c;
      } else if (n == 1) {
        result = func.tableValues[0];
      } else {
        const double pos = c * static_cast<double>(n - 1);
        const std::size_t k = static_cast<std::size_t>(std::floor(pos));
        const std::size_t kClamped = std::min(k, n - 2);
        const double frac = pos - static_cast<double>(kClamped);
        result = func.tableValues[kClamped] * (1.0 - frac) + func.tableValues[kClamped + 1] * frac;
      }
      break;
    }
    case TransferFuncType::Discrete: {
      const std::size_t n = func.tableValues.size();
      if (n == 0) {
        result = c;
      } else {
        const std::size_t k =
            std::min(static_cast<std::size_t>(std::floor(c * static_cast<double>(n))), n - 1);
        result = func.tableValues[k];
      }
      break;
    }
    case TransferFuncType::Linear:
      result = func.slope * c + func.intercept;
      break;
    case TransferFuncType::Gamma:
      result = func.amplitude * std::pow(c, func.exponent) + func.offset;
      break;
  }

  return std::clamp(result, 0.0, 1.0);
}

}  // namespace

void componentTransfer(FloatPixmap& pixmap, const TransferFunc& funcR, const TransferFunc& funcG,
                       const TransferFunc& funcB, const TransferFunc& funcA) {
  auto data = pixmap.data();
  const std::size_t pixelCount = data.size() / 4;

  for (std::size_t i = 0; i < pixelCount; ++i) {
    const std::size_t off = i * 4;
    const float a = data[off + 3];

    if (a == 0.0f) {
      // Transparent pixel: unpremultiplied RGB is 0. Apply all transfer functions.
      const double newR = evalTransferFunc(funcR, 0.0);
      const double newG = evalTransferFunc(funcG, 0.0);
      const double newB = evalTransferFunc(funcB, 0.0);
      const double newA = evalTransferFunc(funcA, 0.0);
      if (newA == 0.0) {
        // Still transparent — leave as zeros.
      } else {
        // Re-premultiply.
        data[off + 0] = static_cast<float>(std::clamp(newR * newA, 0.0, 1.0));
        data[off + 1] = static_cast<float>(std::clamp(newG * newA, 0.0, 1.0));
        data[off + 2] = static_cast<float>(std::clamp(newB * newA, 0.0, 1.0));
        data[off + 3] = static_cast<float>(newA);
      }
      continue;
    }

    // Unpremultiply.
    const double invAlpha = 1.0 / static_cast<double>(a);
    const double r = std::min(1.0, static_cast<double>(data[off + 0]) * invAlpha);
    const double g = std::min(1.0, static_cast<double>(data[off + 1]) * invAlpha);
    const double b = std::min(1.0, static_cast<double>(data[off + 2]) * invAlpha);

    // Apply transfer functions directly.
    const double newR = evalTransferFunc(funcR, r);
    const double newG = evalTransferFunc(funcG, g);
    const double newB = evalTransferFunc(funcB, b);
    const double newA = evalTransferFunc(funcA, static_cast<double>(a));

    // Re-premultiply.
    if (newA == 0.0) {
      data[off + 0] = 0.0f;
      data[off + 1] = 0.0f;
      data[off + 2] = 0.0f;
      data[off + 3] = 0.0f;
    } else {
      data[off + 0] = static_cast<float>(std::clamp(newR * newA, 0.0, 1.0));
      data[off + 1] = static_cast<float>(std::clamp(newG * newA, 0.0, 1.0));
      data[off + 2] = static_cast<float>(std::clamp(newB * newA, 0.0, 1.0));
      data[off + 3] = static_cast<float>(newA);
    }
  }
}

}  // namespace tiny_skia::filter
