#include "donner/base/fonts/CffOutlineComplexity.h"

namespace donner::fonts {

CffOutlineValidationResult ValidateCffOutlineComplexities(std::span<const uint8_t> /*table*/,
                                                          bool /*cff2*/,
                                                          std::size_t /*expectedGlyphs*/) {
  return {};
}

}  // namespace donner::fonts
