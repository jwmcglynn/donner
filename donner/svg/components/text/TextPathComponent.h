#pragma once
/// @file

#include <optional>
#include <ostream>

#include "donner/base/Length.h"
#include "donner/base/Path.h"
#include "donner/base/RcString.h"

namespace donner::svg::components {

/// Method for placing glyphs along a path in \ref xml_textPath.
enum class TextPathMethod {
  Align,    ///< Each glyph is rigidly rotated and translated onto the path tangent.
  Stretch,  ///< Glyph outlines are warped along the path. Donner places these glyphs with
            ///< \ref TextPathMethod::Align, since outline warping is not implemented.
};

/// Ostream output operator for \ref TextPathMethod.
inline std::ostream& operator<<(std::ostream& os, TextPathMethod method) {
  switch (method) {
    case TextPathMethod::Align: return os << "Align";
    case TextPathMethod::Stretch: return os << "Stretch";
  }
  return os << "Unknown";
}

/// Which side of the path to render text in \ref xml_textPath.
enum class TextPathSide {
  Left,   ///< Text rendered on the left side (default).
  Right,  ///< Text rendered on the other side, by reversing the direction of travel.
};

/// Ostream output operator for \ref TextPathSide.
inline std::ostream& operator<<(std::ostream& os, TextPathSide side) {
  switch (side) {
    case TextPathSide::Left: return os << "Left";
    case TextPathSide::Right: return os << "Right";
  }
  return os << "Unknown";
}

/// Spacing mode for \ref xml_textPath.
enum class TextPathSpacing {
  Auto,   ///< The user agent chooses the inter-glyph spacing. Donner chooses the same spacing as
          ///< \ref TextPathSpacing::Exact.
  Exact,  ///< Glyphs advance by exactly their shaped widths.
};

/// Ostream output operator for \ref TextPathSpacing.
inline std::ostream& operator<<(std::ostream& os, TextPathSpacing spacing) {
  switch (spacing) {
    case TextPathSpacing::Auto: return os << "Auto";
    case TextPathSpacing::Exact: return os << "Exact";
  }
  return os << "Unknown";
}

/**
 * Stores attributes specific to \ref xml_textPath elements.
 *
 * The `<textPath>` element renders text along a path given either inline with the `path`
 * attribute or by reference through `href`.
 */
struct TextPathComponent {
  /// Reference to a shape element (IRI fragment, e.g., "#myPath").
  RcString href;

  /// Geometry parsed from the `path` attribute, in the textPath element's own user space. Set only
  /// when the attribute parsed to at least one subpath; an unparsable value leaves this empty so
  /// that \ref href supplies the geometry instead.
  std::optional<Path> inlinePath;

  /// Offset along the path where text begins. Percentages are relative to path length.
  std::optional<Lengthd> startOffset;

  /// How glyphs are placed on the path.
  TextPathMethod method = TextPathMethod::Align;

  /// Which side of the path to render text.
  TextPathSide side = TextPathSide::Left;

  /// Spacing control.
  TextPathSpacing spacing = TextPathSpacing::Exact;
};

}  // namespace donner::svg::components
