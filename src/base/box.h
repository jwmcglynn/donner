#pragma once

#include <ostream>

#include "src/base/math_utils.h"
#include "src/base/vector2.h"

namespace donner {

template <typename T>
struct Box {
  Vector2<T> top_left;
  Vector2<T> bottom_right;

  Box(const Vector2<T>& top_left, const Vector2<T>& bottom_right)
      : top_left(top_left), bottom_right(bottom_right) {}
  Box(const Box<T>& other) = default;
  Box<T>& operator=(const Box<T>& other) = default;

  /**
   * Create an empty box with the given point.
   */
  static Box<T> CreateEmpty(const Vector2<T>& point) { return Box<T>(point, point); }

  /**
   * Expand to include the provided point.
   */
  void addPoint(const Vector2<T>& point) {
    top_left.x = std::min(point.x, top_left.x);
    top_left.y = std::min(point.y, top_left.y);
    bottom_right.x = std::max(point.x, bottom_right.x);
    bottom_right.y = std::max(point.y, bottom_right.y);
  }

  /**
   * Adds a bounding box inside this bounding box.
   */
  void addBox(const Box<T>& box) {
    addPoint(box.top_left);
    addPoint(box.bottom_right);
  }

  T width() const { return bottom_right.x - top_left.x; }
  T height() const { return bottom_right.y - top_left.y; }

  Vector2<T> size() const { return Vector2<T>(width(), height()); }

  Box<T> operator-(const Vector2<T>& vec) const {
    return Box<T>(top_left - vec, bottom_right - vec);
  }

  Box<T>& operator-=(const Vector2<T>& vec) {
    top_left -= vec;
    bottom_right -= vec;
    return *this;
  }

  Box<T> operator+(const Vector2<T>& vec) const {
    return Box<T>(top_left + vec, bottom_right + vec);
  }

  Box<T>& operator+=(const Vector2<T>& vec) {
    top_left += vec;
    bottom_right += vec;
    return *this;
  }

  // Comparison.
  bool operator==(const Box<T>& other) const {
    return top_left == other.top_left && bottom_right == other.bottom_right;
  }
  bool operator!=(const Box<T>& rhs) const { return !operator==(rhs); }

  // Output.
  friend std::ostream& operator<<(std::ostream& os, const Box<T>& box) {
    os << box.top_left << " => " << box.bottom_right;
    return os;
  }
};

typedef Box<double> Boxd;

}  // namespace donner
