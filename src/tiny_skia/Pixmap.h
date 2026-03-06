#pragma once

/// @file Pixmap.h
/// @brief Pixel buffers and views for RGBA image data.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"

namespace tiny_skia {

/// Bytes per pixel (always 4: RGBA premultiplied).
inline constexpr std::size_t kBytesPerPixel = 4;

class Pixmap;
struct MutableSubPixmapView;

/// Immutable view into RGBA pixel data. Does not own memory.
class PixmapView {
 public:
  PixmapView() = default;

  /// Creates a view from raw byte data. Returns nullopt if size mismatches.
  static std::optional<PixmapView> fromBytes(std::span<const std::uint8_t> data, std::uint32_t width,
                                            std::uint32_t height);

  [[nodiscard]] std::uint32_t width() const { return size_.width(); }
  [[nodiscard]] std::uint32_t height() const { return size_.height(); }
  [[nodiscard]] IntSize size() const { return size_; }

  /// Raw byte data (premultiplied RGBA).
  [[nodiscard]] std::span<const std::uint8_t> data() const {
    return std::span<const std::uint8_t>(data_, len_);
  }

  /// Pixel data as PremultipliedColorU8 span.
  [[nodiscard]] std::span<const PremultipliedColorU8> pixels() const;

  /// Returns the pixel at (x, y), or nullopt if out of bounds.
  [[nodiscard]] std::optional<PremultipliedColorU8> pixel(std::uint32_t x, std::uint32_t y) const;

  /// Clones a rectangular region into a new Pixmap.
  [[nodiscard]] std::optional<Pixmap> cloneRect(const IntRect& rect) const;

 private:
  friend class Pixmap;

  explicit PixmapView(const std::uint8_t* data, std::size_t len, IntSize size)
      : data_(data), len_(len), size_(size) {}

  const std::uint8_t* data_ = nullptr;
  std::size_t len_ = 0;
  IntSize size_;
};

/// Mutable view into RGBA pixel data. Does not own memory.
/// Primary drawing target for all rendering operations.
class MutablePixmapView {
 public:
  MutablePixmapView() = default;
  explicit MutablePixmapView(std::uint8_t* data, std::size_t len, IntSize size)
      : data_(data), len_(len), size_(size) {}

  /// Creates a mutable view from raw byte data. Returns nullopt if size mismatches.
  static std::optional<MutablePixmapView> fromBytes(std::span<std::uint8_t> data, std::uint32_t width,
                                            std::uint32_t height);

  [[nodiscard]] std::uint32_t width() const { return size_.width(); }
  [[nodiscard]] std::uint32_t height() const { return size_.height(); }
  [[nodiscard]] IntSize size() const { return size_; }

  /// Raw mutable byte data (premultiplied RGBA).
  [[nodiscard]] std::span<std::uint8_t> data() const {
    return std::span<std::uint8_t>(data_, len_);
  }

  /// Pixel data as mutable PremultipliedColorU8 span.
  [[nodiscard]] std::span<PremultipliedColorU8> pixels() const;

  /// Returns a sub-view covering the full pixmap.
  [[nodiscard]] MutableSubPixmapView subpixmap() const;

  /// Returns a sub-view for the given rectangle, or nullopt if out of bounds.
  [[nodiscard]] std::optional<MutableSubPixmapView> subpixmap(const IntRect& rect) const;

 private:
  std::uint8_t* data_ = nullptr;
  std::size_t len_ = 0;
  IntSize size_;
};

/// @internal
/// Mutable sub-region view used internally by the rendering pipeline.
struct MutableSubPixmapView {
  IntSize size{};
  std::size_t realWidth = 0;
  std::uint8_t* data = nullptr;

  [[nodiscard]] std::size_t width() const { return size.width(); }
  [[nodiscard]] std::size_t height() const { return size.height(); }
  [[nodiscard]] std::span<std::uint8_t> dataSpan() const;
};

/// Owned RGBA pixel buffer. Always premultiplied alpha internally.
///
/// Create with Pixmap::fromSize() and obtain a MutablePixmapView via
/// mutableView() for drawing. Call releaseDemultiplied() to extract
/// straight-alpha bytes for PNG encoding.
class Pixmap {
 public:
  Pixmap() = default;

  /// Creates a zero-filled pixmap. Returns nullopt for zero dimensions.
  static std::optional<Pixmap> fromSize(std::uint32_t width, std::uint32_t height);

  /// Creates a pixmap from existing data. Returns nullopt if size mismatches.
  static std::optional<Pixmap> fromVec(std::vector<std::uint8_t> data, IntSize size);

  /// Immutable view.
  [[nodiscard]] PixmapView view() const { return PixmapView(data_.data(), data_.size(), size_); }

  /// Mutable view — the primary drawing target.
  [[nodiscard]] MutablePixmapView mutableView() { return MutablePixmapView(data_.data(), data_.size(), size_); }

  [[nodiscard]] std::uint32_t width() const { return size_.width(); }
  [[nodiscard]] std::uint32_t height() const { return size_.height(); }
  [[nodiscard]] IntSize size() const { return size_; }

  /// Raw byte data (premultiplied RGBA), const.
  [[nodiscard]] std::span<const std::uint8_t> data() const {
    return std::span<const std::uint8_t>(data_.data(), data_.size());
  }

  /// Raw byte data (premultiplied RGBA), mutable.
  [[nodiscard]] std::span<std::uint8_t> data() {
    return std::span<std::uint8_t>(data_.data(), data_.size());
  }

  [[nodiscard]] std::span<const PremultipliedColorU8> pixels() const;
  [[nodiscard]] std::span<PremultipliedColorU8> pixels();

  /// Returns the pixel at (x, y), or nullopt if out of bounds.
  [[nodiscard]] std::optional<PremultipliedColorU8> pixel(std::uint32_t x, std::uint32_t y) const;

  /// Clones a rectangular region into a new Pixmap.
  [[nodiscard]] std::optional<Pixmap> cloneRect(const IntRect& rect) const;

  /// Fills the entire pixmap with a color (premultiplied internally).
  void fill(const Color& color);

  /// Releases the raw byte buffer (premultiplied alpha).
  [[nodiscard]] std::vector<std::uint8_t> release();

  /// Releases the byte buffer after converting to straight (non-premultiplied) alpha.
  /// Use this for PNG encoding.
  [[nodiscard]] std::vector<std::uint8_t> releaseDemultiplied();

 private:
  explicit Pixmap(std::vector<std::uint8_t> data, IntSize size)
      : data_(std::move(data)), size_(size) {}

  std::vector<std::uint8_t> data_;
  IntSize size_;
};

}  // namespace tiny_skia
