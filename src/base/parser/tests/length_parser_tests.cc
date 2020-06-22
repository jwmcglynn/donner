#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/length_parser.h"
#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/base/tests/base_test_utils.h"

namespace donner {

using Unit = Lengthd::Unit;

static void PrintTo(Unit value, std::ostream* os) {
  switch (value) {
    case Unit::None: *os << "Unit::None"; break;
    case Unit::Percent: *os << "Unit::Percent"; break;
    case Unit::Cm: *os << "Unit::Cm"; break;
    case Unit::Mm: *os << "Unit::Mm"; break;
    case Unit::Q: *os << "Unit::Q"; break;
    case Unit::In: *os << "Unit::In"; break;
    case Unit::Pc: *os << "Unit::Pc"; break;
    case Unit::Pt: *os << "Unit::Pt"; break;
    case Unit::Px: *os << "Unit::Px"; break;
    case Unit::Em: *os << "Unit::Em"; break;
    case Unit::Ex: *os << "Unit::Ex"; break;
    case Unit::Ch: *os << "Unit::Ch"; break;
    case Unit::Rem: *os << "Unit::Rem"; break;
    case Unit::Vw: *os << "Unit::Vw"; break;
    case Unit::Vh: *os << "Unit::Vh"; break;
    case Unit::Vmin: *os << "Unit::Vmin"; break;
    case Unit::Vmax: *os << "Unit::Vmax"; break;
  }
}

static void PrintTo(const Lengthd& value, std::ostream* os) {
  *os << "Length {" << testing::PrintToString(value.value) << ", "
      << testing::PrintToString(value.unit) << "}";
}

static void PrintTo(const LengthParser::Result& value, std::ostream* os) {
  *os << "Result {" << testing::PrintToString(value.length)
      << ", consumed_chars: " << value.consumed_chars << "}";
}

MATCHER_P3(LengthResult, valueMatcher, unitMatcher, consumedChars, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.length.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.length.unit, result_listener) &&
         arg.consumed_chars == consumedChars;
}

TEST(LengthParser, Empty) {
  EXPECT_THAT(LengthParser::Parse(""), ParseErrorIs("Failed to parse number: Invalid argument"));
}

TEST(LengthParser, Zero) {
  EXPECT_THAT(LengthParser::Parse("0"), ParseResultIs(LengthResult(0, Unit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0,"), ParseResultIs(LengthResult(0, Unit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0 "), ParseResultIs(LengthResult(0, Unit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0 asfd"), ParseResultIs(LengthResult(0, Unit::None, 1)));

  EXPECT_THAT(LengthParser::Parse("0cm"), ParseResultIs(LengthResult(0, Unit::Cm, 3)));
  EXPECT_THAT(LengthParser::Parse("0cm "), ParseResultIs(LengthResult(0, Unit::Cm, 3)));
  EXPECT_THAT(LengthParser::Parse("0cm,"), ParseResultIs(LengthResult(0, Unit::Cm, 3)));
}

TEST(LengthParser, InvalidUnit) {
  EXPECT_THAT(LengthParser::Parse("1pp"), ParseErrorIs("Invalid unit"));
  EXPECT_THAT(LengthParser::Parse("1"), ParseErrorIs("Unit expected"));
  EXPECT_THAT(LengthParser::Parse("0ia"), ParseResultIs(LengthResult(0, Unit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0pp"), ParseResultIs(LengthResult(0, Unit::None, 1)));
}

TEST(LengthParser, UnitOptional) {
  LengthParser::Options options;
  options.unit_optional = true;

  EXPECT_THAT(LengthParser::Parse("1pp", options), ParseResultIs(LengthResult(1, Unit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("1", options), ParseResultIs(LengthResult(1, Unit::None, 1)));
}

TEST(LengthParser, Units) {
  EXPECT_THAT(LengthParser::Parse("123%"), ParseResultIs(LengthResult(123, Unit::Percent, 4)));
  EXPECT_THAT(LengthParser::Parse("-4cm"), ParseResultIs(LengthResult(-4, Unit::Cm, 4)));
  EXPECT_THAT(LengthParser::Parse("5mm"), ParseResultIs(LengthResult(5, Unit::Mm, 3)));
  EXPECT_THAT(LengthParser::Parse("6q"), ParseResultIs(LengthResult(6, Unit::Q, 2)));
  EXPECT_THAT(LengthParser::Parse("-7in"), ParseResultIs(LengthResult(-7, Unit::In, 4)));
  EXPECT_THAT(LengthParser::Parse("8pc"), ParseResultIs(LengthResult(8, Unit::Pc, 3)));
  EXPECT_THAT(LengthParser::Parse("-9pt"), ParseResultIs(LengthResult(-9, Unit::Pt, 4)));
  EXPECT_THAT(LengthParser::Parse("10px"), ParseResultIs(LengthResult(10, Unit::Px, 4)));
  EXPECT_THAT(LengthParser::Parse("-11em"), ParseResultIs(LengthResult(-11, Unit::Em, 5)));
  EXPECT_THAT(LengthParser::Parse("12ex"), ParseResultIs(LengthResult(12, Unit::Ex, 4)));
  EXPECT_THAT(LengthParser::Parse("-13ch"), ParseResultIs(LengthResult(-13, Unit::Ch, 5)));
  EXPECT_THAT(LengthParser::Parse("14rem"), ParseResultIs(LengthResult(14, Unit::Rem, 5)));
  EXPECT_THAT(LengthParser::Parse("-15vw"), ParseResultIs(LengthResult(-15, Unit::Vw, 5)));
  EXPECT_THAT(LengthParser::Parse("16vh"), ParseResultIs(LengthResult(16, Unit::Vh, 4)));
  EXPECT_THAT(LengthParser::Parse("-17vmin"), ParseResultIs(LengthResult(-17, Unit::Vmin, 7)));
  EXPECT_THAT(LengthParser::Parse("18vmax"), ParseResultIs(LengthResult(18, Unit::Vmax, 6)));
}

TEST(LengthParser, CaseInsensitive) {
  EXPECT_THAT(LengthParser::Parse("-4cM"), ParseResultIs(LengthResult(-4, Unit::Cm, 4)));
  EXPECT_THAT(LengthParser::Parse("5MM"), ParseResultIs(LengthResult(5, Unit::Mm, 3)));
  EXPECT_THAT(LengthParser::Parse("6Q"), ParseResultIs(LengthResult(6, Unit::Q, 2)));
  EXPECT_THAT(LengthParser::Parse("-7in"), ParseResultIs(LengthResult(-7, Unit::In, 4)));
  EXPECT_THAT(LengthParser::Parse("8Pc"), ParseResultIs(LengthResult(8, Unit::Pc, 3)));
  EXPECT_THAT(LengthParser::Parse("10PX"), ParseResultIs(LengthResult(10, Unit::Px, 4)));
  EXPECT_THAT(LengthParser::Parse("-17Vmin"), ParseResultIs(LengthResult(-17, Unit::Vmin, 7)));
  EXPECT_THAT(LengthParser::Parse("18VMax"), ParseResultIs(LengthResult(18, Unit::Vmax, 6)));
}

TEST(LengthParser, ExtraCharacters) {
  EXPECT_THAT(LengthParser::Parse("123%%"), ParseResultIs(LengthResult(123, Unit::Percent, 4)));
  EXPECT_THAT(LengthParser::Parse("-4cm, asdf"), ParseResultIs(LengthResult(-4, Unit::Cm, 4)));
  EXPECT_THAT(LengthParser::Parse("5mm "), ParseResultIs(LengthResult(5, Unit::Mm, 3)));
  EXPECT_THAT(LengthParser::Parse("6q 7q"), ParseResultIs(LengthResult(6, Unit::Q, 2)));
  EXPECT_THAT(LengthParser::Parse("8Pc,"), ParseResultIs(LengthResult(8, Unit::Pc, 3)));
}

}  // namespace donner
