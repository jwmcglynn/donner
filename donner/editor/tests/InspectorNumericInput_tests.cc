#include "donner/editor/InspectorNumericInput.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>

namespace donner::editor {
namespace {

using ::testing::DoubleEq;
using ::testing::Eq;
using ::testing::Optional;

TEST(InspectorNumericInputTest, LengthKeepsTheAuthoredUnitForABareNumber) {
  EXPECT_THAT(ParseInspectorLength("3", LengthUnit::In), Optional(Eq(Lengthd(3, LengthUnit::In))));
  EXPECT_THAT(ParseInspectorLength(" 2.5 ", LengthUnit::None),
              Optional(Eq(Lengthd(2.5, LengthUnit::None))));
}

TEST(InspectorNumericInputTest, LengthAcceptsEveryInspectorUnit) {
  EXPECT_THAT(ParseInspectorLength("4px", LengthUnit::Em),
              Optional(Eq(Lengthd(4, LengthUnit::Px))));
  EXPECT_THAT(ParseInspectorLength("1in", LengthUnit::None),
              Optional(Eq(Lengthd(1, LengthUnit::In))));
  EXPECT_THAT(ParseInspectorLength("2cm", LengthUnit::None),
              Optional(Eq(Lengthd(2, LengthUnit::Cm))));
  EXPECT_THAT(ParseInspectorLength("2mm", LengthUnit::None),
              Optional(Eq(Lengthd(2, LengthUnit::Mm))));
  EXPECT_THAT(ParseInspectorLength("12pt", LengthUnit::None),
              Optional(Eq(Lengthd(12, LengthUnit::Pt))));
  EXPECT_THAT(ParseInspectorLength("1pc", LengthUnit::None),
              Optional(Eq(Lengthd(1, LengthUnit::Pc))));
  EXPECT_THAT(ParseInspectorLength("1.5em", LengthUnit::None),
              Optional(Eq(Lengthd(1.5, LengthUnit::Em))));
  EXPECT_THAT(ParseInspectorLength("2ex", LengthUnit::None),
              Optional(Eq(Lengthd(2, LengthUnit::Ex))));
  EXPECT_THAT(ParseInspectorLength("50%", LengthUnit::None),
              Optional(Eq(Lengthd(50, LengthUnit::Percent))));
  EXPECT_THAT(ParseInspectorLength("1IN", LengthUnit::None),
              Optional(Eq(Lengthd(1, LengthUnit::In))));
}

TEST(InspectorNumericInputTest, LengthRoundTripsThroughItsCssText) {
  for (const char* text : {"1in", "2.5cm", "3mm", "12pt", "1pc", "1.5em", "2ex", "50%", "7px"}) {
    const std::optional<Lengthd> parsed = ParseInspectorLength(text, LengthUnit::None);
    ASSERT_TRUE(parsed.has_value()) << text;
    EXPECT_THAT(ParseInspectorLength(std::string_view(parsed->toRcString()), LengthUnit::None),
                Optional(Eq(*parsed)))
        << text;
  }
}

TEST(InspectorNumericInputTest, LengthRejectsTextItCannotInterpret) {
  for (const char* text : {"", "  ", "in", "1 in", "1furlong", "1in2", "1..2", "--1", "1e999"}) {
    EXPECT_THAT(ParseInspectorLength(text, LengthUnit::None), Eq(std::nullopt)) << text;
  }
}

TEST(InspectorNumericInputTest, NumberRejectsUnits) {
  EXPECT_THAT(ParseInspectorNumber(" -1.5e2 "), Optional(DoubleEq(-150.0)));
  EXPECT_THAT(ParseInspectorNumber("2px"), Eq(std::nullopt));
  EXPECT_THAT(ParseInspectorNumber(""), Eq(std::nullopt));
}

TEST(InspectorNumericInputTest, DegreesAcceptOnlyTheDegreeUnit) {
  EXPECT_THAT(ParseInspectorDegrees("45"), Optional(DoubleEq(45.0)));
  EXPECT_THAT(ParseInspectorDegrees("45deg"), Optional(DoubleEq(45.0)));
  EXPECT_THAT(ParseInspectorDegrees("-30.5 DEG"), Optional(DoubleEq(-30.5)));
  EXPECT_THAT(ParseInspectorDegrees("1rad"), Eq(std::nullopt));
  EXPECT_THAT(ParseInspectorDegrees("0.25turn"), Eq(std::nullopt));
  EXPECT_THAT(ParseInspectorDegrees("deg"), Eq(std::nullopt));
}

}  // namespace
}  // namespace donner::editor
