#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/// Whether the CSS font-synthesis-small-caps property permits synthetic small caps.
enum class FontSynthesisSmallCaps : uint8_t {
  Auto,  ///< Permit synthesis when the selected face lacks native small caps.
  None,  ///< Use native small caps when available, but never synthesize them.
};

/// Write the CSS keyword for the given synthesis policy.
/// @param os Output stream.
/// @param value Policy to write.
/// @return The output stream.
inline std::ostream& operator<<(std::ostream& os, FontSynthesisSmallCaps value) {
  switch (value) {
    case FontSynthesisSmallCaps::Auto: return os << "auto";
    case FontSynthesisSmallCaps::None: return os << "none";
  }
  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
