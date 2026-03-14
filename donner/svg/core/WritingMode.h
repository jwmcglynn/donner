#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * CSS `writing-mode` property values, controlling the direction of text flow.
 *
 * SVG1 values (`lr-tb`, `rl-tb`, `tb-rl`, etc.) are mapped to these CSS3 equivalents
 * during parsing.
 */
enum class WritingMode : uint8_t {
  HorizontalTb,  ///< [DEFAULT] Left-to-right, top-to-bottom (horizontal text).
  VerticalRl,    ///< Top-to-bottom, right-to-left (vertical CJK style).
  VerticalLr,    ///< Top-to-bottom, left-to-right.
};

/// Returns true if the writing mode is vertical (VerticalRl or VerticalLr).
inline bool isVertical(WritingMode mode) {
  return mode == WritingMode::VerticalRl || mode == WritingMode::VerticalLr;
}

/// ostream output operator for \ref WritingMode, outputs the CSS3 value name.
inline std::ostream& operator<<(std::ostream& os, WritingMode value) {
  switch (value) {
    case WritingMode::HorizontalTb: return os << "horizontal-tb";
    case WritingMode::VerticalRl: return os << "vertical-rl";
    case WritingMode::VerticalLr: return os << "vertical-lr";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
