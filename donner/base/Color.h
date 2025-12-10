#pragma once
/// @file

#include <cstdint>
#include <ostream>
#include <string>

namespace donner {

/**
 * Represents a 32-bit RGBA color with 8-bit channels.
 */
struct RGBA {
  uint8_t r = 0xFF;  ///< Red component, in the range [0, 255].
  uint8_t g = 0xFF;  ///< Green component, in the range [0, 255].
  uint8_t b = 0xFF;  ///< Blue component, in the range [0, 255].
  uint8_t a = 0xFF;  ///< Alpha component, in the range [0, 255].

  /// Default constructor, initializes to fully opaque white.
  constexpr RGBA() = default;

  /// Constructor, initializes to the given RGBA values.
  constexpr RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) : r(r), g(g), b(b), a(a) {}

  /// Constructor for fully opaque RGB colors.
  static constexpr RGBA RGB(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b, 0xFF}; }

  /// Equality operator.
  bool operator==(const RGBA&) const = default;

  /**
   * Converts the color to a hex string.
   *
   * @returns '#rrggbb' if the color is opaque, or '#rrggbbaa' if it contains alpha.
   */
  std::string toHexString() const;
};

/**
 * Outputs: `rgba(r, g, b, a)`.
 *
 * @param os Output stream.
 * @param color Color to output.
 */
std::ostream& operator<<(std::ostream& os, const RGBA& color);

}  // namespace donner
