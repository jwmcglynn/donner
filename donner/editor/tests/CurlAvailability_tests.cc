#include <gtest/gtest.h>

#include <string>

#include "donner/editor/ResourcePolicy.h"

namespace donner::editor {
namespace {

TEST(CurlAvailabilityTest, TestOverrideAvailable) {
  CurlAvailability::TestOverride override(CurlAvailability::State::kAvailable);
  EXPECT_EQ(CurlAvailability::check(), CurlAvailability::State::kAvailable);
}

TEST(CurlAvailabilityTest, TestOverrideMissing) {
  CurlAvailability::TestOverride override(CurlAvailability::State::kMissing);
  EXPECT_EQ(CurlAvailability::check(), CurlAvailability::State::kMissing);
}

TEST(CurlAvailabilityTest, TestOverrideSequencing) {
  {
    CurlAvailability::TestOverride o1(CurlAvailability::State::kAvailable);
    EXPECT_EQ(CurlAvailability::check(), CurlAvailability::State::kAvailable);
  }
  {
    CurlAvailability::TestOverride o2(CurlAvailability::State::kMissing);
    EXPECT_EQ(CurlAvailability::check(), CurlAvailability::State::kMissing);
  }
}

TEST(CurlAvailabilityTest, InstallHintNonEmpty) {
  const std::string hint = CurlAvailability::installHint();
  EXPECT_FALSE(hint.empty());
  // Should contain "curl" somewhere.
  EXPECT_NE(hint.find("curl"), std::string::npos);
}

TEST(CurlAvailabilityTest, InstallHintPlatformSpecific) {
  const std::string hint = CurlAvailability::installHint();
#if defined(__linux__)
  EXPECT_NE(hint.find("apt"), std::string::npos);
#elif defined(__APPLE__)
  EXPECT_NE(hint.find("brew"), std::string::npos);
#else
  EXPECT_NE(hint.find("PATH"), std::string::npos);
#endif
}

}  // namespace
}  // namespace donner::editor
