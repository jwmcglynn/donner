#include "tiny_skia/shaders/RadialGradient.h"

#include <cmath>

#include "tiny_skia/Math.h"

namespace tiny_skia {

namespace {

Transform fromPoly2(Point p0, Point p1) {
  return Transform::fromRow(p1.y - p0.y, p0.x - p1.x, p1.x - p0.x, p1.y - p0.y, p0.x, p0.y);
}

std::optional<Transform> tsFromPolyToPoly(Point src1, Point src2, Point dst1, Point dst2) {
  const auto tmp = fromPoly2(src1, src2);
  const auto res = tmp.invert();
  if (!res.has_value()) return std::nullopt;
  const auto tmp2 = fromPoly2(dst1, dst2);
  return tmp2.preConcat(*res);
}

std::optional<Transform> mapToUnitX(Point origin, Point xIsOne) {
  return tsFromPolyToPoly(origin, xIsOne, Point::fromXY(0.0f, 0.0f), Point::fromXY(1.0f, 0.0f));
}

}  // namespace

bool FocalData::isFocalOnCircle() const { return isNearlyEqual(1.0f, r1); }

bool FocalData::isWellBehaved() const { return !isFocalOnCircle() && r1 > 1.0f; }

bool FocalData::isNativelyFocal() const { return isNearlyZero(focalX); }

bool FocalData::set(float r0_in, float r1_in, Transform& matrix) {
  isSwapped = false;
  focalX = r0_in / (r0_in - r1_in);

  if (isNearlyEqual(focalX, 1.0f)) {
    matrix = matrix.postTranslate(-1.0f, 0.0f).postScale(-1.0f, 1.0f);
    std::swap(r0_in, r1_in);
    focalX = 0.0f;
    isSwapped = true;
  }

  const auto focalMatrix = tsFromPolyToPoly(Point::fromXY(focalX, 0.0f), Point::fromXY(1.0f, 0.0f),
                                            Point::fromXY(0.0f, 0.0f), Point::fromXY(1.0f, 0.0f));
  if (!focalMatrix.has_value()) return false;

  matrix = matrix.postConcat(*focalMatrix);
  r1 = r1_in / std::abs(1.0f - focalX);

  if (isFocalOnCircle()) {
    matrix = matrix.postScale(0.5f, 0.5f);
  } else {
    matrix = matrix.postScale(r1 / (r1 * r1 - 1.0f), 1.0f / std::sqrt(std::abs(r1 * r1 - 1.0f)));
  }

  matrix = matrix.postScale(std::abs(1.0f - focalX), std::abs(1.0f - focalX));
  return true;
}

std::optional<std::variant<Color, RadialGradient>> RadialGradient::createRadialUnchecked(
    Point center, float radius, std::vector<GradientStop> stops, SpreadMode mode,
    Transform transform) {
  const float inv = (radius != 0.0f) ? tiny_skia::invert(radius) : 0.0f;
  if (!std::isfinite(inv)) return std::nullopt;
  const auto pointsToUnit = Transform::fromTranslate(-center.x, -center.y).postScale(inv, inv);

  RadialGradient rg{Gradient(std::move(stops), mode, transform, pointsToUnit)};
  rg.gradientType_ = RadialType{0.0f, radius};
  return std::variant<Color, RadialGradient>{std::move(rg)};
}

std::optional<std::variant<Color, RadialGradient>> RadialGradient::createTwoPoint(
    Point c0, float r0, Point c1, float r1, std::vector<GradientStop> stops, SpreadMode mode,
    Transform transform) {
  GradientType gradientType;
  Transform gradientMatrix;

  if ((c0 - c1).length() < kDegenerateThreshold) {
    // Concentric
    const float maxR = std::max(r0, r1);
    if (isNearlyZero(maxR) || isNearlyEqual(r0, r1)) {
      return std::nullopt;
    }
    const float scale = 1.0f / maxR;
    gradientMatrix = Transform::fromTranslate(-c1.x, -c1.y).postScale(scale, scale);
    gradientType = RadialType{r0, r1};
  } else {
    // Different centers
    const auto unitXTs = mapToUnitX(c0, c1);
    if (!unitXTs.has_value()) return std::nullopt;
    gradientMatrix = *unitXTs;

    const float dCenter = (c0 - c1).length();

    if (isNearlyZeroWithinTolerance(r0 - r1, kDegenerateThreshold)) {
      // Strip gradient (equal radii)
      const float scaledR0 = r0 / dCenter;
      gradientType = StripType{scaledR0};
    } else {
      // Focal gradient
      FocalData fd{};
      if (!fd.set(r0 / dCenter, r1 / dCenter, gradientMatrix)) {
        return std::nullopt;
      }
      gradientType = fd;
    }
  }

  RadialGradient rg{Gradient(std::move(stops), mode, transform, gradientMatrix)};
  rg.gradientType_ = std::move(gradientType);
  return std::variant<Color, RadialGradient>{std::move(rg)};
}

std::optional<std::variant<Color, RadialGradient>> RadialGradient::create(
    Point startPoint, float startRadius, Point endPoint, float endRadius,
    std::vector<GradientStop> stops, SpreadMode mode, Transform transform) {
  if (startRadius < 0.0f || endRadius < 0.0f) return std::nullopt;

  if (stops.empty()) return std::nullopt;

  if (stops.size() == 1) {
    return std::variant<Color, RadialGradient>{stops[0].color};
  }

  if (!transform.invert().has_value()) return std::nullopt;

  const float length = (startPoint - endPoint).length();
  if (!std::isfinite(length)) return std::nullopt;

  if (isNearlyZeroWithinTolerance(length, kDegenerateThreshold)) {
    if (isNearlyEqualWithinTolerance(startRadius, endRadius, kDegenerateThreshold)) {
      // Both center and radii are the same - fully degenerate.
      if (mode == SpreadMode::Pad && endRadius > kDegenerateThreshold) {
        const auto startColor = stops.front().color;
        const auto endColor = stops.back().color;
        std::vector<GradientStop> newStops;
        newStops.push_back(GradientStop::create(0.0f, startColor));
        newStops.push_back(GradientStop::create(1.0f, startColor));
        newStops.push_back(GradientStop::create(1.0f, endColor));
        return createRadialUnchecked(startPoint, endRadius, std::move(newStops), mode, transform);
      }
      return std::nullopt;
    }

    // Same center, different radii.
    if (isNearlyZeroWithinTolerance(startRadius, kDegenerateThreshold)) {
      return createRadialUnchecked(startPoint, endRadius, std::move(stops), mode, transform);
    }
  }

  return createTwoPoint(startPoint, startRadius, endPoint, endRadius, std::move(stops), mode,
                        transform);
}

bool RadialGradient::pushStages(ColorSpace cs, pipeline::RasterPipelineBuilder& p) const {
  using Stage = pipeline::Stage;

  // Try fused fast path for simple radial gradient (same center, startRadius=0, 2-stop, Pad).
  if (const auto* rt = std::get_if<RadialType>(&gradientType_)) {
    if (rt->radius1 == 0.0f) {
      // Simple radial: no ApplyConcentricScaleBias needed (p0=1, p1=0).
      if (base_.tryPushFusedRadial2Stop(p, cs)) {
        return true;
      }
    }
  }

  float p0 = 0.0f, p1 = 0.0f;
  if (const auto* rt = std::get_if<RadialType>(&gradientType_)) {
    if (rt->radius1 == 0.0f) {
      p0 = 1.0f;
      p1 = 0.0f;
    } else {
      const float dRadius = rt->radius2 - rt->radius1;
      p0 = std::max(rt->radius1, rt->radius2) / dRadius;
      p1 = -rt->radius1 / dRadius;
    }
  } else if (const auto* st = std::get_if<StripType>(&gradientType_)) {
    p0 = st->scaledR0 * st->scaledR0;
    p1 = 0.0f;
  } else if (const auto* fd = std::get_if<FocalData>(&gradientType_)) {
    p0 = 1.0f / fd->r1;
    p1 = fd->focalX;
  }

  p.ctx().twoPointConicalGradient =
      pipeline::TwoPointConicalGradientCtx{.mask = {}, .p0 = p0, .p1 = p1};

  const auto& gt = gradientType_;
  return base_.pushStages(
      p, cs,
      [&gt, p0, p1](pipeline::RasterPipelineBuilder& b) {
        if (std::holds_alternative<RadialType>(gt)) {
          b.push(Stage::XYToRadius);
          if (p0 != 1.0f || p1 != 0.0f) {
            b.push(Stage::ApplyConcentricScaleBias);
          }
        } else if (std::holds_alternative<StripType>(gt)) {
          b.push(Stage::XYTo2PtConicalStrip);
          b.push(Stage::Mask2PtConicalNan);
        } else if (const auto* fd = std::get_if<FocalData>(&gt)) {
          if (fd->isFocalOnCircle()) {
            b.push(Stage::XYTo2PtConicalFocalOnCircle);
          } else if (fd->isWellBehaved()) {
            b.push(Stage::XYTo2PtConicalWellBehaved);
          } else if (fd->isSwapped || (1.0f - fd->focalX) < 0.0f) {
            b.push(Stage::XYTo2PtConicalSmaller);
          } else {
            b.push(Stage::XYTo2PtConicalGreater);
          }

          if (!fd->isWellBehaved()) {
            b.push(Stage::Mask2PtConicalDegenerates);
          }

          if ((1.0f - fd->focalX) < 0.0f) {
            b.push(Stage::NegateX);
          }

          if (!fd->isNativelyFocal()) {
            b.push(Stage::Alter2PtConicalCompensateFocal);
          }

          if (fd->isSwapped) {
            b.push(Stage::Alter2PtConicalUnswap);
          }
        }
      },
      [&gt](pipeline::RasterPipelineBuilder& b) {
        if (std::holds_alternative<StripType>(gt)) {
          b.push(Stage::ApplyVectorMask);
        } else if (const auto* fd = std::get_if<FocalData>(&gt)) {
          if (!fd->isWellBehaved()) {
            b.push(Stage::ApplyVectorMask);
          }
        }
      });
}

}  // namespace tiny_skia
