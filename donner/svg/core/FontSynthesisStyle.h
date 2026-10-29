#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/// CSS permission to synthesize an oblique face when a matching designed face is absent.
enum class FontSynthesisStyle : uint8_t {
  Auto,         ///< Permit the backend's existing synthetic oblique behavior.
  None,         ///< Never synthesize an oblique outline.
  ObliqueOnly,  ///< Permit synthesis for oblique requests, but not italic requests.
};

/// Write the CSS keyword for a synthesis policy.
/// @param os Output stream.
/// @param value Policy to write.
/// @return The output stream.
inline std::ostream& operator<<(std::ostream& os, FontSynthesisStyle value) {
  switch (value) {
    case FontSynthesisStyle::Auto: return os << "auto";
    case FontSynthesisStyle::None: return os << "none";
    case FontSynthesisStyle::ObliqueOnly: return os << "oblique-only";
  }
  UTILS_UNREACHABLE();
}

}  // namespace donner::svg
