#pragma once

#include <ostream>

#include "src/base/math_utils.h"

namespace donner {

template <typename T>
struct Vector2 {
  T x = T(0);
  T y = T(1);

  static Vector2<T> Zero() { return Vector2(T(0), T(0)); }
  static Vector2<T> XAxis() { return Vector2(T(1), T(0)); }
  static Vector2<T> YAxis() { return Vector2(T(0), T(1)); }

  Vector2() = default;

  // Standard constructors.
  Vector2(const Vector2<T>& other) = default;
  constexpr Vector2(T x, T y) : x(x), y(y) {}

  // Cast constructors.
  template <typename S>
  Vector2(const Vector2<S>& other) : x(other.x), y(other.y) {}

  template <typename S>
  Vector2(S x, S y) : x(x), y(y) {}

  // Returns the length of the vector.
  UTILS_NO_DISCARD T length() const { return (T)sqrt(double(x * x + y * y)); }
  // Returns the squared length of the vector.
  UTILS_NO_DISCARD T lengthSquared() const { return x * x + y * y; }

  // Returns the distance between two vectors, assuming that each represents a
  // point in space.
  UTILS_NO_DISCARD T distance(const Vector2<T>& other) const { return (other - *this).length(); }
  // Returns the squared distance between two vectors, assuming that each
  // represents a point in space.
  UTILS_NO_DISCARD T distanceSquared(const Vector2<T>& other) const {
    return (other - *this).lengthSquared();
  }

  // Returns the dot product.
  UTILS_NO_DISCARD T dot(const Vector2<T>& other) const { return x * other.x + y * other.y; }

  // Rotate this vector.
  UTILS_NO_DISCARD Vector2<T> rotate(double radians) const {
    return rotate((T)cos(radians), (T)sin(radians));
  }

  // Rotate this vector given a pre-computed cosine/sine angle.
  UTILS_NO_DISCARD Vector2<T> rotate(T cos_result, T sin_result) const {
    return Vector2<T>(x * cos_result - y * sin_result, x * sin_result + y * cos_result);
  }

  // Returns the angle that this vector makes with the +x axis, in radians.
  // Result is returned in the range of [-pi, pi].
  UTILS_NO_DISCARD T angle() const { return (T)atan2(double(y), double(x)); }

  // Returns the angle between this vector and the provided vector, or zero if one of the vectors
  // has zero length.
  //
  // Result is returned in the range of [0, pi].
  UTILS_NO_DISCARD T angleWith(const Vector2<T>& other) const {
    const T magProduct = length() * other.length();
    if (NearZero(magProduct)) {
      return T(0);
    }

    const T cos_theta = dot(other) / magProduct;
    return acos(cos_theta);
  }

  // Returns the normalized vector.
  UTILS_NO_DISCARD Vector2<T> normalize() const {
    const T len = length();

    if (NearZero(len)) {
      return Vector2<T>::Zero();
    } else {
      const T mag = T(1) / len;
      return Vector2<T>(x * mag, y * mag);
    }
  }

  /*************************************************************************/
  // Operators.

  Vector2<T> operator-() const { return Vector2<T>(-x, -y); }
  Vector2<T>& operator=(const Vector2<T>& rhs) {
    x = rhs.x;
    y = rhs.y;
    return *this;
  }

  template <typename S>
  Vector2<T>& operator=(const Vector2<S>& rhs) {
    x = T(rhs.x);
    y = T(rhs.y);
    return *this;
  }

  // Addition.
  Vector2<T> operator+(const Vector2<T>& rhs) const { return Vector2<T>(x + rhs.x, y + rhs.y); }
  Vector2<T>& operator+=(const Vector2<T>& rhs) {
    x += rhs.x;
    y += rhs.y;
    return *this;
  }

  // Subtraction.
  Vector2<T> operator-(const Vector2<T>& rhs) const { return Vector2<T>(x - rhs.x, y - rhs.y); }
  Vector2<T>& operator-=(const Vector2<T>& rhs) {
    x -= rhs.x;
    y -= rhs.y;
    return *this;
  }

  // Piecewise multiplication.
  Vector2<T> operator*(const Vector2<T>& rhs) const { return Vector2<T>(x * rhs.x, y * rhs.y); }
  Vector2<T>& operator*=(const Vector2<T>& rhs) {
    x *= rhs.x;
    y *= rhs.y;
    return *this;
  }

  // Scalar multiplication.
  Vector2<T> operator*(const T a) const { return Vector2<T>(x * a, y * a); }
  Vector2<T>& operator*=(const T a) {
    x *= a;
    y *= a;
    return *this;
  }
  friend Vector2<T> operator*(const T a, const Vector2<T>& other) {
    return Vector2<T>(a * other.x, a * other.y);
  }

  // Piecewise division.
  Vector2<T> operator/(const Vector2<T>& rhs) const { return Vector2<T>(x / rhs.x, y / rhs.y); }
  Vector2<T>& operator/=(const Vector2<T>& rhs) {
    x /= rhs.x;
    y /= rhs.y;
    return *this;
  }

  // Scalar division.
  Vector2<T> operator/(const T a) const { return Vector2<T>(x / a, y / a); }
  Vector2<T>& operator/=(const T a) {
    x /= a;
    y /= a;
    return *this;
  }
  friend Vector2<T> operator/(const T a, const Vector2<T>& other) {
    return Vector2<T>(a / other.x, a / other.y);
  }

  // Comparison.
  bool operator==(const Vector2<T>& other) const {
    return NearEquals(x, other.x) && NearEquals(y, other.y);
  }
  bool operator!=(const Vector2<T>& rhs) const { return !operator==(rhs); }

  // Comparison with a different type.
  template <typename U>
  bool operator==(const Vector2<U>& other) const {
    return NearEquals(x, static_cast<T>(other.x)) && NearEquals(y, static_cast<T>(other.y));
  }
  template <typename U>
  bool operator!=(const Vector2<U>& rhs) const {
    return !operator==(rhs);
  }

  // Output.
  friend std::ostream& operator<<(std::ostream& os, const Vector2<T>& vec) {
    os << "(" << vec.x << ", " << vec.y << ")";
    return os;
  }
};

// Helper typedefs.
typedef Vector2<float> Vector2f;
typedef Vector2<double> Vector2d;
typedef Vector2<int> Vector2i;

}  // namespace donner
