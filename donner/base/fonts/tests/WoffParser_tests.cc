#include "donner/base/fonts/WoffParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>

#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/base/tests/Runfiles.h"

namespace donner::fonts {

namespace {

std::vector<uint8_t> LoadFile(const std::string& location) {
  std::ifstream file(location, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Failed to open file: " << location;
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

}  // namespace

TEST(WoffParser, Simple) {
  const std::string location =
      Runfiles::instance().Rlocation("donner/base/fonts/testdata/valid-001.woff");

  std::vector<uint8_t> woffData = LoadFile(location);
  ASSERT_FALSE(woffData.empty()) << "WOFF file is empty: " << location;

  auto maybeWoffFont = WoffParser::Parse(woffData);
  ASSERT_THAT(maybeWoffFont, NoParseError());
}

}  // namespace donner::fonts
