#include "tiny_skia/Transform.h"

#include <cmath>

#include "tiny_skia/Math.h"
#include "tiny_skia/Point.h"

namespace tiny_skia {

namespace {

double dcross(double a, double b, double c, double d) { return a * b - c * d; }

float dcrossDscale(float a, float b, float c, float d, double scale) {
  return static_cast<float>(dcross(static_cast<double>(a), static_cast<double>(b),
                                   static_cast<double>(c), static_cast<double>(d)) *
                            scale);
}

float mulAddMul(float a, float b, float c, float d) {
  return static_cast<float>(static_cast<double>(a) * static_cast<double>(b) +
                            static_cast<double>(c) * static_cast<double>(d));
}

Transform concat(const Transform& a, const Transform& b) {
  if (a.isIdentity()) return b;
  if (b.isIdentity()) return a;

  if (!a.hasSkew() && !b.hasSkew()) {
    return Transform::fromRow(a.sx * b.sx, 0.0f, 0.0f, a.sy * b.sy, a.sx * b.tx + a.tx,
                              a.sy * b.ty + a.ty);
  }

  return Transform::fromRow(mulAddMul(a.sx, b.sx, a.kx, b.ky), mulAddMul(a.ky, b.sx, a.sy, b.ky),
                            mulAddMul(a.sx, b.kx, a.kx, b.sy), mulAddMul(a.ky, b.kx, a.sy, b.sy),
                            mulAddMul(a.sx, b.tx, a.kx, b.ty) + a.tx,
                            mulAddMul(a.ky, b.tx, a.sy, b.ty) + a.ty);
}

std::optional<double> invDeterminant(const Transform& ts) {
  const auto det = dcross(static_cast<double>(ts.sx), static_cast<double>(ts.sy),
                          static_cast<double>(ts.kx), static_cast<double>(ts.ky));
  const float tolerance = kScalarNearlyZero * kScalarNearlyZero * kScalarNearlyZero;
  if (isNearlyZeroWithinTolerance(static_cast<float>(det), tolerance)) {
    return std::nullopt;
  }
  return 1.0 / det;
}

Transform computeInv(const Transform& ts, double invDet) {
  return Transform::fromRow(static_cast<float>(static_cast<double>(ts.sy) * invDet),
                            static_cast<float>(static_cast<double>(-ts.ky) * invDet),
                            static_cast<float>(static_cast<double>(-ts.kx) * invDet),
                            static_cast<float>(static_cast<double>(ts.sx) * invDet),
                            dcrossDscale(ts.kx, ts.ty, ts.sy, ts.tx, invDet),
                            dcrossDscale(ts.ky, ts.tx, ts.sx, ts.ty, invDet));
}

}  // namespace

bool Transform::isFinite() const {
  return std::isfinite(sx) && std::isfinite(ky) && std::isfinite(kx) && std::isfinite(sy) &&
         std::isfinite(tx) && std::isfinite(ty);
}

bool Transform::isIdentity() const { return *this == Transform::identity(); }

bool Transform::isTranslate() const { return !hasScale() && !hasSkew(); }

bool Transform::isScaleTranslate() const { return !hasSkew(); }

bool Transform::hasScale() const { return sx != 1.0f || sy != 1.0f; }

bool Transform::hasSkew() const { return kx != 0.0f || ky != 0.0f; }

bool Transform::hasTranslate() const { return tx != 0.0f || ty != 0.0f; }

std::optional<Transform> Transform::invert() const {
  if (isIdentity()) return *this;

  if (isScaleTranslate()) {
    if (hasScale()) {
      const auto invX = tiny_skia::invert(sx);
      const auto invY = tiny_skia::invert(sy);
      if (!std::isfinite(invX) || !std::isfinite(invY)) {
        return std::nullopt;
      }
      return fromRow(invX, 0.0f, 0.0f, invY, -tx * invX, -ty * invY);
    }
    return fromTranslate(-tx, -ty);
  }

  const auto invDet = invDeterminant(*this);
  if (!invDet.has_value()) return std::nullopt;

  const auto invTs = computeInv(*this, *invDet);
  if (!invTs.isFinite()) return std::nullopt;

  return invTs;
}

Transform Transform::preConcat(const Transform& other) const { return concat(*this, other); }

Transform Transform::postConcat(const Transform& other) const { return concat(other, *this); }

Transform Transform::preScale(float sx, float sy) const { return preConcat(fromScale(sx, sy)); }

Transform Transform::postScale(float sx, float sy) const { return postConcat(fromScale(sx, sy)); }

Transform Transform::preTranslate(float tx, float ty) const {
  return preConcat(fromTranslate(tx, ty));
}

Transform Transform::postTranslate(float tx, float ty) const {
  return postConcat(fromTranslate(tx, ty));
}

void Transform::mapPoints(std::span<Point> points) const {
  for (auto& p : points) {
    const float nx = sx * p.x + kx * p.y + tx;
    const float ny = ky * p.x + sy * p.y + ty;
    p.x = nx;
    p.y = ny;
  }
}

}  // namespace tiny_skia
