#include "donner/svg/resources/Base64.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner::svg {

using donner::base::parser::ParseErrorIs;
using donner::base::parser::ParseResultIs;
using testing::HasSubstr;

TEST(Base64, EmptyString) {
  EXPECT_THAT(DecodeBase64Data(""), ParseResultIs(std::vector<uint8_t>{}));
}

TEST(Base64, ValidBase64) {
  EXPECT_THAT(DecodeBase64Data("TWFu"), ParseResultIs(std::vector<uint8_t>{'M', 'a', 'n'}));
  EXPECT_THAT(DecodeBase64Data("TWE="), ParseResultIs(std::vector<uint8_t>{'M', 'a'}));
  EXPECT_THAT(DecodeBase64Data("TQ=="), ParseResultIs(std::vector<uint8_t>{'M'}));
}

TEST(Base64, ValidBase64WithWhitespace) {
  EXPECT_THAT(DecodeBase64Data(" TWE= "), ParseResultIs(std::vector<uint8_t>{'M', 'a'}));
  EXPECT_THAT(DecodeBase64Data("\nTWE=\n"), ParseResultIs(std::vector<uint8_t>{'M', 'a'}));
}

TEST(Base64, InvalidCharacter) {
  EXPECT_THAT(DecodeBase64Data("TW@="), ParseErrorIs(HasSubstr("Invalid base64 char '@'")));
  EXPECT_THAT(DecodeBase64Data("TWE*"), ParseErrorIs(HasSubstr("Invalid base64 char '*'")));
}

}  // namespace donner::svg
