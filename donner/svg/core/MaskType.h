#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * CSS `mask-type` property values.
 *
 * @see https://drafts.fxtf.org/css-masking-1/#the-mask-type
 */
enum class MaskType : uint8_t {
  Luminance,  ///< [DEFAULT] Derive mask coverage from luminance multiplied by alpha.
  Alpha,      ///< Derive mask coverage from the alpha channel only.
};

/// Ostream output operator for \ref MaskType.
inline std::ostream& operator<<(std::ostream& os, MaskType value) {
  switch (value) {
    case MaskType::Luminance: return os << "luminance";
    case MaskType::Alpha: return os << "alpha";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
