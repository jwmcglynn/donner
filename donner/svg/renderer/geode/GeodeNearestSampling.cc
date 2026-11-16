#include "donner/svg/renderer/geode/GeodeNearestSampling.h"

#include <cmath>

namespace donner::geode {

namespace {

/// A single-precision affine transform: x' = sx * x + kx * y + tx, y' = ky * x + sy * y + ty.
struct FloatAffine {
  float sx = 1.0f;
  float ky = 0.0f;
  float kx = 0.0f;
  float sy = 1.0f;
  float tx = 0.0f;
  float ty = 0.0f;

  bool isFinite() const {
    return std::isfinite(sx) && std::isfinite(ky) && std::isfinite(kx) && std::isfinite(sy) &&
           std::isfinite(tx) && std::isfinite(ty);
  }
};

/// Tolerance below which a determinant counts as singular: (1/4096)^3.
constexpr float kSingularDeterminant = (1.0f / 4096.0f) * (1.0f / 4096.0f) * (1.0f / 4096.0f);

/// The inverse of \p ts, computed with the same operations and roundings as tiny-skia's
/// `Transform::invert`, which the software renderer's image sampler uses.
std::optional<FloatAffine> Invert(const FloatAffine& ts) {
  const bool hasSkew = ts.kx != 0.0f || ts.ky != 0.0f;
  const bool hasScale = ts.sx != 1.0f || ts.sy != 1.0f;
  if (!hasSkew) {
    if (!hasScale && ts.tx == 0.0f && ts.ty == 0.0f) {
      return ts;
    }
    if (!hasScale) {
      return FloatAffine{1.0f, 0.0f, 0.0f, 1.0f, -ts.tx, -ts.ty};
    }
    const float invX = 1.0f / ts.sx;
    const float invY = 1.0f / ts.sy;
    if (!std::isfinite(invX) || !std::isfinite(invY)) {
      return std::nullopt;
    }
    return FloatAffine{invX, 0.0f, 0.0f, invY, -ts.tx * invX, -ts.ty * invY};
  }

  const double det = static_cast<double>(ts.sx) * static_cast<double>(ts.sy) -
                     static_cast<double>(ts.kx) * static_cast<double>(ts.ky);
  if (std::abs(static_cast<float>(det)) <= kSingularDeterminant) {
    return std::nullopt;
  }
  const double invDet = 1.0 / det;
  const auto crossScaled = [invDet](float a, float b, float c, float d) {
    return static_cast<float>((static_cast<double>(a) * static_cast<double>(b) -
                               static_cast<double>(c) * static_cast<double>(d)) *
                              invDet);
  };
  const FloatAffine inverse{static_cast<float>(static_cast<double>(ts.sy) * invDet),
                            static_cast<float>(static_cast<double>(-ts.ky) * invDet),
                            static_cast<float>(static_cast<double>(-ts.kx) * invDet),
                            static_cast<float>(static_cast<double>(ts.sx) * invDet),
                            crossScaled(ts.kx, ts.ty, ts.sy, ts.tx),
                            crossScaled(ts.ky, ts.tx, ts.sx, ts.ty)};
  if (!inverse.isFinite()) {
    return std::nullopt;
  }
  return inverse;
}

}  // namespace

std::optional<NearestTexelRows> NearestTexelRowsFor(const Transform2d& targetFromTexel) {
  // Transform2d stores [a b c d e f] with x' = a*x + c*y + e and y' = b*x + d*y + f.
  const FloatAffine narrowed{
      static_cast<float>(targetFromTexel.data[0]), static_cast<float>(targetFromTexel.data[1]),
      static_cast<float>(targetFromTexel.data[2]), static_cast<float>(targetFromTexel.data[3]),
      static_cast<float>(targetFromTexel.data[4]), static_cast<float>(targetFromTexel.data[5])};
  const std::optional<FloatAffine> texelFromTarget = Invert(narrowed);
  if (!texelFromTarget.has_value()) {
    return std::nullopt;
  }
  NearestTexelRows rows;
  rows.texelX = {texelFromTarget->sx, texelFromTarget->kx, texelFromTarget->tx};
  rows.texelY = {texelFromTarget->ky, texelFromTarget->sy, texelFromTarget->ty};
  return rows;
}

}  // namespace donner::geode
