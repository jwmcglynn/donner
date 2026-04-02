#pragma once

namespace tiny_skia {

struct Point;

enum class SearchAxis {
  X,
  Y,
};

struct Point64 {
  double x = 0.0;
  double y = 0.0;

  static constexpr Point64 fromXY(double x, double y) { return Point64{x, y}; }

  static Point64 fromPoint(const Point& point);

  static constexpr Point64 zero() { return Point64{}; }

  Point toPoint() const;

  [[nodiscard]] double axisCoord(SearchAxis axis) const;
};

}  // namespace tiny_skia
