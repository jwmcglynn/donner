#pragma once

#include <ostream>

#include "src/base/box.h"
#include "src/base/math_utils.h"
#include "src/base/vector2.h"

namespace donner {

struct UninitializedTag {};
constexpr UninitializedTag uninitialized;

/**
 * A 2D 3x2 matrix with six parameters, equivalent to the matrix:
 *
 *  | a c 0 e |
 *  | b d 0 f |
 *  | 0 0 1 0 |
 *  | 0 0 0 1 |
 *
 * Elements are stored in column-major order.
 *
 * @tparam T Element type.
 */
template <typename T>
struct Transform {
  T data[6];

  Transform() {
    data[0] = T(1);
    data[1] = T(0);
    data[2] = T(0);
    data[3] = T(1);
    data[4] = T(0);
    data[5] = T(0);
  }

  explicit Transform(UninitializedTag) {}

  static Transform Rotation(T theta) {
    const T sin_val = (T)sin(theta);
    const T cos_val = (T)cos(theta);

    Transform<T> result(uninitialized);
    result.data[0] = cos_val;
    result.data[1] = sin_val;
    result.data[2] = -sin_val;
    result.data[3] = cos_val;
    result.data[4] = T(0);
    result.data[5] = T(0);
    return result;
  }

  static Transform Scale(const Vector2<T>& extent) {
    Transform<T> result(uninitialized);
    result.data[0] = extent.x;
    result.data[1] = T(0);
    result.data[2] = T(0);
    result.data[3] = extent.y;
    result.data[4] = T(0);
    result.data[5] = T(0);
    return result;
  }

  static Transform Translate(const Vector2<T>& offset) {
    Transform<T> result;
    result.data[4] = offset.x;
    result.data[5] = offset.y;
    return result;
  }

  static Transform ShearX(T theta) {
    const T shear = (T)tan(theta);

    Transform<T> result;
    result.data[2] = shear;
    return result;
  }

  static Transform ShearY(T theta) {
    const T shear = (T)tan(theta);

    Transform<T> result;
    result.data[1] = shear;
    return result;
  }

  bool isIdentity() const {
    return (NearEquals(data[0], T(1)) && NearZero(data[1]) && NearZero(data[2]) &&
            NearEquals(data[3], T(1)) && NearZero(data[4]) && NearZero(data[5]));
  }

  T determinant() const { return data[0] * data[3] - data[1] * data[2]; }

  Transform<T> inversed() const {
    const T invDet = T(1.0) / determinant();

    Transform<T> result(uninitialized);
    result.data[0] = data[3] * invDet;
    result.data[1] = -data[1] * invDet;
    result.data[2] = -data[2] * invDet;
    result.data[3] = data[0] * invDet;
    result.data[4] = -data[4] * result.data[0] - data[5] * result.data[2];
    result.data[5] = -data[4] * result.data[1] - data[5] * result.data[3];
    return result;
  }

  Vector2<T> transformVector(const Vector2<T>& v) const {
    return Vector2<T>(data[0] * v.x + data[2] * v.y,  //
                      data[1] * v.x + data[3] * v.y);
  }

  Vector2<T> transformPosition(const Vector2<T>& v) const {
    return Vector2<T>(data[0] * v.x + data[2] * v.y + data[4],
                      data[1] * v.x + data[3] * v.y + data[4]);
  }

  Box<T> transformBox(const Box<T>& box) const {
    const Vector2<T> corners[4] = {box.top_left,                                    //
                                   Vector2<T>(box.bottom_right.x, box.top_left.y),  //
                                   box.bottom_right,                                //
                                   Vector2<T>(box.top_left.x, box.bottom_right.y)};

    Box<T> result = Box<T>::CreateEmpty(transformPosition(corners[0]));
    result.addPoint(transformPosition(corners[1]));
    result.addPoint(transformPosition(corners[2]));
    result.addPoint(transformPosition(corners[3]));
    return result;
  }

  Transform<T> operator*(const Transform<T>& rhs) const {
    Transform<T> result(uninitialized);
    result.data[0] = data[0] * rhs.data[0] + data[1] * rhs.data[2];
    result.data[1] = data[0] * rhs.data[1] + data[1] * rhs.data[3];
    result.data[2] = data[2] * rhs.data[0] + data[3] * rhs.data[2];
    result.data[3] = data[2] * rhs.data[1] + data[3] * rhs.data[3];
    result.data[4] = data[4] * rhs.data[0] + data[5] * rhs.data[2] + rhs.data[4];
    result.data[5] = data[4] * rhs.data[1] + data[5] * rhs.data[3] + rhs.data[5];
    return result;
  }

  Transform<T>& operator*=(const Transform<T>& rhs) {
    *this = (*this * rhs);
    return *this;
  }

  Transform<T>& operator=(const Transform<T>& other) {
    for (int i = 0; i < 6; ++i) {
      data[i] = other.data[i];
    }

    return *this;
  }
};

// Helper typedefs.
typedef Transform<float> Transformf;
typedef Transform<double> Transformd;

}  // namespace donner