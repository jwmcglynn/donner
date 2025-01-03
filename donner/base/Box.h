#pragma once
/// @file

#include <ostream>

#include "donner/base/MathUtils.h"
#include "donner/base/Vector2.h"

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

  /// Default constructor: Creates an empty box centered on (0, 0).
  Box() : topLeft(Vector2<T>()), bottomRight(Vector2<T>()) {}

  /**
   * Construct a new box with the given top-left and bottom-right corners.
   *
   * @param topLeft Top-left corner.
   * @param bottomRight Bottom-right corner.
   */
  Box(const Vector2<T>& topLeft, const Vector2<T>& bottomRight)
      : topLeft(topLeft), bottomRight(bottomRight) {}

  /// Destructor.
  ~Box() = default;

  /**
   * Creates a Box from x, y, width, and height.
   *
   * @param x X coordinate of the top-left corner.
   * @param y Y coordinate of the top-left corner.
   * @param width Width of the box.
   * @param height Height of the box.
   */
  static Box<T> FromXYWH(T x, T y, T width, T height) {
    return Box<T>(Vector2<T>(x, y), Vector2<T>(x + width, y + height));
  }

  /**
   * Copy constructor.
   *
   * @param other Other box.
   */
  Box(const Box<T>& other) = default;

  /**
   * Copy assignment operator.
   *
   * @param other Other box.
   */
  Box<T>& operator=(const Box<T>& other) = default;

  /**
   * Move constructor.
   *
   * @param other Other box.
   */
  Box(Box<T>&& other) noexcept = default;

  /**
   * Move assignment operator.
   *
   * @param other Other box.
   */
  Box<T>& operator=(Box<T>&& other) noexcept = default;

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
   * Create a new box that is expanded to include both boxes.
   *
   * @param a First box.
   * @param b Second box.
   */
  static Box<T> Union(const Box<T>& a, const Box<T>& b) {
    Box<T> result = a;
    result.addBox(b);
    return result;
  }

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

  /**
   * Return a box with the same size but moved to the origin, i.e. with the top-left corner at (0,
   * 0).
   */
  Box toOrigin() const { return Box(Vector2d(), size()); }

  /// Returns the box width.
  T width() const { return bottomRight.x - topLeft.x; }
  /// Returns the box height.
  T height() const { return bottomRight.y - topLeft.y; }

  /// Returns the box size.
  Vector2<T> size() const { return Vector2<T>(width(), height()); }

  /// Returns true if the box has zero width or height.
  bool isEmpty() const { return NearZero(width()) || NearZero(height()); }

  /**
   * Returns true if the box contains the given point.
   *
   * @param point Point to check.
   */
  bool contains(const Vector2<T>& point) const {
    return point.x >= topLeft.x && point.x <= bottomRight.x && point.y >= topLeft.y &&
           point.y <= bottomRight.y;
  }

  /**
   * Inflates the box size by the given amount in all directions.
   *
   * @param amount Amount to inflate the box by.
   */
  Box<T> inflatedBy(T amount) const {
    return Box<T>(topLeft - Vector2<T>(amount, amount), bottomRight + Vector2<T>(amount, amount));
  }

  /// Return the box moved by subtracting the given vector.
  Box<T> operator-(const Vector2<T>& vec) const { return Box<T>(topLeft - vec, bottomRight - vec); }

  /// Move the box by subtracting the given vector.
  Box<T>& operator-=(const Vector2<T>& vec) {
    topLeft -= vec;
    bottomRight -= vec;
    return *this;
  }

  /// Return the box moved by adding the given vector.
  Box<T> operator+(const Vector2<T>& vec) const { return Box<T>(topLeft + vec, bottomRight + vec); }

  /// Move the box by adding the given vector.
  Box<T>& operator+=(const Vector2<T>& vec) {
    topLeft += vec;
    bottomRight += vec;
    return *this;
  }

  /// @name Comparison
  /// @{

  /// Equality operator.
  bool operator==(const Box<T>& other) const {
    return topLeft == other.topLeft && bottomRight == other.bottomRight;
  }

  /// Inequality operator.
  bool operator!=(const Box<T>& rhs) const { return !operator==(rhs); }
  /// @}

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const Box<T>& box) {
    os << box.topLeft << " => " << box.bottomRight;
    return os;
  }
};

/// Shorthand for \ref Box<double>.
using Boxd = Box<double>;

}  // namespace donner
