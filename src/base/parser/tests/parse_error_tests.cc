#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/parse_error.h"

namespace donner::base::parser {

TEST(ParseError, ResolveOffset) {
  {
    // Base case: Regular offset is left unchanged.
    ParseError err;
    err.location = FileOffset::Offset(1);

    EXPECT_EQ(err.location.resolveOffset("abcdef"), FileOffset::Offset(1));

    err.location = FileOffset::Offset(5);
    EXPECT_EQ(err.location.resolveOffset("abcdef"), FileOffset::Offset(5));
  }

  {
    // EndOfString() is resolved based on the input string.
    ParseError err;
    err.location = FileOffset::EndOfString();

    EXPECT_EQ(err.location.resolveOffset("abcdef"), FileOffset::Offset(6));
    EXPECT_EQ(err.location.resolveOffset("test string please ignore"), FileOffset::Offset(25));
  }
}

TEST(ParseError, Output) {
  ParseError err;
  err.reason = "Test reason";
  err.location = FileOffset::LineAndOffset(1, 2);

  EXPECT_EQ((std::ostringstream() << err).str(), "Parse error at 1:2: Test reason");
}

}  // namespace donner::base::parser
