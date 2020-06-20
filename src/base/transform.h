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
 * @f[
 * \begin{bmatrix}
 *   a & c & 0 & e  \\
 *   b & d & 0 & f  \\
 *   0 & 0 & 1 & 0  \\
 *   0 & 0 & 0 & 1  \\
 * \end{bmatrix}
 * @f]
 *
 * Elements are stored in column-major order.
 *
 * @tparam T Element type.
 */
template <typename T>
struct Transform {
  T data[6];  // For the layout above, stores [a b c d e f]

  Transform() {
    data[0] = T(1);
    data[1] = T(0);
    data[2] = T(0);
    data[3] = T(1);
    data[4] = T(0);
    data[5] = T(0);
  }

  explicit Transform(UninitializedTag) {}

  /**
   * Return a 2D rotation matrix with the given angle, in radians.
   *
   * @param theta Angle in radians.
   */
  static Transform Rotation(T theta) {
    const T sin_val = std::sin(theta);
    const T cos_val = std::cos(theta);

    Transform<T> result(uninitialized);
    result.data[0] = cos_val;
    result.data[1] = sin_val;
    result.data[2] = -sin_val;
    result.data[3] = cos_val;
    result.data[4] = T(0);
    result.data[5] = T(0);
    return result;
  }

  /**
   * Return a 2D scale matrix.
   *
   * @param extent Scale x/y parameters.
   */
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

  /**
   * Return a 2D translation matrix.
   *
   * @param offset Translation offset.
   */
  static Transform Translate(const Vector2<T>& offset) {
    Transform<T> result;
    result.data[4] = offset.x;
    result.data[5] = offset.y;
    return result;
  }

  /**
   * Returns a 2D skew transformation along the X axis, as defined by
   * https://www.w3.org/TR/css-transforms-1/#SkewXDefined
   *
   * @param theta Angle in radians.
   */
  static Transform SkewX(T theta) {
    const T shear = std::tan(theta);

    Transform<T> result;
    result.data[2] = shear;
    return result;
  }

  /**
   * Returns a 2D skew transformation along the Y axis, as defined by
   * https://www.w3.org/TR/css-transforms-1/#SkewYDefined
   *
   * @param theta Angle in radians.
   */
  static Transform SkewY(T theta) {
    const T shear = std::tan(theta);

    Transform<T> result;
    result.data[1] = shear;
    return result;
  }

  /**
   * Returns true if this transform is equal to the identity matrix.
   */
  bool isIdentity() const {
    return (NearEquals(data[0], T(1)) && NearZero(data[1]) && NearZero(data[2]) &&
            NearEquals(data[3], T(1)) && NearZero(data[4]) && NearZero(data[5]));
  }

  /**
   * Returns the determinant.
   */
  T determinant() const { return data[0] * data[3] - data[1] * data[2]; }

  /**
   * Returns the inverse of this transform.
   */
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

  /**
   * Transforms a column vector, applying rotations/scaling but not translation.
   *
   * @f{
   *   v' = M \begin{bmatrix}
   *            v_x  \\
   *            v_y  \\
   *             0   \\
   *             0
   *          \end{bmatrix}
   * @f}
   *
   * @param v Vector to transform.
   * @result Transformed vector.
   */
  Vector2<T> transformVector(const Vector2<T>& v) const {
    return Vector2<T>(data[0] * v.x + data[2] * v.y,  //
                      data[1] * v.x + data[3] * v.y);
  }

  /**
   * Transforms a position given as a vector.
   *
   * @f[
   *  v' = M \begin{bmatrix}
   *           v_x  \\
   *           v_y  \\
   *            0   \\
   *            1
   *         \end{bmatrix}
   * @f]
   *
   * @param v Vector to transform.
   * @result Transformed vector.
   */
  Vector2<T> transformPosition(const Vector2<T>& v) const {
    return Vector2<T>(data[0] * v.x + data[2] * v.y + data[4],
                      data[1] * v.x + data[3] * v.y + data[5]);
  }

  /**
   * Transform an axis-aligned bounding box, returning a new axis-aligned bounding box with the
   * result.
   *
   * @param box Box to transform.
   */
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

  /**
   * Post-multiplies rhs with this transform.
   *
   * Example: Take A, transform by T, transform by R is written as
   * \code{.cpp}
   *   R * T * A
   * \endcode
   *
   * @param rhs Other transform.
   */
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

  /**
   * Post-multiplies rhs with this transform, equivalent to (*this * rhs).
   *
   * Note that when applying transformations, transformations typically need to be applied
   * right-to-left so this operator may be backwards.
   *
   * @param rhs Other transform.
   */
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

  // Output.
  friend std::ostream& operator<<(std::ostream& os, const Transform<T>& t) {
    os << "matrix(" << t.data[0] << " " << t.data[1] << " " << t.data[2] << " " << t.data[3] << " "
       << t.data[4] << " " << t.data[5] << ") => " << std::endl
       << "[ " << t.data[0] << "\t" << t.data[2] << "\t0\t" << t.data[4] << std::endl
       << "  " << t.data[1] << "\t" << t.data[3] << "\t0\t" << t.data[5] << std::endl
       << "  0\t0\t1\t0" << std::endl
       << "  0\t0\t0\t1 ]" << std::endl;
    return os;
  }
};

// Helper typedefs.
typedef Transform<float> Transformf;
typedef Transform<double> Transformd;

}  // namespace donner
