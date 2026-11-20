// Includes only Utils.h, so the NDEBUG assertion macros must be self-contained.
#include "donner/base/Utils.h"

namespace donner {

namespace {

[[maybe_unused]] int ReleaseAssertNonNegative(int value) {
  UTILS_RELEASE_ASSERT(value >= 0);
  UTILS_RELEASE_ASSERT_MSG(value >= 0, "value is non-negative");
  return value;
}

}  // namespace

}  // namespace donner
