#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace donner::backends::tiny_skia_cpp {

/** An owned RGBA pixmap with tightly packed rows. */
class Pixmap {
public:
  /// Constructs an empty (invalid) pixmap.
  Pixmap() = default;

  /**
   * Creates a pixmap sized to \a width x \a height with 4 bytes per pixel.
   *
   * @param width Image width in pixels.
   * @param height Image height in pixels.
   */
  static Pixmap Create(int width, int height);

  /// Returns true when the pixmap contains allocated pixel data.
  bool isValid() const { return width_ > 0 && height_ > 0 && !pixels_.empty(); }

  /// Returns the image width in pixels.
  int width() const { return width_; }

  /// Returns the image height in pixels.
  int height() const { return height_; }

  /// Returns the stride in bytes between rows.
  size_t strideBytes() const { return strideBytes_; }

  /// Returns a mutable view of the pixel buffer.
  std::span<uint8_t> pixels() { return pixels_; }

  /// Returns an immutable view of the pixel buffer.
  std::span<const uint8_t> pixels() const { return pixels_; }

  /// Returns a mutable pointer to the pixel buffer.
  uint8_t* data() { return pixels_.data(); }

  /// Returns an immutable pointer to the pixel buffer.
  const uint8_t* data() const { return pixels_.data(); }

private:
  Pixmap(int width, int height, size_t strideBytes, std::vector<uint8_t>&& pixels);

  static constexpr size_t kBytesPerPixel = 4;

  int width_ = 0;
  int height_ = 0;
  size_t strideBytes_ = 0;
  std::vector<uint8_t> pixels_;
};

}  // namespace donner::backends::tiny_skia_cpp
