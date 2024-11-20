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

}  // namespace donner::svg::parser
