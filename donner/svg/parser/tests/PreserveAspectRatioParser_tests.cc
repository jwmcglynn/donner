#include "donner/svg/parser/PreserveAspectRatioParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/ParseResultTestUtils.h"

namespace donner::svg::parser {

using Align = PreserveAspectRatio::Align;
using MeetOrSlice = PreserveAspectRatio::MeetOrSlice;

TEST(PreserveAspectRatioParser, Empty) {
  EXPECT_THAT(PreserveAspectRatioParser::Parse(""),
              ParseErrorIs("Unexpected end of string instead of align"));
}

TEST(PreserveAspectRatioParser, InvalidWhitespace) {
  EXPECT_THAT(PreserveAspectRatioParser::Parse(" "),
              ParseErrorIs("Unexpected end of string instead of align"));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none slice "),
              ParseErrorIs("End of attribute expected"));
}

TEST(PreserveAspectRatioParser, BadToken) {
  EXPECT_THAT(PreserveAspectRatioParser::Parse("noneslice"),
              ParseErrorIs("Invalid align: 'noneslice'"));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("invalid"),
              ParseErrorIs("Invalid align: 'invalid'"));
}

TEST(PreserveAspectRatioParser, None) {
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none"),
              ParseResultIs(PreserveAspectRatio{Align::None, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none meet"),
              ParseResultIs(PreserveAspectRatio{Align::None, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none slice"),
              ParseResultIs(PreserveAspectRatio{Align::None, MeetOrSlice::Slice}));
}

TEST(PreserveAspectRatioParser, AlignOnly) {
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none"),
              ParseResultIs(PreserveAspectRatio{Align::None, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none meet"),
              ParseResultIs(PreserveAspectRatio{Align::None, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none slice"),
              ParseResultIs(PreserveAspectRatio{Align::None, MeetOrSlice::Slice}));

  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMinYMin"),
              ParseResultIs(PreserveAspectRatio{Align::XMinYMin, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMidYMin"),
              ParseResultIs(PreserveAspectRatio{Align::XMidYMin, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMaxYMin"),
              ParseResultIs(PreserveAspectRatio{Align::XMaxYMin, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMinYMid"),
              ParseResultIs(PreserveAspectRatio{Align::XMinYMid, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMidYMid"),
              ParseResultIs(PreserveAspectRatio{Align::XMidYMid, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMaxYMid"),
              ParseResultIs(PreserveAspectRatio{Align::XMaxYMid, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMinYMax"),
              ParseResultIs(PreserveAspectRatio{Align::XMinYMax, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMidYMax"),
              ParseResultIs(PreserveAspectRatio{Align::XMidYMax, MeetOrSlice::Meet}));
  EXPECT_THAT(PreserveAspectRatioParser::Parse("xMaxYMax"),
              ParseResultIs(PreserveAspectRatio{Align::XMaxYMax, MeetOrSlice::Meet}));
}

TEST(PreserveAspectRatioParser, InvalidMeetOrSlice) {
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none badtoken"),
              ParseErrorIs("Invalid meetOrSlice: 'badtoken'"));
}

// ---------------------------------------------------------------------------
// Range-accuracy tests: verify that error SourceRanges cover the right span.
// ---------------------------------------------------------------------------

TEST(PreserveAspectRatioParser, RangeEmptyAlign) {
  // "" => empty align => EndOfString.
  EXPECT_THAT(PreserveAspectRatioParser::Parse(""), ParseErrorEndOfString());
  // " " => after skipping whitespace, remaining is empty => EndOfString.
  EXPECT_THAT(PreserveAspectRatioParser::Parse(" "), ParseErrorEndOfString());
}

TEST(PreserveAspectRatioParser, RangeInvalidAlign) {
  // "invalid" => rangeFrom(0) to consumed 7 => [0,7).
  EXPECT_THAT(PreserveAspectRatioParser::Parse("invalid"), ParseErrorRange(0, 7));
  // "noneslice" => rangeFrom(0) to consumed 9 => [0,9).
  EXPECT_THAT(PreserveAspectRatioParser::Parse("noneslice"), ParseErrorRange(0, 9));
}

TEST(PreserveAspectRatioParser, RangeInvalidMeetOrSlice) {
  // "none badtoken" => "none" consumed (4 chars), skip whitespace (1), "badtoken" starts at 5.
  // rangeFrom(5) to consumed 13 => [5,13).
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none badtoken"), ParseErrorRange(5, 13));
}

TEST(PreserveAspectRatioParser, RangeEndOfAttribute) {
  // "none slice " => after parsing "slice" (10 chars), remaining is " ".
  // currentRange(0,1) = [10,11).
  EXPECT_THAT(PreserveAspectRatioParser::Parse("none slice "), ParseErrorRange(10, 11));
}

}  // namespace donner::svg::parser
