#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/svg/parser/preserve_aspect_ratio_parser.h"
#include "src/svg/parser/tests/parse_result_test_utils.h"

namespace donner {

using Align = PreserveAspectRatio::Align;
using MeetOrSlice = PreserveAspectRatio::MeetOrSlice;

static void PrintTo(PreserveAspectRatio::Align value, std::ostream* os) {
  switch (value) {
    case PreserveAspectRatio::Align::None: *os << "Align::None"; break;
    case PreserveAspectRatio::Align::XMinYMin: *os << "Align::XMinYMin"; break;
    case PreserveAspectRatio::Align::XMidYMin: *os << "Align::XMidYMin"; break;
    case PreserveAspectRatio::Align::XMaxYMin: *os << "Align::XMaxYMin"; break;
    case PreserveAspectRatio::Align::XMinYMid: *os << "Align::XMinYMid"; break;
    case PreserveAspectRatio::Align::XMidYMid: *os << "Align::XMidYMid"; break;
    case PreserveAspectRatio::Align::XMaxYMid: *os << "Align::XMaxYMid"; break;
    case PreserveAspectRatio::Align::XMinYMax: *os << "Align::XMinYMax"; break;
    case PreserveAspectRatio::Align::XMidYMax: *os << "Align::XMidYMax"; break;
    case PreserveAspectRatio::Align::XMaxYMax: *os << "Align::XMaxYMax"; break;
  }
}

static void PrintTo(PreserveAspectRatio::MeetOrSlice value, std::ostream* os) {
  switch (value) {
    case PreserveAspectRatio::MeetOrSlice::Meet: *os << "MeetOrSlice::Meet"; break;
    case PreserveAspectRatio::MeetOrSlice::Slice: *os << "MeetOrSlice::Slice"; break;
  }
}

static void PrintTo(const PreserveAspectRatio& value, std::ostream* os) {
  *os << "PreserveAspectRatio {" << testing::PrintToString(value.align) << ", "
      << testing::PrintToString(value.meetOrSlice) << "}";
}

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

}  // namespace donner
