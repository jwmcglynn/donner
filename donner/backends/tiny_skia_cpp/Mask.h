#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace donner::backends::tiny_skia_cpp {

/**
 * An 8-bit alpha mask storing per-pixel coverage.
 */
class Mask {
public:
  /// Constructs an empty (invalid) mask.
  Mask() = default;

  /**
   * Allocates a mask sized to \a width x \a height.
   *
   * @param width Mask width in pixels.
   * @param height Mask height in pixels.
   */
  static Mask Create(int width, int height);

  /// Returns true when the mask is allocated.
  bool isValid() const { return width_ > 0 && height_ > 0 && !pixels_.empty(); }

  /// Mask width in pixels.
  int width() const { return width_; }

  /// Mask height in pixels.
  int height() const { return height_; }

  /// Number of bytes between rows.
  size_t strideBytes() const { return strideBytes_; }

  /// Mutable view of the mask buffer.
  std::span<uint8_t> pixels() { return pixels_; }

  /// Immutable view of the mask buffer.
  std::span<const uint8_t> pixels() const { return pixels_; }

  /// Mutable pointer to the mask buffer.
  uint8_t* data() { return pixels_.data(); }

  /// Immutable pointer to the mask buffer.
  const uint8_t* data() const { return pixels_.data(); }

  /// Fills the entire mask with the given coverage value.
  void clear(uint8_t coverage);

private:
  Mask(int width, int height, size_t strideBytes, std::vector<uint8_t>&& pixels);

  int width_ = 0;
  int height_ = 0;
  size_t strideBytes_ = 0;
  std::vector<uint8_t> pixels_;
};

}  // namespace donner::backends::tiny_skia_cpp
