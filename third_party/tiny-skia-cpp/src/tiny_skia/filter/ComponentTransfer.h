#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "tiny_skia/Pixmap.h"

namespace tiny_skia::filter {

/// Transfer function type for feComponentTransfer.
enum class TransferFuncType : std::uint8_t {
  Identity,
  Table,
  Discrete,
  Linear,
  Gamma,
};

/// A transfer function for a single channel.
struct TransferFunc {
  TransferFuncType type = TransferFuncType::Identity;
  std::span<const double> tableValues;
  double slope = 1.0;
  double intercept = 0.0;
  double amplitude = 1.0;
  double exponent = 1.0;
  double offset = 0.0;
};

/**
 * Apply per-channel transfer functions to each pixel.
 *
 * For each pixel, unpremultiplies, applies the per-channel function,
 * clamps to [0, 255], and re-premultiplies.
 *
 * @param pixmap The pixmap to modify in-place (premultiplied RGBA).
 * @param funcR Transfer function for the red channel.
 * @param funcG Transfer function for the green channel.
 * @param funcB Transfer function for the blue channel.
 * @param funcA Transfer function for the alpha channel.
 */
void componentTransfer(Pixmap& pixmap, const TransferFunc& funcR, const TransferFunc& funcG,
                       const TransferFunc& funcB, const TransferFunc& funcA);

}  // namespace tiny_skia::filter
