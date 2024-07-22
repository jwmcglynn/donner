#pragma once
/// @file

namespace donner {

/**
 * A container for font information relevant for computing font-relative lengths, per
 * https://www.w3.org/TR/css-values/#font-relative-lengths.
 */
struct FontMetrics {
  // This matches the default font size of Chrome.
  double fontSize = 16.0f;      //!< "em" measurement.
  double rootFontSize = 16.0f;  //!< The font-size of the root element, "rem".
  double exUnitInEm = 0.5f;     //!< x-height measurement.
  double chUnitInEm =
      0.5f;  //!< Equal to the used advance measure of the "0" glyph in the font used to render it.

  /// The value of an "ex" unit.
  double exUnit() const { return exUnitInEm * fontSize; }

  /// The value of a "ch" unit.
  double chUnit() const { return chUnitInEm * fontSize; }

  /**
   * Construct a FontMetrics with default values for a given font size.
   *
   * @param fontSize The font size to use for the default values.
   */
  static FontMetrics DefaultsWithFontSize(double fontSize) {
    FontMetrics metrics;
    metrics.fontSize = fontSize;
    metrics.rootFontSize = fontSize;
    return metrics;
  }
};

/**
 * A container with ratios for converting absolute lengths, such as "cm" or "in", see
 * https://www.w3.org/TR/css-values/#absolute-lengths.
 */
struct AbsoluteLengthMetrics {
  static constexpr double kDpi = 96.0;  ///< Hardcoded DPI for computing absolute lengths.
  static constexpr double kInchesToPixels = kDpi;         ///< 1 inch = 96 pixels.
  static constexpr double kPointsToPixels = kDpi / 72.0;  ///< 1 point = 1/72 inch.
  static constexpr double kCmToPixels = kDpi / 2.54;      ///< 1 cm = 1/2.54 inch.
};

}  // namespace donner
