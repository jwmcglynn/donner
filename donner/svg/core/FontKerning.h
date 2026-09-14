#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/// Computed CSS font-kerning policy. Auto currently enables kerning at every size.
enum class FontKerning : uint8_t {
  Auto,    ///< The user agent selects when to apply kerning.
  Normal,  ///< Apply font kerning.
  None,    ///< Suppress font kerning.
};

/// Writes the CSS keyword for a kerning policy.
inline std::ostream& operator<<(std::ostream& os, FontKerning value) {
  switch (value) {
    case FontKerning::Auto: return os << "auto";
    case FontKerning::Normal: return os << "normal";
    case FontKerning::None: return os << "none";
  }
  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
