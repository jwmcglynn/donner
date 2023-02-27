#pragma once

#include <ostream>

#include "src/base/math_utils.h"
#include "src/base/vector2.h"

namespace donner {

/**
 * A 2D axis-aligned bounding box.
 *
 * For example, to construct a box, add points to it, and query its size:
 * ```
 * Boxd box(Vector2d(-1.0, -1.0), Vector2d(1.0, 1.0));
 * box.addPoint(Vector2d(2.0, 0.0));
 *
 * assert(box == Boxd(Vector2d(-1.0, -1.0), Vector2d(2.0, 1.0)));
 * assert(box.size() == Vector2d(3.0, 2.0));
 * ```
 *
 * @tparam T Element type, e.g. double, float, int, etc.
 */
template <typename T>
struct Box {
  /// The top-left corner of the box.
  Vector2<T> topLeft;
  /// The bottom-right corner of the box.
  Vector2<T> bottomRight;

  /**
   * Construct a new box with the given top-left and bottom-right corners.
   *
   * @param topLeft Top-left corner.
   * @param bottomRight Bottom-right corner.
   */
  Box(const Vector2<T>& topLeft, const Vector2<T>& bottomRight)
      : topLeft(topLeft), bottomRight(bottomRight) {}

  Box(const Box<T>& other) = default;
  Box<T>& operator=(const Box<T>& other) = default;

  /**
   * Create an empty box that is centered on the given point.
   *
   * @param point Center point.
   */
  static Box<T> CreateEmpty(const Vector2<T>& point) { return Box<T>(point, point); }

  /**
   * Create a box with the given size, with the top-left corner at the origin.
   *
   * @param size Size of the box.
   */
  static Box<T> WithSize(const Vector2<T>& size) { return Box<T>(Vector2<T>(), size); }

  /**
   * Expand to include the provided point.
   *
   * @param point Point to include.
   */
  void addPoint(const Vector2<T>& point) {
    topLeft.x = std::min(point.x, topLeft.x);
    topLeft.y = std::min(point.y, topLeft.y);
    bottomRight.x = std::max(point.x, bottomRight.x);
    bottomRight.y = std::max(point.y, bottomRight.y);
  }

  /**
   * Adds a bounding box inside this bounding box.
   *
   * @param box Box to add.
   */
  void addBox(const Box<T>& box) {
    addPoint(box.topLeft);
    addPoint(box.bottomRight);
  }

  /// Returns the box width.
  T width() const { return bottomRight.x - topLeft.x; }
  /// Returns the box height.
  T height() const { return bottomRight.y - topLeft.y; }

  /// Returns the box size.
  Vector2<T> size() const { return Vector2<T>(width(), height()); }

  /// Returns true if the box has zero width or height.
  bool isEmpty() const { return NearZero(width()) || NearZero(height()); }

  Box<T> operator-(const Vector2<T>& vec) const { return Box<T>(topLeft - vec, bottomRight - vec); }

  Box<T>& operator-=(const Vector2<T>& vec) {
    topLeft -= vec;
    bottomRight -= vec;
    return *this;
  }

  Box<T> operator+(const Vector2<T>& vec) const { return Box<T>(topLeft + vec, bottomRight + vec); }

  Box<T>& operator+=(const Vector2<T>& vec) {
    topLeft += vec;
    bottomRight += vec;
    return *this;
  }

  // Comparison.
  bool operator==(const Box<T>& other) const {
    return topLeft == other.topLeft && bottomRight == other.bottomRight;
  }
  bool operator!=(const Box<T>& rhs) const { return !operator==(rhs); }

  // Output.
  friend std::ostream& operator<<(std::ostream& os, const Box<T>& box) {
    os << box.topLeft << " => " << box.bottomRight;
    return os;
  }
};

typedef Box<double> Boxd;

}  // namespace donner
