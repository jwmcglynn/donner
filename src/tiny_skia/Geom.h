#pragma once

/// @file Geom.h
/// @brief Geometric primitives: Rect, IntRect, ScreenIntRect, IntSize.

#include <cstdint>
#include <optional>

#include "tiny_skia/Math.h"

namespace tiny_skia {

class ScreenIntRect;
class IntRect;
class Rect;

/// Non-zero integer dimensions (width x height).
class IntSize {
 public:
  constexpr IntSize() = default;
  constexpr IntSize(LengthU32 width, LengthU32 height) : width_(width), height_(height) {}

  /// Creates from width/height. Returns nullopt if either is zero.
  static std::optional<IntSize> fromWH(LengthU32 width, LengthU32 height) {
    if (width == 0u || height == 0u) {
      return std::nullopt;
    }
    return IntSize{width, height};
  }

  constexpr LengthU32 width() const { return width_; }
  constexpr LengthU32 height() const { return height_; }
  [[nodiscard]] ScreenIntRect toScreenIntRect(std::uint32_t x, std::uint32_t y) const;
  [[nodiscard]] std::optional<IntRect> toIntRect(std::int32_t x, std::int32_t y) const;
  [[nodiscard]] Rect toRect() const;

  constexpr bool operator==(const IntSize&) const = default;

 private:
  LengthU32 width_ = 0;
  LengthU32 height_ = 0;
};

/// Unsigned integer rectangle (x, y, width, height). Used for screen coordinates.
class ScreenIntRect {
 public:
  constexpr ScreenIntRect() = default;

  /// Creates from components. Returns nullopt for zero width/height or overflow.
  static std::optional<ScreenIntRect> fromXYWH(std::uint32_t x, std::uint32_t y,
                                               std::uint32_t width, std::uint32_t height);

  static constexpr ScreenIntRect fromXYWHSafe(std::uint32_t x, std::uint32_t y, LengthU32 width,
                                              LengthU32 height) {
    return ScreenIntRect{x, y, width, height};
  }

  constexpr std::uint32_t x() const { return x_; }
  constexpr std::uint32_t y() const { return y_; }
  constexpr LengthU32 width() const { return width_; }
  constexpr LengthU32 height() const { return height_; }
  constexpr LengthU32 widthSafe() const { return width_; }

  constexpr std::uint32_t left() const { return x_; }
  constexpr std::uint32_t top() const { return y_; }

  std::uint32_t right() const;
  std::uint32_t bottom() const;
  IntSize size() const;
  [[nodiscard]] bool contains(const ScreenIntRect& other) const;
  [[nodiscard]] IntRect toIntRect() const;
  [[nodiscard]] Rect toRect() const;

 private:
  constexpr ScreenIntRect(std::uint32_t x, std::uint32_t y, LengthU32 width, LengthU32 height)
      : x_{x}, y_{y}, width_{width}, height_{height} {}

  std::uint32_t x_ = 0;
  std::uint32_t y_ = 0;
  LengthU32 width_;
  LengthU32 height_;
};

/// Signed integer rectangle (x, y, width, height).
class IntRect {
 public:
  /// Creates from components. Returns nullopt for zero dimensions or overflow.
  static std::optional<IntRect> fromXYWH(std::int32_t x, std::int32_t y, std::uint32_t width,
                                         std::uint32_t height);

  constexpr std::int32_t x() const { return x_; }
  constexpr std::int32_t y() const { return y_; }
  constexpr LengthU32 width() const { return width_; }
  constexpr LengthU32 height() const { return height_; }

  [[nodiscard]] constexpr std::int32_t left() const { return x_; }
  [[nodiscard]] constexpr std::int32_t top() const { return y_; }

  [[nodiscard]] std::int32_t right() const;
  [[nodiscard]] std::int32_t bottom() const;

  /// Returns the intersection with another rect, or nullopt if disjoint.
  [[nodiscard]] std::optional<IntRect> intersect(const IntRect& other) const;

  [[nodiscard]] std::optional<ScreenIntRect> toScreenIntRect() const;

 private:
  constexpr IntRect(std::int32_t x, std::int32_t y, LengthU32 width, LengthU32 height)
      : x_{x}, y_{y}, width_{width}, height_{height} {}

  std::int32_t x_ = 0;
  std::int32_t y_ = 0;
  LengthU32 width_ = 0;
  LengthU32 height_ = 0;
};

/// Floating-point rectangle (left, top, right, bottom).
/// All components must be finite, and width/height must be > 0.
class Rect {
 public:
  /// Creates from edges. Returns nullopt for non-finite, empty, or inverted rects.
  static std::optional<Rect> fromLTRB(float left, float top, float right, float bottom);

  /// Creates from origin and size. Returns nullopt if invalid.
  static std::optional<Rect> fromXYWH(float x, float y, float w, float h) {
    return fromLTRB(x, y, x + w, y + h);
  }

  constexpr float left() const { return left_; }
  constexpr float top() const { return top_; }
  constexpr float right() const { return right_; }
  constexpr float bottom() const { return bottom_; }

  constexpr float width() const { return right_ - left_; }
  constexpr float height() const { return bottom_ - top_; }

  constexpr bool operator==(const Rect&) const = default;

  /// Converts to IntRect by rounding outward (floor left/top, ceil right/bottom).
  [[nodiscard]] std::optional<IntRect> roundOut() const;
  /// Converts to IntRect by rounding to nearest integer.
  [[nodiscard]] std::optional<IntRect> round() const;

 private:
  constexpr Rect(float left, float top, float right, float bottom)
      : left_(left), top_(top), right_(right), bottom_(bottom) {}

  float left_ = 0.0f;
  float top_ = 0.0f;
  float right_ = 0.0f;
  float bottom_ = 0.0f;
};

/// @internal
std::optional<ScreenIntRect> intRectToScreen(const IntRect& rect);

}  // namespace tiny_skia
