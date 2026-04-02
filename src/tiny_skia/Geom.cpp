#include "tiny_skia/Geom.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "tiny_skia/FloatingPoint.h"

namespace tiny_skia {

namespace {

constexpr std::uint32_t kMaxCoord =
    static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());

std::optional<LengthU32> checkDimension(std::uint32_t value) {
  return value == 0 || value > kMaxCoord ? std::nullopt : std::optional{LengthU32{value}};
}

}  // namespace

std::optional<ScreenIntRect> ScreenIntRect::fromXYWH(std::uint32_t x, std::uint32_t y,
                                                     std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    return std::nullopt;
  }

  if (x > kMaxCoord || y > kMaxCoord) {
    return std::nullopt;
  }

  auto widthSafe = checkDimension(width);
  auto heightSafe = checkDimension(height);
  if (!widthSafe || !heightSafe) {
    return std::nullopt;
  }

  if (x > kMaxCoord - widthSafe.value()) {
    return std::nullopt;
  }
  if (y > kMaxCoord - heightSafe.value()) {
    return std::nullopt;
  }

  return ScreenIntRect{x, y, widthSafe.value(), heightSafe.value()};
}

std::uint32_t ScreenIntRect::right() const { return x_ + width_; }

std::uint32_t ScreenIntRect::bottom() const { return y_ + height_; }

IntSize ScreenIntRect::size() const { return IntSize{width_, height_}; }

bool ScreenIntRect::contains(const ScreenIntRect& other) const {
  return x_ <= other.x_ && y_ <= other.y_ && right() >= other.right() && bottom() >= other.bottom();
}

IntRect ScreenIntRect::toIntRect() const {
  return IntRect::fromXYWH(static_cast<std::int32_t>(x_), static_cast<std::int32_t>(y_), width_,
                           height_)
      .value();
}

Rect ScreenIntRect::toRect() const {
  return Rect::fromLTRB(static_cast<float>(x_), static_cast<float>(y_),
                        static_cast<float>(x_) + width_, static_cast<float>(y_) + height_)
      .value();
}

std::optional<IntRect> IntRect::fromXYWH(std::int32_t x, std::int32_t y, std::uint32_t width,
                                         std::uint32_t height) {
  if (width == 0 || height == 0) {
    return std::nullopt;
  }

  if (width > kMaxCoord || height > kMaxCoord) {
    return std::nullopt;
  }

  const auto widthSafe = checkDimension(width);
  const auto heightSafe = checkDimension(height);
  if (!widthSafe || !heightSafe) {
    return std::nullopt;
  }

  // Check that right = x + width doesn't overflow int32.
  const auto right = static_cast<std::int64_t>(x) + static_cast<std::int64_t>(width);
  const auto bottom = static_cast<std::int64_t>(y) + static_cast<std::int64_t>(height);
  if (right > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) ||
      bottom > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
    return std::nullopt;
  }

  return IntRect{x, y, widthSafe.value(), heightSafe.value()};
}

std::optional<ScreenIntRect> IntRect::toScreenIntRect() const {
  if (x_ < 0 || y_ < 0) {
    return std::nullopt;
  }
  return ScreenIntRect::fromXYWH(static_cast<std::uint32_t>(x_), static_cast<std::uint32_t>(y_),
                                 width_, height_);
}

std::int32_t IntRect::right() const { return x_ + static_cast<std::int32_t>(width_); }

std::int32_t IntRect::bottom() const { return y_ + static_cast<std::int32_t>(height_); }

std::optional<IntRect> IntRect::intersect(const IntRect& other) const {
  const auto left = std::max(x_, other.x_);
  const auto top = std::max(y_, other.y_);

  const auto rightCoord = std::min(right(), other.right());
  const auto bottomCoord = std::min(bottom(), other.bottom());

  if (rightCoord <= left || bottomCoord <= top) {
    return std::nullopt;
  }

  return IntRect::fromXYWH(
      left, top, static_cast<std::uint32_t>(static_cast<std::uint32_t>(rightCoord - left)),
      static_cast<std::uint32_t>(static_cast<std::uint32_t>(bottomCoord - top)));
}

ScreenIntRect IntSize::toScreenIntRect(std::uint32_t x, std::uint32_t y) const {
  return ScreenIntRect::fromXYWHSafe(x, y, width_, height_);
}

std::optional<IntRect> IntSize::toIntRect(std::int32_t x, std::int32_t y) const {
  return IntRect::fromXYWH(x, y, width_, height_);
}

Rect IntSize::toRect() const {
  return Rect::fromLTRB(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_))
      .value();
}

std::optional<Rect> Rect::fromLTRB(float left, float top, float right, float bottom) {
  if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
      !std::isfinite(bottom)) {
    return std::nullopt;
  }
  if (left > right || top > bottom) {
    return std::nullopt;
  }
  return Rect{left, top, right, bottom};
}

std::optional<IntRect> Rect::roundOut() const {
  const auto left = saturateFloorToI32(left_);
  const auto top = saturateFloorToI32(top_);
  const auto right = saturateCeilToI32(right_);
  const auto bottom = saturateCeilToI32(bottom_);

  const auto width = std::max<std::int64_t>(1, static_cast<std::int64_t>(right - left));
  const auto height = std::max<std::int64_t>(1, static_cast<std::int64_t>(bottom - top));
  if (width > std::numeric_limits<std::uint32_t>::max() ||
      height > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  return IntRect::fromXYWH(left, top, static_cast<std::uint32_t>(width),
                           static_cast<std::uint32_t>(height));
}

std::optional<IntRect> Rect::round() const {
  const auto left = saturateRoundToI32(left_);
  const auto top = saturateRoundToI32(top_);
  const auto right = saturateRoundToI32(right_);
  const auto bottom = saturateRoundToI32(bottom_);

  const auto width = std::max<std::int64_t>(1, static_cast<std::int64_t>(right - left));
  const auto height = std::max<std::int64_t>(1, static_cast<std::int64_t>(bottom - top));
  if (width > std::numeric_limits<std::uint32_t>::max() ||
      height > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  return IntRect::fromXYWH(left, top, static_cast<std::uint32_t>(width),
                           static_cast<std::uint32_t>(height));
}

std::optional<ScreenIntRect> intRectToScreen(const IntRect& rect) { return rect.toScreenIntRect(); }

}  // namespace tiny_skia
