#include "donner/base/parser/LengthParser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"

using testing::Eq;
using testing::Optional;

namespace donner::parser {

MATCHER_P3(LengthResult, valueMatcher, unitMatcher, consumedChars, "") {
  return testing::ExplainMatchResult(valueMatcher, arg.length.value, result_listener) &&
         testing::ExplainMatchResult(unitMatcher, arg.length.unit, result_listener) &&
         arg.consumedChars == consumedChars;
}

TEST(LengthParser, TestHelpers) {
  LengthParser::Result result;
  result.consumedChars = 1;
  result.length.value = 2.0;
  result.length.unit = Lengthd::Unit::None;
  EXPECT_EQ(testing::PrintToString(result), "Result {2, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Percent;
  EXPECT_EQ(testing::PrintToString(result), "Result {2%, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Cm;
  EXPECT_EQ(testing::PrintToString(result), "Result {2cm, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Mm;
  EXPECT_EQ(testing::PrintToString(result), "Result {2mm, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Q;
  EXPECT_EQ(testing::PrintToString(result), "Result {2q, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::In;
  EXPECT_EQ(testing::PrintToString(result), "Result {2in, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Pc;
  EXPECT_EQ(testing::PrintToString(result), "Result {2pc, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Pt;
  EXPECT_EQ(testing::PrintToString(result), "Result {2pt, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Px;
  EXPECT_EQ(testing::PrintToString(result), "Result {2px, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Em;
  EXPECT_EQ(testing::PrintToString(result), "Result {2em, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Ex;
  EXPECT_EQ(testing::PrintToString(result), "Result {2ex, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Ch;
  EXPECT_EQ(testing::PrintToString(result), "Result {2ch, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Rem;
  EXPECT_EQ(testing::PrintToString(result), "Result {2rem, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Vw;
  EXPECT_EQ(testing::PrintToString(result), "Result {2vw, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Vh;
  EXPECT_EQ(testing::PrintToString(result), "Result {2vh, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Vmin;
  EXPECT_EQ(testing::PrintToString(result), "Result {2vmin, consumedChars: 1}");

  result.length.unit = Lengthd::Unit::Vmax;
  EXPECT_EQ(testing::PrintToString(result), "Result {2vmax, consumedChars: 1}");
}

TEST(LengthParser, Empty) {
  EXPECT_THAT(LengthParser::Parse(""),
              ParseErrorIs("Failed to parse number: Unexpected end of string"));
}

TEST(LengthParser, Zero) {
  EXPECT_THAT(LengthParser::Parse("0"), ParseResultIs(LengthResult(0, LengthUnit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0,"), ParseResultIs(LengthResult(0, LengthUnit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0 "), ParseResultIs(LengthResult(0, LengthUnit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0 asfd"), ParseResultIs(LengthResult(0, LengthUnit::None, 1)));

  EXPECT_THAT(LengthParser::Parse("0cm"), ParseResultIs(LengthResult(0, LengthUnit::Cm, 3)));
  EXPECT_THAT(LengthParser::Parse("0cm "), ParseResultIs(LengthResult(0, LengthUnit::Cm, 3)));
  EXPECT_THAT(LengthParser::Parse("0cm,"), ParseResultIs(LengthResult(0, LengthUnit::Cm, 3)));
}

TEST(LengthParser, InvalidUnit) {
  EXPECT_THAT(LengthParser::Parse("1pp"), ParseErrorIs("Invalid unit"));
  EXPECT_THAT(LengthParser::Parse("1"), ParseErrorIs("Unit expected"));
  EXPECT_THAT(LengthParser::Parse("0ia"), ParseResultIs(LengthResult(0, LengthUnit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("0pp"), ParseResultIs(LengthResult(0, LengthUnit::None, 1)));
}

TEST(LengthParser, UnitOptional) {
  LengthParser::Options options;
  options.unitOptional = true;

  EXPECT_THAT(LengthParser::Parse("1pp", options),
              ParseResultIs(LengthResult(1, LengthUnit::None, 1)));
  EXPECT_THAT(LengthParser::Parse("1", options),
              ParseResultIs(LengthResult(1, LengthUnit::None, 1)));
}

TEST(LengthParser, Units) {
  EXPECT_THAT(LengthParser::ParseUnit("%"), Optional(LengthUnit::Percent));
  EXPECT_THAT(LengthParser::ParseUnit("cm"), Optional(LengthUnit::Cm));
  EXPECT_THAT(LengthParser::ParseUnit("mm"), Optional(LengthUnit::Mm));
  EXPECT_THAT(LengthParser::ParseUnit("q"), Optional(LengthUnit::Q));
  EXPECT_THAT(LengthParser::ParseUnit("in"), Optional(LengthUnit::In));
  EXPECT_THAT(LengthParser::ParseUnit("pc"), Optional(LengthUnit::Pc));

  EXPECT_THAT(LengthParser::ParseUnit("p"), Eq(std::nullopt));
  EXPECT_THAT(LengthParser::ParseUnit("%a"), Eq(std::nullopt));
  EXPECT_THAT(LengthParser::ParseUnit("mmm"), Eq(std::nullopt));
}

TEST(LengthParser, LengthUnits) {
  EXPECT_THAT(LengthParser::Parse("123%"),
              ParseResultIs(LengthResult(123, LengthUnit::Percent, 4)));
  EXPECT_THAT(LengthParser::Parse("-4cm"), ParseResultIs(LengthResult(-4, LengthUnit::Cm, 4)));
  EXPECT_THAT(LengthParser::Parse("5mm"), ParseResultIs(LengthResult(5, LengthUnit::Mm, 3)));
  EXPECT_THAT(LengthParser::Parse("6q"), ParseResultIs(LengthResult(6, LengthUnit::Q, 2)));
  EXPECT_THAT(LengthParser::Parse("-7in"), ParseResultIs(LengthResult(-7, LengthUnit::In, 4)));
  EXPECT_THAT(LengthParser::Parse("8pc"), ParseResultIs(LengthResult(8, LengthUnit::Pc, 3)));
  EXPECT_THAT(LengthParser::Parse("-9pt"), ParseResultIs(LengthResult(-9, LengthUnit::Pt, 4)));
  EXPECT_THAT(LengthParser::Parse("10px"), ParseResultIs(LengthResult(10, LengthUnit::Px, 4)));
  EXPECT_THAT(LengthParser::Parse("-11em"), ParseResultIs(LengthResult(-11, LengthUnit::Em, 5)));
  EXPECT_THAT(LengthParser::Parse("12ex"), ParseResultIs(LengthResult(12, LengthUnit::Ex, 4)));
  EXPECT_THAT(LengthParser::Parse("-13ch"), ParseResultIs(LengthResult(-13, LengthUnit::Ch, 5)));
  EXPECT_THAT(LengthParser::Parse("14rem"), ParseResultIs(LengthResult(14, LengthUnit::Rem, 5)));
  EXPECT_THAT(LengthParser::Parse("-15vw"), ParseResultIs(LengthResult(-15, LengthUnit::Vw, 5)));
  EXPECT_THAT(LengthParser::Parse("16vh"), ParseResultIs(LengthResult(16, LengthUnit::Vh, 4)));
  EXPECT_THAT(LengthParser::Parse("-17vmin"),
              ParseResultIs(LengthResult(-17, LengthUnit::Vmin, 7)));
  EXPECT_THAT(LengthParser::Parse("18vmax"), ParseResultIs(LengthResult(18, LengthUnit::Vmax, 6)));
}

TEST(LengthParser, CaseInsensitive) {
  EXPECT_THAT(LengthParser::Parse("-4cM"), ParseResultIs(LengthResult(-4, LengthUnit::Cm, 4)));
  EXPECT_THAT(LengthParser::Parse("5MM"), ParseResultIs(LengthResult(5, LengthUnit::Mm, 3)));
  EXPECT_THAT(LengthParser::Parse("6Q"), ParseResultIs(LengthResult(6, LengthUnit::Q, 2)));
  EXPECT_THAT(LengthParser::Parse("-7in"), ParseResultIs(LengthResult(-7, LengthUnit::In, 4)));
  EXPECT_THAT(LengthParser::Parse("8Pc"), ParseResultIs(LengthResult(8, LengthUnit::Pc, 3)));
  EXPECT_THAT(LengthParser::Parse("10PX"), ParseResultIs(LengthResult(10, LengthUnit::Px, 4)));
  EXPECT_THAT(LengthParser::Parse("-17Vmin"),
              ParseResultIs(LengthResult(-17, LengthUnit::Vmin, 7)));
  EXPECT_THAT(LengthParser::Parse("18VMax"), ParseResultIs(LengthResult(18, LengthUnit::Vmax, 6)));
}

TEST(LengthParser, ExtraCharacters) {
  EXPECT_THAT(LengthParser::Parse("123%%"),
              ParseResultIs(LengthResult(123, LengthUnit::Percent, 4)));
  EXPECT_THAT(LengthParser::Parse("-4cm, asdf"),
              ParseResultIs(LengthResult(-4, LengthUnit::Cm, 4)));
  EXPECT_THAT(LengthParser::Parse("5mm "), ParseResultIs(LengthResult(5, LengthUnit::Mm, 3)));
  EXPECT_THAT(LengthParser::Parse("6q 7q"), ParseResultIs(LengthResult(6, LengthUnit::Q, 2)));
  EXPECT_THAT(LengthParser::Parse("8Pc,"), ParseResultIs(LengthResult(8, LengthUnit::Pc, 3)));
}

}  // namespace donner::parser
