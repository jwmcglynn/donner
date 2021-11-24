#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/parse_error.h"

namespace donner {

TEST(ParseError, ResolveOffset) {
  ParseError err;
  err.offset = ParseError::kEndOfString;

  std::string_view sourceString = "abcdef";
  EXPECT_EQ(err.resolveOffset(sourceString), 6);
}

TEST(ParseError, Output) {
  ParseError err;
  err.reason = "Test reason";
  err.line = 1;
  err.offset = 2;

  EXPECT_EQ((std::ostringstream() << err).str(), "Parse error at 1:2: Test reason");
}

}  // namespace donner
