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
 * Box2d box(Vector2d(-1.0, -1.0), Vector2d(1.0, 1.0));
 * box.addPoint(Vector2d(2.0, 0.0));
 *
 * assert(box == Box2d(Vector2d(-1.0, -1.0), Vector2d(2.0, 1.0)));
 * assert(box.size() == Vector2d(3.0, 2.0));
 * ```
 *
 * @tparam T Element type, e.g. double, float, int, etc.
 */
template <typename T>
struct Box2 {
  /// The top-left corner of the box.
  Vector2<T> topLeft;
  /// The bottom-right corner of the box.
  Vector2<T> bottomRight;

  /// Default constructor: Creates an empty box centered on (0, 0).
  Box2() : topLeft(Vector2<T>()), bottomRight(Vector2<T>()) {}

  /**
   * Construct a new box with the given top-left and bottom-right corners.
   *
   * @param topLeft Top-left corner.
   * @param bottomRight Bottom-right corner.
   */
  Box2(const Vector2<T>& topLeft, const Vector2<T>& bottomRight)
      : topLeft(topLeft), bottomRight(bottomRight) {}

  /// Destructor.
  ~Box2() = default;

  /**
   * Creates a Box2 from x, y, width, and height.
   *
   * @param x X coordinate of the top-left corner.
   * @param y Y coordinate of the top-left corner.
   * @param width Width of the box.
   * @param height Height of the box.
   */
  static Box2<T> FromXYWH(T x, T y, T width, T height) {
    return Box2<T>(Vector2<T>(x, y), Vector2<T>(x + width, y + height));
  }

  /**
   * Copy constructor.
   *
   * @param other Other box.
   */
  Box2(const Box2<T>& other) = default;

  /**
   * Copy assignment operator.
   *
   * @param other Other box.
   */
  Box2<T>& operator=(const Box2<T>& other) = default;

  /**
   * Move constructor.
   *
   * @param other Other box.
   */
  Box2(Box2<T>&& other) noexcept = default;

  /**
   * Move assignment operator.
   *
   * @param other Other box.
   */
  Box2<T>& operator=(Box2<T>&& other) noexcept = default;

  /**
   * Create an empty box that is centered on the given point.
   *
   * @param point Center point.
   */
  static Box2<T> CreateEmpty(const Vector2<T>& point) { return Box2<T>(point, point); }

  /**
   * Create a box with the given size, with the top-left corner at the origin.
   *
   * @param size Size of the box.
   */
  static Box2<T> WithSize(const Vector2<T>& size) { return Box2<T>(Vector2<T>(), size); }

  /**
   * Create a new box that is expanded to include both boxes.
   *
   * @param a First box.
   * @param b Second box.
   */
  static Box2<T> Union(const Box2<T>& a, const Box2<T>& b) {
    Box2<T> result = a;
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
  void addBox(const Box2<T>& box) {
    addPoint(box.topLeft);
    addPoint(box.bottomRight);
  }

  /**
   * Return a box with the same size but moved to the origin, i.e. with the top-left corner at (0,
   * 0).
   */
  Box2 toOrigin() const { return Box2(Vector2<T>(), size()); }

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
  Box2<T> inflatedBy(T amount) const {
    return Box2<T>(topLeft - Vector2<T>(amount, amount), bottomRight + Vector2<T>(amount, amount));
  }

  /// Return the box moved by subtracting the given vector.
  Box2<T> operator-(const Vector2<T>& vec) const { return Box2<T>(topLeft - vec, bottomRight - vec); }

  /// Move the box by subtracting the given vector.
  Box2<T>& operator-=(const Vector2<T>& vec) {
    topLeft -= vec;
    bottomRight -= vec;
    return *this;
  }

  /// Return the box moved by adding the given vector.
  Box2<T> operator+(const Vector2<T>& vec) const { return Box2<T>(topLeft + vec, bottomRight + vec); }

  /// Move the box by adding the given vector.
  Box2<T>& operator+=(const Vector2<T>& vec) {
    topLeft += vec;
    bottomRight += vec;
    return *this;
  }

  /// @name Comparison
  /// @{

  /// Equality operator.
  bool operator==(const Box2<T>& other) const {
    return topLeft == other.topLeft && bottomRight == other.bottomRight;
  }

  /// Inequality operator.
  bool operator!=(const Box2<T>& rhs) const { return !operator==(rhs); }
  /// @}

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const Box2<T>& box) {
    os << box.topLeft << " => " << box.bottomRight;
    return os;
  }
};

/// @name Typedefs
/// @{

/// Shorthand for \ref Box2<double>.
using Box2d = Box2<double>;

/// @}

/// @name Compatibility aliases
/// @{

template <typename T>
using Box = Box2<T>;
using Boxd = Box2d;

/// @}

}  // namespace donner
