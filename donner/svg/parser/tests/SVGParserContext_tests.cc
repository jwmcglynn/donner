/**
 * Tests for SVGParserContext: verifies that diagnostics produced by subparsers are remapped back
 * into the coordinates of the original SVG source.
 */

#include "donner/svg/parser/details/SVGParserContext.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner::svg::parser {

namespace {

/// Three lines, so that line starts (0, 4, 9) are distinct from column offsets.
constexpr std::string_view kInput = "aaa\nbbbb\ncccccc";

/// Build a diagnostic at \p offset within a subparser's own input, with no line information,
/// which is what a subparser that only sees its own substring produces.
ParseDiagnostic SubparserError(size_t offset) {
  ParseDiagnostic error;
  error.reason = "subparser failure";
  error.range.start = FileOffset::Offset(offset);
  error.range.end = FileOffset::Offset(offset);
  return error;
}

}  // namespace

TEST(SVGParserContext, FromSubparserReportsColumnWithinLine) {
  ParseWarningSink warnings;
  SVGParser::Options options;
  SVGParserContext context(kInput, warnings, options);

  // Offset 11 is on line 3, column 2. A subparser error three characters into its input is
  // therefore at line 3, column 5, not at column 9 (the byte offset where line 3 starts).
  const ParseDiagnostic remapped =
      context.fromSubparser(SubparserError(3), ParserOrigin::StartOffset(11));

  EXPECT_THAT(remapped.range.start, ParseErrorPos(3, 5));
  EXPECT_EQ(remapped.range.start.offset, 14u);
}

TEST(SVGParserContext, FromSubparserOnFirstLine) {
  ParseWarningSink warnings;
  SVGParser::Options options;
  SVGParserContext context(kInput, warnings, options);

  const ParseDiagnostic remapped =
      context.fromSubparser(SubparserError(2), ParserOrigin::StartOffset(1));

  EXPECT_THAT(remapped.range.start, ParseErrorPos(1, 3));
  EXPECT_EQ(remapped.range.start.offset, 3u);
}

TEST(SVGParserContext, FromSubparserWithMultiLineSubparserError) {
  ParseWarningSink warnings;
  SVGParser::Options options;
  SVGParserContext context(kInput, warnings, options);

  // A subparser error on the subparser's own second line lands on the line after the origin's,
  // and keeps the subparser's own column.
  ParseDiagnostic error;
  error.reason = "subparser failure";
  error.range.start = FileOffset::OffsetWithLineInfo(6, FileOffset::LineInfo{2, 1});
  error.range.end = error.range.start;

  const ParseDiagnostic remapped =
      context.fromSubparser(std::move(error), ParserOrigin::StartOffset(11));

  EXPECT_THAT(remapped.range.start, ParseErrorPos(4, 1));
}

}  // namespace donner::svg::parser
