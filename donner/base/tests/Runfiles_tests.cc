#include "donner/base/tests/Runfiles.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace donner {

TEST(Runfiles, SameDepot) {
  const std::string location =
      Runfiles::instance().Rlocation("donner/base/tests/testdata/test.txt");

  EXPECT_TRUE(std::filesystem::exists(location)) << "File not found: " << location;
}

TEST(Runfiles, DifferentDepot) {
  const std::string location =
      Runfiles::instance().RlocationExternal("css-parsing-tests", "one_component_value.json");

  EXPECT_TRUE(std::filesystem::exists(location)) << "File not found: " << location;
}

}  // namespace donner
