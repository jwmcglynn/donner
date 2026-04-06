#include "donner/base/ParseDiagnostic.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::parser {

TEST(ParseDiagnostic, ResolveOffset) {
  {
    // Base case: Regular offset is left unchanged.
    auto err = ParseDiagnostic::Error("test", FileOffset::Offset(1));

    EXPECT_EQ(err.range.start.resolveOffset("abcdef"), FileOffset::Offset(1));

    err.range.start = FileOffset::Offset(5);
    EXPECT_EQ(err.range.start.resolveOffset("abcdef"), FileOffset::Offset(5));
  }

  {
    // EndOfString() is resolved based on the input string.
    auto err = ParseDiagnostic::Error("test", FileOffset::EndOfString());

    EXPECT_EQ(err.range.start.resolveOffset("abcdef"), FileOffset::Offset(6));
    EXPECT_EQ(err.range.start.resolveOffset("test string please ignore"), FileOffset::Offset(25));
  }
}

TEST(ParseDiagnostic, Output) {
  auto err =
      ParseDiagnostic::Error("Test reason",
                             FileOffset::OffsetWithLineInfo(3, FileOffset::LineInfo(1, 2)));

  EXPECT_EQ((std::ostringstream() << err).str(), "Parse error at 1:2: Test reason");
}

}  // namespace donner::parser
