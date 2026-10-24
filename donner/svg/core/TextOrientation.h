#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/// CSS `text-orientation` values used in vertical writing modes.
enum class TextOrientation : uint8_t {
  Mixed,
  Upright,
  Sideways,
};

/// Writes the CSS keyword for p value.
inline std::ostream& operator<<(std::ostream& os, TextOrientation value) {
  switch (value) {
    case TextOrientation::Mixed: return os << "mixed";
    case TextOrientation::Upright: return os << "upright";
    case TextOrientation::Sideways: return os << "sideways";
  }
  return os;
}

}  // namespace donner::svg
