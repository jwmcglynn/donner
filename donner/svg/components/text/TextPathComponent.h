#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/base/Path.h"
#include "donner/base/RcString.h"

namespace donner::svg::components {

/// Method for placing glyphs along a path in \ref xml_textPath.
enum class TextPathMethod {
  Align,    ///< Glyphs are aligned to the tangent of the path.
  Stretch,  ///< Glyphs are stretched along the tangent to match path curvature.
};

/// Which side of the path to render text in \ref xml_textPath.
enum class TextPathSide {
  Left,   ///< Text rendered on the left side (default).
  Right,  ///< Text rendered on the right side (reversed direction).
};

/// Spacing mode for \ref xml_textPath.
enum class TextPathSpacing {
  Auto,   ///< UA determines spacing.
  Exact,  ///< Exact inter-character spacing.
};

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
