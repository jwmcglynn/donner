#include "donner/svg/text/TextTier.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <sstream>
#include <string>

namespace donner::svg {
namespace {

std::string ToString(TextTier tier) {
  std::ostringstream stream;
  stream << tier;
  return stream.str();
}

TEST(TextTier, OutputOperatorNamesEachTier) {
  EXPECT_EQ(ToString(TextTier::None), "TextTier::None");
  EXPECT_EQ(ToString(TextTier::Basic), "TextTier::Basic");
  EXPECT_EQ(ToString(TextTier::Full), "TextTier::Full");
}

// The test target passes the tier its configuration selects through the environment, so this
// fails if the library and the build settings disagree.
TEST(TextTier, MatchesBuildConfiguration) {
  const char* expected = std::getenv("DONNER_EXPECTED_TEXT_TIER");
  ASSERT_NE(expected, nullptr) << "DONNER_EXPECTED_TEXT_TIER must be set by the test target";
  EXPECT_EQ(ToString(BuiltTextTier()), std::string("TextTier::") + expected);
}

TEST(TextTier, PredicatesFollowTier) {
  const TextTier tier = BuiltTextTier();
  EXPECT_EQ(IsTextBuiltIn(), tier != TextTier::None) << tier;
  EXPECT_EQ(IsFullTextBuiltIn(), tier == TextTier::Full) << tier;
}

// Bazel builds WOFF2 support with the full text configuration; CMake has a separate option.
TEST(TextTier, BazelBuildsWoff2ExactlyWithFullText) {
  EXPECT_EQ(IsWoff2BuiltIn(), BuiltTextTier() == TextTier::Full) << BuiltTextTier();
}

TEST(TextTier, PrepareTextResourcesNeedsNothingWithoutText) {
  if (IsTextBuiltIn()) {
    GTEST_SKIP() << "Text is built in; donner_svg_tool_tests covers resource preparation";
  }

  std::string error = "unchanged";
  EXPECT_TRUE(PrepareTextResources(&error));
  EXPECT_EQ(error, "unchanged");
}

}  // namespace
}  // namespace donner::svg
