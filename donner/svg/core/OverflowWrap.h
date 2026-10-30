#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/// CSS emergency wrapping policy. BreakWord and Anywhere differ in intrinsic sizing.
enum class OverflowWrap : uint8_t { Normal, BreakWord, Anywhere };

/// Write the CSS keyword for the policy.
/// @param os Destination stream.
/// @param value Emergency wrapping policy.
inline std::ostream& operator<<(std::ostream& os, OverflowWrap value) {
  switch (value) {
    case OverflowWrap::Normal: return os << "normal";
    case OverflowWrap::BreakWord: return os << "break-word";
    case OverflowWrap::Anywhere: return os << "anywhere";
  }
  return os;
}

}  // namespace donner::svg
