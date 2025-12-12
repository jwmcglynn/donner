#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>
#include <variant>

namespace donner::css {

/**
 * Represents as 32-bit RGBA color, with each component in the range [0, 255].
 *
 * To construct, with an alpha channel:
 * ```
 * RGBA(r, g, b, a);
 * ```
 *
 * With no alpha:
 * ```
 * RGBA::RGB(r, g, b);
 * ```
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

  /// Constructor, for RGB colors, which are fully opaque.
  static constexpr RGBA RGB(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b, 0xFF}; }

  /// Equality operator.
  bool operator==(const RGBA&) const = default;

  /**
   * Convert the color to a hex string, such as `#ff0000`.
   *
   * @returns '#rrggbb' if the color is opaque, or '#rrggbbaa' if the color has an alpha channel.
   */
  std::string toHexString() const;

  /**
   * Ostream output operator.
   *
   * Outputs: `rgba(r, g, b, a)`.
   *
   * @param os The output stream.
   * @param color The color to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const RGBA& color);
};

/// Represents an HSLA color.
struct HSLA {
  float hDeg;        ///< Hue component, in degrees [0, 360].
  float s;           ///< Saturation component, as a percentage [0, 1].
  float l;           ///< Lightness component, as a percentage [0, 1].
  uint8_t a = 0xFF;  ///< Alpha component, as uint8 [0, 255].

  /**
   * Constructor, initializes to the given HSLA values.
   *
   * @param hDeg The hue component, in degrees [0, 360].
   * @param s The saturation component, as a percentage [0, 1].
   * @param l The lightness component, as a percentage [0, 1].
   * @param a The alpha component, as a uint8 [0, 255].
   */
  constexpr HSLA(float hDeg, float s, float l, uint8_t a) : hDeg(hDeg), s(s), l(l), a(a) {}

  /**
   * Constructor, for HSL colors, which are fully opaque.
   *
   * @param hDeg The hue component, in degrees [0, 360].
   * @param s The saturation component, as a percentage [0, 1].
   * @param l The lightness component, as a percentage [0, 1].
   */
  static constexpr HSLA HSL(float hDeg, float s, float l) { return {hDeg, s, l, 0xFF}; }

  /// Convert the color to an RGBA color.
  RGBA toRGBA() const;

  /// Equality operator.
  bool operator==(const HSLA&) const = default;

  /**
   * Ostream output operator.
   *
   * Outputs: `hsla(h, s, l, a)`.
   *
   * @param os The output stream.
   * @param color The color to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const HSLA& color);
};

/// Identifies authored color spaces preserved prior to conversion to RGBA.
enum class ColorSpaceId {
  kSRGB,
  kSRGBLinear,
  kDisplayP3,
  kA98Rgb,
  kProPhotoRgb,
  kRec2020,
  kXyzD65,
  kXyzD50,
  kHwb,
  kLab,
  kLch,
  kOklab,
  kOklch,
};

/// Parse a color space name into a \ref ColorSpaceId.
std::optional<ColorSpaceId> ColorSpaceIdFromString(std::string_view name);

/// Represents a color stored in its authored color space.
struct ColorSpaceValue {
  ColorSpaceId id = ColorSpaceId::kSRGB;
  double c1 = 0.0;
  double c2 = 0.0;
  double c3 = 0.0;
  uint8_t alpha = 0xFF;

  bool operator==(const ColorSpaceValue&) const = default;
};

/**
 * Represents a CSS color value, like a \ref RGBA color from a `#rrggbb` or `#rgb` hex value, or the
 * `currentcolor` keyword.
 *
 * Colors are parsed using \ref donner::css::parser::ColorParser.
 *
 * Note that non-RGB colors, such as HSL are not yet supported, see bug
 * https://github.com/jwmcglynn/donner/issues/6.
 */
struct Color {
  /// Represents the `currentColor` keyword.
  struct CurrentColor {
    /// Equality operator.
    bool operator==(const CurrentColor&) const = default;

    /// Ostream output operator.
    friend std::ostream& operator<<(std::ostream& os, const CurrentColor&) {
      return os << "currentColor";
    }
  };

  /// A variant for supported color types.
  using Type = std::variant<RGBA, CurrentColor, HSLA, ColorSpaceValue>;

  /// The color value.
  Type value;

  /**
   * Construct a new color object from a supported color type.
   *
   * For example:
   * ```
   * Color(RGBA::RGB(0xFF, 0x00, 0x00));
   * ```
   *
   * @param value The color value.
   */
  /* implicit */ constexpr Color(Type value) : value(value) {}

  /// Equality operator.
  bool operator==(const Color& other) const { return value == other.value; }

  /// Equality operator for Color == RGBA.
  bool operator==(const RGBA& other) const {
    return std::holds_alternative<RGBA>(value) && std::get<RGBA>(value) == other;
  }

  /// Equality operator RGBA == Color.
  friend bool operator==(const RGBA& lhs, const Color& rhs) { return rhs == lhs; }

  bool operator==(const HSLA& other) const {
    return std::holds_alternative<HSLA>(value) && std::get<HSLA>(value) == other;
  }
  bool operator==(const CurrentColor& other) const {
    return std::holds_alternative<CurrentColor>(value);
  }

  /**
   * Parse a named color, such as `red` or `steelblue`.
   *
   * All colors on the CSS named color list are supported,
   * https://www.w3.org/TR/css-color-4/#named-colors, plus two special colors, `transparent` and
   * `currentcolor`.
   *
   * @param name The color name, such as `red` or `steelblue`.
   * @return The parsed color, or `std::nullopt` if the color could not be parsed.
   */
  static std::optional<Color> ByName(std::string_view name);

  /// Returns true if the color is `currentcolor`.
  bool isCurrentColor() const { return std::holds_alternative<CurrentColor>(value); }

  /// Returns true if the color is an RGBA color.
  bool hasRGBA() const { return std::holds_alternative<RGBA>(value); }

  /**
   * Returns the RGBA color value, if this object stores an RGBA color.
   *
   * @pre `hasRGBA()` returns true.
   */
  RGBA rgba() const { return std::get<RGBA>(value); }

  /// Returns true if the color is an HSLA color.
  bool hasHSLA() const { return std::holds_alternative<HSLA>(value); }

  /**
   * Returns the HSLA color value, if this object stores an HSLA color.
   *
   * @pre `hasHSLA()` returns true.
   */
  HSLA hsla() const { return std::get<HSLA>(value); }

  /**
   * Returns the color as RGBA. Note that \ref isCurrentColor() colors cannot be converted to RGBA
   * and will assert if called.
   */
  RGBA asRGBA() const;

  /**
   * Resolves the current value of this color to RGBA, by using the current rendering state, such as
   * the \p currentColor and \p opacity.
   *
   * @param currentColor The current color, used if this color is `currentcolor`.
   * @param opacity The current opacity, used to multiply the alpha channel.
   */
  RGBA resolve(RGBA currentColor, float opacity) const;

  /**
   * Ostream output operator.
   *
   * Example output:
   * ```
   * Color(currentColor)
   * ```
   *
   * or
   * ```
   * Color(rgba(0, 255, 128, 255))
   * ```
   *
   * @param os The output stream.
   * @param color The color to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Color& color);
};

namespace string_literals {

/**
 * String literal operator for constructing a \ref donner::css::Color from hex values.
 *
 * For example:
 * ```
 * const Color red = 0xFF0000_rgb;
 * ```
 *
 * @param value Integer representation of the color value (without alpha, 24-bits used).
 */
constexpr Color operator""_rgb(unsigned long long value) {
  return Color(RGBA::RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF));
}

/**
 * String literal operator for constructing a \ref donner::css::Color from hex values, with an alpha
 * channel.
 *
 * For example, for 50% opacity red:
 * ```
 * const Color red = 0xFF000080_rgba;
 * ```
 *
 * @param value Integer representation of the color value.
 */
constexpr Color operator""_rgba(unsigned long long value) {
  return Color(RGBA((value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF));
}

}  // namespace string_literals

}  // namespace donner::css
