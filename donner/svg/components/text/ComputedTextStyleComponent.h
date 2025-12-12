#pragma once
/// @file

#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/svg/core/Typography.h"

namespace donner::svg::components {

/**
 * Resolved typography values for a text node after the CSS cascade.
 */
struct ComputedTextStyleComponent {
  /// Requested font families in preference order.
  SmallVector<RcString, 1> fontFamily;
  /// Font slant style (normal/italic/oblique).
  FontStyle fontStyle = FontStyle::Normal;
  /// Font weight (numeric or keyword).
  FontWeight fontWeight = FontWeight::Normal();
  /// Condensed/expanded face selection.
  FontStretch fontStretch = FontStretch::Normal;
  /// Variant selection (e.g., small-caps).
  FontVariant fontVariant = FontVariant::Normal;
  /// Requested font size.
  Lengthd fontSize{16, Lengthd::Unit::Px};
  /// Glyph spacing adjustments.
  TextSpacing letterSpacing = TextSpacing::Normal();
  /// Word spacing adjustments.
  TextSpacing wordSpacing = TextSpacing::Normal();
  /// Text anchoring relative to the x/y origin.
  TextAnchor textAnchor = TextAnchor::Start;
  /// White-space collapse/wrapping behavior.
  WhiteSpace whiteSpace = WhiteSpace::Normal;
  /// Base direction for bidirectional text.
  Direction direction = Direction::Ltr;
};

}  // namespace donner::svg::components
