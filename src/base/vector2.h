#pragma once
/// @file

#include <ostream>

#include "src/base/math_utils.h"

namespace donner {

/**
 * A 2D vector, (x, y).
 *
 * @tparam T The type of the vector's components.
 */
template <typename T>
struct Vector2 {
  T x = T(0);  ///< The x component of the vector.
  T y = T(0);  ///< The y component of the vector.

  /// Returns a vector with all components set to zero.
  static Vector2<T> Zero() { return Vector2(T(0), T(0)); }

  /// Returns a vector with for the x-axis, i.e. (1, 0).
  static Vector2<T> XAxis() { return Vector2(T(1), T(0)); }

  /// Returns a vector with for the y-axis, i.e. (0, 1).
  static Vector2<T> YAxis() { return Vector2(T(0), T(1)); }

  /// Default constructor for a zero vector.
  Vector2() = default;

  /// Copy constructor.
  Vector2(const Vector2<T>& other) = default;

  /// Construct a vector from a given x and y component.
  constexpr Vector2(T x, T y) : x(x), y(y) {}

  /**
   * Construct a vector from a vector of a different type, by implicitly casting each component.
   *
   * @tparam S The type of the other vector's components.
   * @param other The other vector.
   */
  template <typename S>
  Vector2(const Vector2<S>& other) : x(other.x), y(other.y) {}

  /**
   * Construct a vector from components of a different type, by implicitly casting each component.
   *
   * @tparam S The type of the components.
   * @param x The x component.
   * @param y The y component.
   */
  template <typename S>
  Vector2(S x, S y) : x(x), y(y) {}

  /// Returns the length of the vector.
  UTILS_NO_DISCARD T length() const { return static_cast<T>(sqrt(double(x * x + y * y))); }

  /// Returns the squared length of the vector.
  UTILS_NO_DISCARD T lengthSquared() const { return x * x + y * y; }

  /**
   * Returns the distance between two vectors, assuming that each represents a point in space.
   *
   * @param other The other vector.
   */
  UTILS_NO_DISCARD T distance(const Vector2<T>& other) const { return (other - *this).length(); }

  /**
   * Returns the squared distance between two vectors, assuming that each represents a point in
   * space.
   *
   * @param other The other vector.
   */
  UTILS_NO_DISCARD T distanceSquared(const Vector2<T>& other) const {
    return (other - *this).lengthSquared();
  }

  /**
   * Returns the dot product of this vector and \p other
   *
   * @param other The other vector.
   */
  UTILS_NO_DISCARD T dot(const Vector2<T>& other) const { return x * other.x + y * other.y; }

  /**
   * Rotate this vector by \p radians
   *
   * @param radians Angle in radians.
   */
  UTILS_NO_DISCARD Vector2<T> rotate(double radians) const {
    return rotate(std::cos(radians), std::sin(radians));
  }

  /**
   * Rotate this vector by a given pre-computed cosine/sine angle.
   *
   * @param cosResult The result of `cos(angle)`
   * @param sinResult The result of `sin(angle)`
   */
  UTILS_NO_DISCARD Vector2<T> rotate(T cosResult, T sinResult) const {
    return Vector2<T>(x * cosResult - y * sinResult, x * sinResult + y * cosResult);
  }

  /**
   * Returns the angle that this vector makes with the +x axis, in radians.
   *
   * @return Angle in the range of [-π, π].
   */
  UTILS_NO_DISCARD T angle() const { return static_cast<T>(atan2(double(y), double(x))); }

  /**
   * Returns the angle between this vector and the provided vector, or zero if one of the vectors
   * has zero length.
   *
   * @param other Other vector.
   * @return Angle in the range of [0, π].
   */
  UTILS_NO_DISCARD T angleWith(const Vector2<T>& other) const {
    const T magProduct = length() * other.length();
    if (NearZero(magProduct)) {
      return T(0);
    }

    const T cosTheta = dot(other) / magProduct;
    return std::acos(cosTheta);
  }

  /**
   * Returns the normalized form of this vector.
   */
  UTILS_NO_DISCARD Vector2<T> normalize() const {
    const T len = length();

    if (NearZero(len)) {
      return Vector2<T>::Zero();
    } else {
      const T mag = T(1) / len;
      return Vector2<T>(x * mag, y * mag);
    }
  }

  /// @addtogroup Operators
  /// @{

  /**
   * Assignment operator.
   *
   * @param rhs The vector to copy.
   */
  Vector2<T>& operator=(const Vector2<T>& rhs) {
    x = rhs.x;
    y = rhs.y;
    return *this;
  }

  /**
   * Assignment operator from a vector of a different type.
   *
   * @tparam S The type of the other vector's components.
   * @param rhs The vector to copy.
   */
  template <typename S>
  Vector2<T>& operator=(const Vector2<S>& rhs) {
    x = T(rhs.x);
    y = T(rhs.y);
    return *this;
  }

  /// Unary negation.
  Vector2<T> operator-() const { return Vector2<T>(-x, -y); }

  /**
   * Addition operator, add two vectors and return the result.
   *
   * @param rhs The vector to add.
   */
  Vector2<T> operator+(const Vector2<T>& rhs) const { return Vector2<T>(x + rhs.x, y + rhs.y); }

  /**
   * Addition assignment operator, add \p rhs to this vector.
   *
   * @param rhs The vector to add.
   */
  Vector2<T>& operator+=(const Vector2<T>& rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
  }

  /**
   * Subtraction operator, subtract two vectors and return the result.
   *
   * @param rhs The vector to subtract.
   */
  Vector2<T> operator-(const Vector2<T>& rhs) const { return Vector2<T>(x - rhs.x, y - rhs.y); }

  /**
   * Subtraction assignment operator, subtract \p rhs from this vector.
   *
   * @param rhs The vector to subtract.
   */
  Vector2<T>& operator-=(const Vector2<T>& rhs) {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
  }

  /**
   * Piecewise multiplication operator, multiply the x and y components of two vectors and return
   * the result.
   *
   * Equivalent to `Vector2<T>(lhs.x * rhs.x, lhs.y * rhs.y)`.
   *
   * @param rhs The vector to multiply.
   */
  Vector2<T> operator*(const Vector2<T>& rhs) const { return Vector2<T>(x * rhs.x, y * rhs.y); }

  /**
   * Piecewise multiplication assignment operator, multiply the x and y components of this vector
   * by the x and y components of \p rhs.
   *
   * Equivalent to `lhs.x *= rhs.x; lhs.y *= rhs.y;`.
   *
   * @param rhs The vector to multiply.
   */
  Vector2<T>& operator*=(const Vector2<T>& rhs) {
    x *= rhs.x;
    y *= rhs.y;
    return *this;
  }

  /**
   * Scalar multiplication operator, multiply the x and y components of this vector by \p a and
   * return the result.
   *
   * Equivalent to `Vector2<T>(lhs.x * a, lhs.y * a)`.
   *
   * @param a The scalar to multiply.
   */
  Vector2<T> operator*(const T a) const { return Vector2<T>(x * a, y * a); }

  /**
   * Scalar multiplication assignment operator, multiply the x and y components of this vector by
   * \p a.
   *
   * Equivalent to `lhs.x *= a; lhs.y *= a;`.
   *
   * @param a The scalar to multiply.
   */
  Vector2<T>& operator*=(const T a) {
    x *= a;
    y *= a;
    return *this;
  }

  /**
   * Reversed scalar multiplication operator, multiply the x and y components of this vector by \p a
   * and return the result.
   *
   * Equivalent to `Vector2<T>(a * lhs.x, a * lhs.y)`.
   *
   * @param a The scalar to multiply.
   * @param other The vector to multiply.
   */
  friend Vector2<T> operator*(const T a, const Vector2<T>& other) {
    return Vector2<T>(a * other.x, a * other.y);
  }

  /**
   * Piecewise division operator, divide the x and y components of two vectors and return the
   * result.
   *
   * Equivalent to `Vector2<T>(lhs.x / rhs.x, lhs.y / rhs.y)`.
   *
   * @param rhs The vector to divide.
   */
  Vector2<T> operator/(const Vector2<T>& rhs) const { return Vector2<T>(x / rhs.x, y / rhs.y); }

  /**
   * Piecewise division assignment operator, divide the x and y components of this vector by the x
   * and y components of \p rhs.
   *
   * Equivalent to `lhs.x /= rhs.x; lhs.y /= rhs.y;`.
   *
   * @param rhs The vector to divide.
   */
  Vector2<T>& operator/=(const Vector2<T>& rhs) {
    x /= rhs.x;
    y /= rhs.y;
    return *this;
  }

  /**
   * Scalar division operator, divide the x and y components of this vector by \p a and return the
   * result.
   *
   * Equivalent to `Vector2<T>(lhs.x / a, lhs.y / a)`.
   *
   * @param a The scalar to divide.
   */
  Vector2<T> operator/(const T a) const { return Vector2<T>(x / a, y / a); }

  /**
   * Scalar division assignment operator, divide the x and y components of this vector by \p a.
   *
   * Equivalent to `lhs.x /= a; lhs.y /= a;`.
   *
   * @param a The scalar to divide.
   */
  Vector2<T>& operator/=(const T a) {
    x /= a;
    y /= a;
    return *this;
  }

  /**
   * Reversed scalar division operator, divide the x and y components of this vector by \p a and
   * return the result.
   *
   * Equivalent to `Vector2<T>(a / lhs.x, a / lhs.y)`.
   *
   * @param a The scalar to divide.
   * @param other The vector to divide.
   */
  friend Vector2<T> operator/(const T a, const Vector2<T>& other) {
    return Vector2<T>(a / other.x, a / other.y);
  }

  /**
   * Equality operator, check if two vectors are equal.
   *
   * @param other The vector to compare.
   */
  bool operator==(const Vector2<T>& other) const {
    return NearEquals(x, other.x) && NearEquals(y, other.y);
  }

  /**
   * Inequality operator, check if two vectors are not equal.
   *
   * @param rhs The vector to compare.
   */
  bool operator!=(const Vector2<T>& rhs) const { return !operator==(rhs); }

  /**
   * Equality operator with a vector of a different type, check if two vectors are equal.
   *
   * @param other The vector to compare.
   */
  template <typename U>
  bool operator==(const Vector2<U>& other) const {
    return NearEquals(x, static_cast<T>(other.x)) && NearEquals(y, static_cast<T>(other.y));
  }

  /**
   * Inequality operator with a vector of a different type, check if two vectors are not equal.
   *
   * @param rhs The vector to compare.
   */
  template <typename U>
  bool operator!=(const Vector2<U>& rhs) const {
    return !operator==(rhs);
  }

  /// @}

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const Vector2<T>& vec) {
    os << "(" << vec.x << ", " << vec.y << ")";
    return os;
  }
};

/// @addtogroup Typedefs
/// @{

/// Shorthand for \ref Vector2<float>.
typedef Vector2<float> Vector2f;

/// Shorthand for \ref Vector2<double>.
typedef Vector2<double> Vector2d;

/// Shorthand for \ref Vector2<int>.
typedef Vector2<int> Vector2i;

/// @}

}  // namespace donner
