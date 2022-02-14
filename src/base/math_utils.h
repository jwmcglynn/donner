#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "src/base/utils.h"

namespace donner {
namespace details {

template <typename T>
struct AddUnsigned {
private:
  typedef typename std::enable_if<std::is_integral<T>::value, T>::type EnableType;

public:
  typedef EnableType type;
};

template <>
struct AddUnsigned<char> {
  typedef unsigned char type;
};
template <>
struct AddUnsigned<signed char> {
  typedef unsigned char type;
};
template <>
struct AddUnsigned<short> {
  typedef unsigned short type;
};
template <>
struct AddUnsigned<int> {
  typedef unsigned int type;
};
template <>
struct AddUnsigned<long> {
  typedef unsigned long type;
};
template <>
struct AddUnsigned<long long> {
  typedef unsigned long long type;
};

// Don't define these, these don't have an unsigned counterpart.
template <>
struct AddUnsigned<bool>;
template <>
struct AddUnsigned<wchar_t>;

}  // namespace details

template <typename T>
struct MathConstants {};

template <>
struct MathConstants<float> {
  static constexpr float kPi = 3.14159265359f;
  static constexpr float kReciprocalPi = 1.0f / kPi;
  static constexpr float kHalfPi = kPi / 2.0f;
  static constexpr float kDegToRad = kPi / 180.0f;
  static constexpr float kRadToDeg = 180.0f / kPi;
};

template <>
struct MathConstants<double> {
  static constexpr double kPi = 3.1415926535897932384626433832795028841971693993751;
  static constexpr double kReciprocalPi = 1.0 / kPi;
  static constexpr double kHalfPi = kPi / 2.0;
  static constexpr double kDegToRad = kPi / 180.0;
  static constexpr double kRadToDeg = 180.0 / kPi;
};

/**
 * Returns minimum of the provided values.
 */
template <typename T>
inline const T& Min(const T& a, const T& b) {
  return a < b ? a : b;
}

/**
 * Returns minimum of the provided values.
 */
template <typename T, typename... Args>
inline const T& Min(const T& a, const T& b, Args&&... args) {
  return Min(Min(a, b), std::forward<Args>(args)...);
}

/**
 * Returns maximum of the provided values.
 */
template <typename T>
inline const T& Max(const T& a, const T& b) {
  return a < b ? b : a;
}

/**
 * Returns maximum of the provided values.
 */
template <typename T, typename... Args>
inline const T& Max(const T& a, const T& b, Args&&... args) {
  return Max(Max(a, b), std::forward<Args>(args)...);
}

/**
 * Returns the absolute value of the number.
 */
inline float Abs(float a) {
  return fabsf(a);
}

/**
 * Returns the absolute value of the number.
 */
inline double Abs(double a) {
  return fabs(a);
}

/**
 * Returns the absolute value of the number.
 */
template <typename T,
          typename = std::enable_if<std::is_integral<T>::value && std::is_signed<T>::value>>
inline T Abs(T a) {
  if (UTILS_PREDICT_FALSE(a == std::numeric_limits<T>::lowest())) {
    return std::numeric_limits<T>::max();
  }

  return a < 0 ? -a : a;
}

/**
 * Round a floating point value to an integer.
 */
template <typename T, typename = std::enable_if<std::is_floating_point<T>::value>>
inline T Round(T orig) {
  return static_cast<T>(floor(orig + 0.5));
}

/**
 * Returns linear interpolation of @a a and @a b with ratio @a t.
 *
 * @return @a a if t == 0, @a b if t == 1, and the linear interpolation else.
 */
template <typename T>
inline T Lerp(T a, T b, const float t) {
  assert(t >= 0.0f && t <= 1.0f);
  return T(a * (1.0f - t)) + (b * t);
}

/**
 * Clamps a value between low and high.
 */
template <typename T>
inline const T Clamp(T value, T low, T high) {
  return Min(Max(value, low), high);
}

/**
 * Returns if @a a equals @a b, taking possible rounding errors into account.
 */
template <typename T>
inline bool NearEquals(T a, T b, T tolerance = std::numeric_limits<T>::epsilon()) {
  return (b <= a + tolerance) && (a <= b + tolerance);
}

/**
 * Returns if @a a equals zero, taking rounding errors into account.
 */
template <typename T>
inline bool NearZero(T a, T tolerance = std::numeric_limits<T>::epsilon()) {
  return Abs(a) <= tolerance;
}

/**
 * Test if a variable is in a specific range, using an optimized technique
 * that requires only one branch.  Some compilers do this automatically.
 *
 * Example:
 * \code{.cpp}
 *   if (InRange(var, 'a', 'z')) // ...
 * \endcode
 */
template <typename T, typename = std::enable_if<std::is_integral<T>::value>>
inline bool InRange(T var, T start, T end) {
  assert(start <= end);

  using UnsignedT = typename details::AddUnsigned<T>::type;
  return static_cast<UnsignedT>(var - start) <= UnsignedT(end - start);
}

template <typename T>
struct QuadraticSolution {
  T solution[2] = {};
  bool hasSolution = false;
};

/**
 * Solve a quadratic equation.
 *
 * \f$ a x^2 + b x + c = 0 \f$
 *
 * @param a First coefficient.
 * @param b Second coefficient.
 * @param c Third coefficient.
 * @return QuadraticSolution, containing 0-2 solutions.
 */
template <typename T>
QuadraticSolution<T> SolveQuadratic(T a, T b, T c) {
  QuadraticSolution<T> res;

  // b^2 - 4ac.
  T sqrtContent = b * b - T(4) * a * c;

  // Check to see if any solutions exist.
  if (sqrtContent < T(0) || a == T(0)) {
    res.hasSolution = false;
    return res;
  }

  // Build up the equation.
  sqrtContent = sqrt(sqrtContent);

  // Solve the two conditions.
  res.solution[0] = (-b + sqrtContent) / (T(2) * a);
  res.solution[1] = (-b - sqrtContent) / (T(2) * a);
  res.hasSolution = true;
  return res;
}

}  // namespace donner
