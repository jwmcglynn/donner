#include "tiny_skia/path64/Point64.h"

#include "tiny_skia/Point.h"

namespace tiny_skia {

Point64 Point64::fromPoint(const Point& point) {
  return Point64{static_cast<double>(point.x), static_cast<double>(point.y)};
}

Point Point64::toPoint() const { return Point{static_cast<float>(x), static_cast<float>(y)}; }

double Point64::axisCoord(SearchAxis axis) const { return axis == SearchAxis::X ? x : y; }

}  // namespace tiny_skia
