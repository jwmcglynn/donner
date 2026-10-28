#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/// Supported CSS text-transform operations. Case and kana-size transforms are not implemented.
enum class TextTransform : uint8_t { None, FullWidth };

/// Writes the CSS keyword for \p value.
inline std::ostream& operator<<(std::ostream& os, TextTransform value) {
  switch (value) {
    case TextTransform::None: return os << "none";
    case TextTransform::FullWidth: return os << "full-width";
  }
  return os;
}

}  // namespace donner::svg
