#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/parse_error.h"

namespace donner {

TEST(ParseError, ResolveOffset) {
  {
    // Base case: Regular offset is left unchanged.
    ParseError err;
    err.offset = 1;

    EXPECT_EQ(err.resolveOffset("abcdef"), 1);

    err.offset = 5;
    EXPECT_EQ(err.resolveOffset("abcdef"), 5);
  }

  {
    // kEndOfString is resolved based on the input string.
    ParseError err;
    err.offset = ParseError::kEndOfString;

    EXPECT_EQ(err.resolveOffset("abcdef"), 6);
    EXPECT_EQ(err.resolveOffset("test string please ignore"), 25);
  }
}

TEST(ParseError, Output) {
  ParseError err;
  err.reason = "Test reason";
  err.line = 1;
  err.offset = 2;

  EXPECT_EQ((std::ostringstream() << err).str(), "Parse error at 1:2: Test reason");
}

}  // namespace donner
