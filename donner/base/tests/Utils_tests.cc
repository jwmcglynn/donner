#include "donner/base/Utils.h"

#include <gtest/gtest.h>

namespace donner {

TEST(Utils, ReleaseAssert) {
  UTILS_RELEASE_ASSERT(true);

#ifdef NDEBUG
  EXPECT_EXIT(UTILS_RELEASE_ASSERT(false), testing::KilledBySignal(SIGABRT), "");
#endif

  EXPECT_DEATH({ UTILS_RELEASE_ASSERT(false); }, "");
}

TEST(Utils, ReleaseAssertMsg) {
  UTILS_RELEASE_ASSERT_MSG(true, "test");

#ifdef NDEBUG
  EXPECT_EXIT(UTILS_RELEASE_ASSERT_MSG(false, "test"), testing::KilledBySignal(SIGABRT), "test");
#endif

  EXPECT_DEATH({ UTILS_RELEASE_ASSERT_MSG(false, "test"); }, "test");
}

}  // namespace donner
