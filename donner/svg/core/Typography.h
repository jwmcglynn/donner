#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <ostream>

#include "donner/base/Length.h"
#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Font slant style used by the `font-style` property.
 */
enum class FontStyle : uint8_t {
  Normal,  ///< Upright glyphs.
  Italic,  ///< Italic glyphs.
  Oblique  ///< Oblique glyphs.
};

inline std::ostream& operator<<(std::ostream& os, FontStyle style) {
  switch (style) {
    case FontStyle::Normal: return os << "normal";
    case FontStyle::Italic: return os << "italic";
    case FontStyle::Oblique: return os << "oblique";
  }

  UTILS_UNREACHABLE();
}

/**
 * Font weight used by the `font-weight` property. Supports numeric weights as well as relative
 * keywords such as `bolder` and `lighter`.
 */
struct FontWeight {
  /// Kind of font weight that was specified.
  enum class Kind : uint8_t {
    Normal,   ///< `normal`, equivalent to 400.
    Bold,     ///< `bold`, equivalent to 700.
    Lighter,  ///< `lighter`, resolved relative to the parent during layout.
    Bolder,   ///< `bolder`, resolved relative to the parent during layout.
    Number    ///< Numeric weight between 1 and 1000.
  };

  /// Weight kind.
  Kind kind = Kind::Normal;
  /// Numeric weight value, valid when \ref kind is \ref Kind::Number.
  int value = 400;

  /// Equality operator.
  bool operator==(const FontWeight&) const = default;

  /// Factory for `normal`.
  static FontWeight Normal() { return FontWeight{Kind::Normal, 400}; }
  /// Factory for `bold`.
  static FontWeight Bold() { return FontWeight{Kind::Bold, 700}; }
  /// Factory for numeric weights.
  static FontWeight Number(int weight) { return FontWeight{Kind::Number, weight}; }
  /// Factory for `bolder`.
  static FontWeight Bolder() { return FontWeight{Kind::Bolder, 400}; }
  /// Factory for `lighter`.
  static FontWeight Lighter() { return FontWeight{Kind::Lighter, 400}; }
};

inline std::ostream& operator<<(std::ostream& os, const FontWeight& weight) {
  switch (weight.kind) {
    case FontWeight::Kind::Normal: return os << "normal";
    case FontWeight::Kind::Bold: return os << "bold";
    case FontWeight::Kind::Lighter: return os << "lighter";
    case FontWeight::Kind::Bolder: return os << "bolder";
    case FontWeight::Kind::Number: return os << weight.value;
  }

  UTILS_UNREACHABLE();
}

/**
 * Font stretch keywords used by the `font-stretch` property.
 */
enum class FontStretch : uint8_t {
  UltraCondensed,
  ExtraCondensed,
  Condensed,
  SemiCondensed,
  Normal,
  SemiExpanded,
  Expanded,
  ExtraExpanded,
  UltraExpanded,
};

inline std::ostream& operator<<(std::ostream& os, FontStretch stretch) {
  switch (stretch) {
    case FontStretch::UltraCondensed: return os << "ultra-condensed";
    case FontStretch::ExtraCondensed: return os << "extra-condensed";
    case FontStretch::Condensed: return os << "condensed";
    case FontStretch::SemiCondensed: return os << "semi-condensed";
    case FontStretch::Normal: return os << "normal";
    case FontStretch::SemiExpanded: return os << "semi-expanded";
    case FontStretch::Expanded: return os << "expanded";
    case FontStretch::ExtraExpanded: return os << "extra-expanded";
    case FontStretch::UltraExpanded: return os << "ultra-expanded";
  }

  UTILS_UNREACHABLE();
}

/**
 * Font variant keywords used by the `font-variant` property.
 */
enum class FontVariant : uint8_t {
  Normal,    ///< Default glyphs.
  SmallCaps  ///< Small-caps glyphs.
};

inline std::ostream& operator<<(std::ostream& os, FontVariant variant) {
  switch (variant) {
    case FontVariant::Normal: return os << "normal";
    case FontVariant::SmallCaps: return os << "small-caps";
  }

  UTILS_UNREACHABLE();
}

/**
 * Represents spacing properties such as `letter-spacing` and `word-spacing`.
 */
struct TextSpacing {
  /// Kind of spacing value specified.
  enum class Kind : uint8_t {
    Normal,  ///< The keyword `normal`.
    Length   ///< A concrete length or percentage value.
  };

  /// Spacing kind.
  Kind kind = Kind::Normal;
  /// Length value when \ref kind is \ref Kind::Length.
  std::optional<Lengthd> length;

  /// Equality operator.
  bool operator==(const TextSpacing&) const = default;

  /// Factory for `normal`.
  static TextSpacing Normal() { return TextSpacing{Kind::Normal, std::nullopt}; }
  /// Factory for concrete lengths.
  static TextSpacing Length(Lengthd value) { return TextSpacing{Kind::Length, value}; }
};

inline std::ostream& operator<<(std::ostream& os, const TextSpacing& spacing) {
  switch (spacing.kind) {
    case TextSpacing::Kind::Normal: return os << "normal";
    case TextSpacing::Kind::Length:
      if (spacing.length.has_value()) {
        return os << spacing.length.value();
      }
      return os << "0";
  }

  UTILS_UNREACHABLE();
}

/**
 * Text anchor alignment for the `text-anchor` property.
 */
enum class TextAnchor : uint8_t { Start, Middle, End };

inline std::ostream& operator<<(std::ostream& os, TextAnchor anchor) {
  switch (anchor) {
    case TextAnchor::Start: return os << "start";
    case TextAnchor::Middle: return os << "middle";
    case TextAnchor::End: return os << "end";
  }

  UTILS_UNREACHABLE();
}

/**
 * White-space handling for text content.
 */
enum class WhiteSpace : uint8_t {
  Normal,
  Pre,
  NoWrap,
  PreWrap,
  PreLine,
  BreakSpaces,
};

inline std::ostream& operator<<(std::ostream& os, WhiteSpace value) {
  switch (value) {
    case WhiteSpace::Normal: return os << "normal";
    case WhiteSpace::Pre: return os << "pre";
    case WhiteSpace::NoWrap: return os << "nowrap";
    case WhiteSpace::PreWrap: return os << "pre-wrap";
    case WhiteSpace::PreLine: return os << "pre-line";
    case WhiteSpace::BreakSpaces: return os << "break-spaces";
  }

  UTILS_UNREACHABLE();
}

/**
 * Text directionality.
 */
enum class Direction : uint8_t { Ltr, Rtl };

inline std::ostream& operator<<(std::ostream& os, Direction direction) {
  switch (direction) {
    case Direction::Ltr: return os << "ltr";
    case Direction::Rtl: return os << "rtl";
  }

  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
