#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * CSS `isolation` property values. Controls whether an element creates a new stacking context.
 */
enum class Isolation : uint8_t {
  Auto,     ///< [DEFAULT] Element does not necessarily create a new stacking context.
  Isolate,  ///< Element creates a new stacking context (isolated group).
};

/// ostream output operator for \ref Isolation.
inline std::ostream& operator<<(std::ostream& os, Isolation value) {
  switch (value) {
    case Isolation::Auto: return os << "auto";
    case Isolation::Isolate: return os << "isolate";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
