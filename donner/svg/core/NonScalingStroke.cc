#include "donner/svg/core/NonScalingStroke.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace donner::svg {

namespace {

/// Returns whether every component of \p transform is finite. A non-finite CTM comes from
/// degenerate authored input; the renderer still has to answer every query about it without
/// propagating the non-finite value into geometry or into a cache key.
bool IsFiniteTransform(const Transform2d& transform) {
  for (size_t i = 0; i < 6; ++i) {
    if (!std::isfinite(transform.data[i])) {
      return false;
    }
  }

  return true;
}

}  // namespace

bool IsSimilarityTransform(const Transform2d& transform) {
  // Layout is [a c e; b d f], so (a, b) and (c, d) are the two linear columns.
  const double a = transform.data[0];
  const double b = transform.data[1];
  const double c = transform.data[2];
  const double d = transform.data[3];

  const double scaleSq = a * a + b * b;
  if (!(scaleSq > 0.0) || !std::isfinite(scaleSq)) {
    return false;
  }

  constexpr double kRelativeTolerance = 1e-9;
  const double otherScaleSq = c * c + d * d;
  if (!std::isfinite(otherScaleSq) ||
      std::abs(otherScaleSq - scaleSq) > kRelativeTolerance * scaleSq) {
    return false;
  }

  // Equal-length columns are a similarity exactly when they are orthogonal.
  return std::abs(a * c + b * d) <= kRelativeTolerance * scaleSq;
}

NonScalingStrokeMode ResolveNonScalingStrokeMode(VectorEffect vectorEffect,
                                                 const Transform2d& worldFromEntity,
                                                 bool geometryMustStayLocal) {
  if (vectorEffect != VectorEffect::NonScalingStroke) {
    return NonScalingStrokeMode::Scaling;
  }

  // A non-finite CTM is not a similarity, but it must not select the host-space path either:
  // mapping the centerline through it yields non-finite geometry, and a cache keyed on it can
  // never hit again because a NaN component compares unequal to itself.
  if (geometryMustStayLocal || !IsFiniteTransform(worldFromEntity) ||
      IsSimilarityTransform(worldFromEntity)) {
    return NonScalingStrokeMode::ScaledLocal;
  }

  return NonScalingStrokeMode::HostSpace;
}

double EffectiveStrokeWidth(double authoredWidth, NonScalingStrokeMode mode,
                            const Transform2d& worldFromEntity) {
  if (mode != NonScalingStrokeMode::ScaledLocal) {
    return authoredWidth;
  }

  const double scale = std::sqrt(std::abs(worldFromEntity.determinant()));
  if (!std::isfinite(scale) || !(scale > 0.0)) {
    return authoredWidth;
  }

  return authoredWidth / scale;
}

double StrokeCullHalfExtent(double strokeWidth, StrokeLinecap linecap, StrokeLinejoin linejoin,
                            double miterLimit) {
  const double halfStroke = strokeWidth * 0.5;

  double joinReach = halfStroke;
  switch (linejoin) {
    case StrokeLinejoin::Miter:
    case StrokeLinejoin::MiterClip:
    case StrokeLinejoin::Arcs:
      // An unbounded miter limit means the tip is unbounded too. A cull bound may only
      // over-count, so report an infinite reach and let the caller decline to cull.
      joinReach = std::isfinite(miterLimit) ? halfStroke * std::max(1.0, miterLimit)
                                            : std::numeric_limits<double>::infinity();
      break;
    case StrokeLinejoin::Round:
    case StrokeLinejoin::Bevel: joinReach = halfStroke; break;
  }

  // A square cap is a half-width extension of the segment, so its far corners sit
  // `sqrt(2) * halfStroke` from the endpoint rather than `halfStroke` from the centerline.
  const double capReach =
      linecap == StrokeLinecap::Square ? halfStroke * std::numbers::sqrt2 : halfStroke;

  return std::max(joinReach, capReach);
}

}  // namespace donner::svg
