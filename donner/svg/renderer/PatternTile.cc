#include "donner/svg/renderer/PatternTile.h"

#include <algorithm>
#include <cmath>

namespace donner::svg {
namespace {

bool IsFinite(const Transform2d& transform) {
  for (double value : transform.data) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<PatternTileRasterMetrics> ComputePatternTileRasterMetrics(
    const Box2d& tileRect, const Transform2d& deviceFromPattern) {
  const double tileWidth = tileRect.width();
  const double tileHeight = tileRect.height();
  const double determinant = deviceFromPattern.determinant();
  if (!(tileWidth > 0.0) || !(tileHeight > 0.0) || !std::isfinite(tileWidth) ||
      !std::isfinite(tileHeight) || !IsFinite(deviceFromPattern) || !std::isfinite(determinant) ||
      determinant == 0.0 || !std::isfinite(1.0 / determinant)) {
    return std::nullopt;
  }

  const double scaleX =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(1.0, 0.0)).length());
  const double scaleY =
      std::max(1.0, deviceFromPattern.transformVector(Vector2d(0.0, 1.0)).length());
  constexpr double kPatternSupersampleScale = 2.0;
  const Vector2d requestedRasterFromPatternScale(scaleX * kPatternSupersampleScale,
                                                 scaleY * kPatternSupersampleScale);
  const double requestedPixelWidth = tileWidth * requestedRasterFromPatternScale.x;
  const double requestedPixelHeight = tileHeight * requestedRasterFromPatternScale.y;
  if (!(requestedPixelWidth > 0.0) || !(requestedPixelHeight > 0.0) ||
      !std::isfinite(requestedPixelWidth) || !std::isfinite(requestedPixelHeight) ||
      requestedPixelWidth > kMaxPatternTileRasterDimension ||
      requestedPixelHeight > kMaxPatternTileRasterDimension) {
    return std::nullopt;
  }

  const int pixelWidth = std::max(1, static_cast<int>(std::ceil(requestedPixelWidth)));
  const int pixelHeight = std::max(1, static_cast<int>(std::ceil(requestedPixelHeight)));
  return PatternTileRasterMetrics{
      .pixelWidth = pixelWidth,
      .pixelHeight = pixelHeight,
      .rasterFromPatternScale = Vector2d(static_cast<double>(pixelWidth) / tileWidth,
                                         static_cast<double>(pixelHeight) / tileHeight),
  };
}

Transform2d TargetFromPatternRaster(const Transform2d& targetFromPattern,
                                    const Vector2d& rasterFromPatternScale) {
  return Transform2d::Scale(1.0 / rasterFromPatternScale.x, 1.0 / rasterFromPatternScale.y) *
         targetFromPattern;
}

}  // namespace donner::svg
