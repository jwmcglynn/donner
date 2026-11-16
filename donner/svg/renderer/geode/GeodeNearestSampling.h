#pragma once
/// @file
/// Single-precision mapping from target pixels to source texels for nearest image sampling.

#include <array>
#include <optional>

#include "donner/base/Transform.h"

namespace donner::geode {

/**
 * Affine rows that map a target pixel position (x, y) to source texel coordinates:
 * `texelX = x * texelX[0] + (y * texelX[1] + texelX[2])`, and likewise for `texelY`.
 */
struct NearestTexelRows {
  std::array<float, 3> texelX{};  //!< x, y and constant terms of the texel x coordinate.
  std::array<float, 3> texelY{};  //!< x, y and constant terms of the texel y coordinate.
};

/**
 * Inverts \p targetFromTexel the way the software renderer's image sampler does: the transform is
 * first narrowed to single precision, then inverted with that sampler's algorithm (a single
 * precision reciprocal for scale and translation, otherwise a double precision determinant with
 * each coefficient rounded once). Sampling the same pixel centers through these rows then selects
 * the same texels as the software renderer wherever a texel boundary passes through a pixel
 * center, up to the floating-point contraction of the final multiply-add.
 *
 * @param targetFromTexel Transform from source texel coordinates to target pixels.
 * @return The rows, or `std::nullopt` when the narrowed transform is not invertible.
 */
std::optional<NearestTexelRows> NearestTexelRowsFor(const Transform2d& targetFromTexel);

}  // namespace donner::geode
