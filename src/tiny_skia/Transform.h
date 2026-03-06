#pragma once

/// @file Transform.h
/// @brief 2D affine transformation matrix.

#include <optional>
#include <span>

namespace tiny_skia {

struct Point;

/// 2D affine transformation: [sx kx tx; ky sy ty; 0 0 1].
///
/// Supports translate, scale, skew. Use fromTranslate/fromScale for common
/// cases, fromRow for arbitrary matrices. Compose with preConcat/postConcat.
class Transform {
 public:
  float sx = 1.0f;  ///< Horizontal scale.
  float kx = 0.0f;  ///< Horizontal skew.
  float ky = 0.0f;  ///< Vertical skew.
  float sy = 1.0f;  ///< Vertical scale.
  float tx = 0.0f;  ///< Horizontal translation.
  float ty = 0.0f;  ///< Vertical translation.

  constexpr Transform() = default;

  [[nodiscard]] static constexpr Transform identity() { return Transform{}; }

  /// Creates from all 6 matrix entries (row-major: sx, ky, kx, sy, tx, ty).
  [[nodiscard]] static constexpr Transform fromRow(float sx, float ky, float kx, float sy, float tx,
                                                   float ty) {
    Transform t;
    t.sx = sx;
    t.ky = ky;
    t.kx = kx;
    t.sy = sy;
    t.tx = tx;
    t.ty = ty;
    return t;
  }

  [[nodiscard]] static constexpr Transform fromTranslate(float tx, float ty) {
    return fromRow(1.0f, 0.0f, 0.0f, 1.0f, tx, ty);
  }

  [[nodiscard]] static constexpr Transform fromScale(float sx, float sy) {
    return fromRow(sx, 0.0f, 0.0f, sy, 0.0f, 0.0f);
  }

  [[nodiscard]] bool isFinite() const;
  [[nodiscard]] bool isIdentity() const;
  [[nodiscard]] bool isTranslate() const;
  [[nodiscard]] bool isScaleTranslate() const;
  [[nodiscard]] bool hasScale() const;
  [[nodiscard]] bool hasSkew() const;
  [[nodiscard]] bool hasTranslate() const;

  /// Returns the inverse, or nullopt if singular.
  [[nodiscard]] std::optional<Transform> invert() const;

  /// Returns this * other (other applied first).
  [[nodiscard]] Transform preConcat(const Transform& other) const;
  /// Returns other * this (this applied first).
  [[nodiscard]] Transform postConcat(const Transform& other) const;
  [[nodiscard]] Transform preScale(float sx, float sy) const;
  [[nodiscard]] Transform postScale(float sx, float sy) const;
  [[nodiscard]] Transform preTranslate(float tx, float ty) const;
  [[nodiscard]] Transform postTranslate(float tx, float ty) const;

  /// Transforms an array of points in-place.
  void mapPoints(std::span<Point> points) const;

  constexpr bool operator==(const Transform&) const = default;
};

}  // namespace tiny_skia
