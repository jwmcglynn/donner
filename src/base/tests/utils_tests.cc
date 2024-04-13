#include <gtest/gtest.h>

#include "src/base/utils.h"

namespace donner {

TEST(Utils, ReleaseAssert) {
  UTILS_RELEASE_ASSERT(true);

  EXPECT_DEATH({ UTILS_RELEASE_ASSERT(false); }, "");
}

TEST(Utils, ReleaseAssertMsg) {
  UTILS_RELEASE_ASSERT_MSG(true, "test");

  EXPECT_DEATH({ UTILS_RELEASE_ASSERT_MSG(false, "test"); }, "test");
}

}  // namespace donner
