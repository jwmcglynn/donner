#pragma once

namespace donner {

/**
 * A container for font information relevant for computing font-relative lengths, per
 * https://www.w3.org/TR/css-values/#font-relative-lengths.
 */
struct FontMetrics {
  // This matches the define font size of Chrome.
  double fontSize = 16.0f;      //!< "em" measurement.
  double rootFontSize = 16.0f;  //!< The font-size of the root element, "rem".
  double exUnitInEm = 0.5f;     //!< x-height measurement.
  double chUnitInEm =
      0.5f;  //!< Equal to the used advance measure of the "0" glyph in the font used to render it.

  double exUnit() const { return exUnitInEm * fontSize; }
  double chUnit() const { return chUnitInEm * fontSize; }

  static FontMetrics DefaultsWithFontSize(double fontSize) {
    FontMetrics metrics;
    metrics.fontSize = fontSize;
    metrics.rootFontSize = fontSize;
    return metrics;
  }
};

struct RelativeLengthMetrics {
  static constexpr double kDpi = 96.0;
  static constexpr double kInchesToPixels = kDpi;
  static constexpr double kPointsToPixels = kDpi / 72.0;
  static constexpr double kCmToPixels = kDpi / 2.54;
};

}  // namespace donner
