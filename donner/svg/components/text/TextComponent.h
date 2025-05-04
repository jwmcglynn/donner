#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/svg/core/LengthAdjust.h"

namespace donner::svg::components {

/**
 * Defines the start of a text element, which may have other text elements as children. Created on
 * each \ref xml_text, \ref xml_tspan, and \ref xml_textPath element.
 */
struct TextComponent {
  /// Text content.
  RcString text;

  /// Override for the text length. If empty, the property is not set.
  std::optional<Lengthd> textLength;

  /// How to adjust the text length, either by adding spacing between glyphs or stretching the
  /// glyphs themselves.
  LengthAdjust lengthAdjust = LengthAdjust::Default;
};

}  // namespace donner::svg::components
