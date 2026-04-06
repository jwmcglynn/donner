#include "donner/svg/tool/DonnerSvgTool.h"

#include <sstream>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::svg {
namespace {

TEST(DonnerSvgTool, HelpFlagPrintsPreviewAndReturnsZero) {
  char arg0[] = "donner-svg";
  char arg1[] = "--help";
  char* argv[] = {arg0, arg1};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(2, argv, out, err), 0);
  EXPECT_THAT(out.str(), testing::HasSubstr("--preview"));
  EXPECT_TRUE(err.str().empty());
}

TEST(DonnerSvgTool, UnknownFlagReturnsOne) {
  char arg0[] = "donner-svg";
  char arg1[] = "--bogus";
  char* argv[] = {arg0, arg1};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(2, argv, out, err), 1);
  EXPECT_THAT(err.str(), testing::HasSubstr("Unknown option: --bogus"));
}

TEST(DonnerSvgTool, PreviewFlagParsesAndThenFailsOnMissingInputFile) {
  char arg0[] = "donner-svg";
  char arg1[] = "--preview";
  char arg2[] = "does_not_exist.svg";
  char* argv[] = {arg0, arg1, arg2};

  std::ostringstream out;
  std::ostringstream err;
  EXPECT_EQ(RunDonnerSvgTool(3, argv, out, err), 2);
  EXPECT_THAT(err.str(), testing::HasSubstr("Failed to read input SVG"));
}

}  // namespace
}  // namespace donner::svg
