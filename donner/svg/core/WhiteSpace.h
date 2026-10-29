#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/// CSS white-space keywords controlling collapsing and line breaking.
enum class WhiteSpace : uint8_t { Normal, Pre, Nowrap, PreWrap, BreakSpaces, PreLine };

/// Whether spaces and tabs retain their authored widths.
constexpr bool PreservesSpaces(WhiteSpace value) {
  return value == WhiteSpace::Pre || value == WhiteSpace::PreWrap ||
         value == WhiteSpace::BreakSpaces;
}

/// Whether source segment breaks force a new line.
constexpr bool PreservesSegmentBreaks(WhiteSpace value) {
  return PreservesSpaces(value) || value == WhiteSpace::PreLine;
}

/// Whether unforced line breaking is permitted.
constexpr bool AllowsSoftWrap(WhiteSpace value) {
  return value != WhiteSpace::Pre && value != WhiteSpace::Nowrap;
}

/// Writes the CSS keyword for \p value.
inline std::ostream& operator<<(std::ostream& os, WhiteSpace value) {
  switch (value) {
    case WhiteSpace::Normal: return os << "normal";
    case WhiteSpace::Pre: return os << "pre";
    case WhiteSpace::Nowrap: return os << "nowrap";
    case WhiteSpace::PreWrap: return os << "pre-wrap";
    case WhiteSpace::BreakSpaces: return os << "break-spaces";
    case WhiteSpace::PreLine: return os << "pre-line";
  }
  return os;
}

}  // namespace donner::svg
