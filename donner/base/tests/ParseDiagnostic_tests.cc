#include "donner/base/ParseDiagnostic.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <sstream>

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
  auto err = ParseDiagnostic::Error("Test reason",
                                    FileOffset::OffsetWithLineInfo(3, FileOffset::LineInfo(1, 2)));

  EXPECT_EQ((std::ostringstream() << err).str(), "error at 1:2: Test reason");
}

TEST(FileOffset, AddParentOffsetComposesAbsoluteOffsetAndLineInfo) {
  const FileOffset parent = FileOffset::OffsetWithLineInfo(10, FileOffset::LineInfo{3, 5});

  EXPECT_EQ(FileOffset::OffsetWithLineInfo(2, FileOffset::LineInfo{1, 4}).addParentOffset(parent),
            FileOffset::OffsetWithLineInfo(12, FileOffset::LineInfo{3, 9}));
  EXPECT_EQ(FileOffset::OffsetWithLineInfo(7, FileOffset::LineInfo{2, 1}).addParentOffset(parent),
            FileOffset::OffsetWithLineInfo(17, FileOffset::LineInfo{4, 1}));
  EXPECT_EQ(FileOffset::Offset(6).addParentOffset(parent),
            FileOffset::OffsetWithLineInfo(16, FileOffset::LineInfo{3, 11}));
  EXPECT_EQ(FileOffset::EndOfString().addParentOffset(parent),
            FileOffset::OffsetWithLineInfo(10, FileOffset::LineInfo{3, 5}));
}

TEST(FileOffset, AddParentOffsetWithoutParentLineInfoOnlyComposesAbsoluteOffset) {
  EXPECT_EQ(FileOffset::OffsetWithLineInfo(2, FileOffset::LineInfo{4, 9})
                .addParentOffset(FileOffset::Offset(10)),
            FileOffset::Offset(12));
}

TEST(FileOffset, EqualityComparesOffsetAndLineInfo) {
  EXPECT_EQ(FileOffset::OffsetWithLineInfo(5, FileOffset::LineInfo{2, 3}),
            FileOffset::OffsetWithLineInfo(5, FileOffset::LineInfo{2, 3}));
  EXPECT_NE(FileOffset::OffsetWithLineInfo(5, FileOffset::LineInfo{2, 3}),
            FileOffset::OffsetWithLineInfo(5, FileOffset::LineInfo{2, 4}));
  EXPECT_NE(FileOffset::OffsetWithLineInfo(5, FileOffset::LineInfo{2, 3}), FileOffset::Offset(5));
}

TEST(FileOffset, OutputIncludesLineInfoOffsetAndEndOfString) {
  EXPECT_EQ((std::ostringstream() << FileOffset::Offset(5)).str(), "FileOffset[offset 5]");
  EXPECT_EQ((std::ostringstream() << FileOffset::OffsetWithLineInfo(5, {2, 3})).str(),
            "FileOffset[line 2:3 offset 5]");
  EXPECT_EQ((std::ostringstream() << FileOffset::EndOfString()).str(), "FileOffset[<eos>]");
}

}  // namespace donner::parser
