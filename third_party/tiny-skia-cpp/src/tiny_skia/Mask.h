#pragma once

/// @file Mask.h
/// @brief Alpha mask for clipping drawing operations.

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "tiny_skia/Geom.h"
#include "tiny_skia/Pixmap.h"

namespace tiny_skia {

// Forward declarations for fillPath/intersectPath.
class Path;
class Transform;
enum class FillRule : std::uint8_t;

/// How to extract mask data from a pixmap.
enum class MaskType : std::uint8_t {
  Alpha = 0,     ///< Use the alpha channel.
  Luminance = 1, ///< Use luminance (weighted RGB).
};

/// @internal
/// Immutable sub-region view into mask data.
struct SubMaskView {
  IntSize size{};
  std::uint32_t realWidth = 0;
  const std::uint8_t* data = nullptr;
};

/// @internal
/// Mutable sub-region view into mask data.
struct MutableSubMaskView {
  IntSize size{};
  std::uint32_t realWidth = 0;
  std::uint8_t* data = nullptr;
};

/// 8-bit alpha mask for clipping.
///
/// Create with fromSize() (zero-filled), then draw paths with fillPath()
/// or derive from a pixmap with fromPixmap(). Pass to Painter methods
/// to clip drawing to the mask.
class Mask {
 public:
  Mask() = default;

  /// Creates a zero-filled mask. Returns nullopt for zero dimensions.
  static std::optional<Mask> fromSize(std::uint32_t width, std::uint32_t height);

  /// Creates a mask from existing data. Returns nullopt if size mismatches.
  static std::optional<Mask> fromVec(std::vector<std::uint8_t> data, IntSize size);

  /// Extracts a mask from a pixmap's alpha or luminance channel.
  static Mask fromPixmap(const PixmapView& pixmap, MaskType maskType);

  [[nodiscard]] std::uint32_t width() const { return size_.width(); }
  [[nodiscard]] std::uint32_t height() const { return size_.height(); }
  [[nodiscard]] IntSize size() const { return size_; }

  [[nodiscard]] std::span<const std::uint8_t> data() const {
    return std::span<const std::uint8_t>(data_.data(), data_.size());
  }

  [[nodiscard]] std::span<std::uint8_t> data() {
    return std::span<std::uint8_t>(data_.data(), data_.size());
  }

  /// @internal
  [[nodiscard]] SubMaskView submask() const;
  /// @internal
  [[nodiscard]] std::optional<SubMaskView> submask(IntRect rect) const;
  /// @internal
  [[nodiscard]] MutableSubMaskView subpixmap();
  /// @internal
  [[nodiscard]] std::optional<MutableSubMaskView> subpixmap(IntRect rect);

  /// Releases the underlying byte buffer.
  [[nodiscard]] std::vector<std::uint8_t> release();

  /// Draws a filled path onto the mask (white=255 where filled).
  void fillPath(const Path& path, FillRule fillRule, bool antiAlias, Transform transform);

  /// Intersects (AND) a path with the current mask contents.
  void intersectPath(const Path& path, FillRule fillRule, bool antiAlias, Transform transform);

  /// Inverts all mask values (255 - x).
  void invert();

  /// Clears the mask to zero.
  void clear();

 private:
  explicit Mask(std::vector<std::uint8_t> data, IntSize size)
      : data_(std::move(data)), size_(size) {}

  std::vector<std::uint8_t> data_;
  IntSize size_;
};

}  // namespace tiny_skia
