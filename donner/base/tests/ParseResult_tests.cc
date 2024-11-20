#include "donner/base/ParseResult.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/FileOffset.h"
#include "donner/base/tests/ParseResultTestUtils.h"

using testing::_;  // NOLINT
using testing::Not;
using testing::Optional;
using testing::StartsWith;

namespace donner::parser {

TEST(ParseResult, Value) {
  ParseResult<int> result = []() -> ParseResult<int> { return 42; }();

  EXPECT_TRUE(result.hasResult());
  EXPECT_FALSE(result.hasError());

  EXPECT_EQ(result.result(), 42);

  // Mutable accessor.
  result.result() = 43;
  EXPECT_EQ(result.result(), 43);

  // R-value reference accessor.
  const int value = std::move(result.result());  // NOLINT: Testing move
  EXPECT_EQ(value, 43);
}

TEST(ParseResult, DeathTests) {
  ParseResult<int> withResult = 42;
  EXPECT_DEATH(withResult.error(), "hasError");
  EXPECT_DEATH({ withResult.error().location = FileOffset::Offset(42); }, "hasError");
  EXPECT_DEATH({ const auto error = std::move(withResult.error()); }, "hasError");

  ParseResult<int> withError = ParseError();
  EXPECT_DEATH(withError.result(), "hasResult");
  EXPECT_DEATH({ withError.result() = 42; }, "hasResult");
  EXPECT_DEATH(
      {
        [[maybe_unused]] const int result = std::move(withError.result());  // NOLINT: Testing move
      },
      "hasResult");
}

TEST(ParseResult, Error) {
  ParseResult<int> result = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    return error;
  }();

  EXPECT_FALSE(result.hasResult());
  EXPECT_TRUE(result.hasError());

  EXPECT_EQ(result.error().reason, "Test error please ignore");
  EXPECT_EQ(result.error().location, FileOffset::Offset(0));

  // Mutable accessor.
  result.error().location = FileOffset::Offset(42);
  EXPECT_EQ(result.error().location, FileOffset::Offset(42));

  // R-value reference accessor.
  const ParseError error = std::move(result.error());
  EXPECT_EQ(error.reason, "Test error please ignore");
}

TEST(ParseResult, ResultAndError) {
  ParseResult<int> result = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    return ParseResult<int>(42, std::move(error));
  }();

  EXPECT_TRUE(result.hasResult());
  EXPECT_TRUE(result.hasError());

  EXPECT_EQ(result.result(), 42);
  EXPECT_EQ(result.error().reason, "Test error please ignore");
}

TEST(ParseResult, Map) {
  ParseResult<int> withResult = 42;
  EXPECT_THAT(std::move(withResult).map<int>([](int result) { return result * 2; }),
              ParseResultIs(84));

  ParseResult<int> withError = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    return error;
  }();

  EXPECT_THAT(std::move(withError).map<int>([](int result) { return result * 2; }),
              ParseErrorIs("Test error please ignore"));
}

TEST(ParseResult, MapError) {
  auto mapFn = [](ParseError&& error) {
    error.reason = "Updated message";
    return std::move(error);
  };

  ParseResult<int> withResult = 42;
  EXPECT_THAT(std::move(withResult).mapError<int>(mapFn), ParseResultIs(42));

  ParseResult<int> withError = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    return error;
  }();

  EXPECT_THAT(std::move(withError).mapError<int>(mapFn), ParseErrorIs("Updated message"));
}

TEST(ParseResultTestUtils, PrintTo) {
  ParseResult<int> withResult = 42;
  EXPECT_EQ(testing::PrintToString(withResult), "ParseResult { result: 42 }");

  ParseResult<int> withError = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    return error;
  }();
  EXPECT_EQ(testing::PrintToString(withError),
            "ParseResult { error: Parse error at 0:0: Test error please ignore }");

  ParseResult<int> withBoth = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    return ParseResult<int>(42, std::move(error));
  }();
  EXPECT_EQ(testing::PrintToString(withBoth),
            "ParseResult { result: 42 error: Parse error at 0:0: Test error please ignore }");
}

TEST(ParseResultTestUtils, ErrorMatchers) {
  ParseResult<int> withResult = 42;
  ParseResult<int> withError = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    error.location = FileOffset::OffsetWithLineInfo(31, FileOffset::LineInfo(1, 30));
    return error;
  }();

  std::optional<ParseError> optionalError = withError.error();

  EXPECT_THAT(withResult, NoParseError());
  EXPECT_THAT(withError, Not(NoParseError()));

  EXPECT_THAT(withError, ParseErrorIs("Test error please ignore"));
  EXPECT_THAT(withResult, Not(ParseErrorIs("Test error please ignore")));
  EXPECT_THAT(withError, ParseErrorIs(StartsWith("Test error")));
  EXPECT_THAT(optionalError, Optional(ParseErrorIs("Test error please ignore")));

  EXPECT_THAT(withError, ParseErrorPos(1, 30));
  EXPECT_THAT(withError, ParseErrorPos(_, _));
  EXPECT_THAT(withResult, Not(ParseErrorPos(_, _)));
  EXPECT_THAT(optionalError, Optional(ParseErrorPos(1, 30)));
}

TEST(ParseResultTestUtils, ResultMatchers) {
  ParseResult<int> withResult = 42;
  ParseResult<int> withError = ParseError();

  EXPECT_THAT(withResult, ParseResultIs(42));
  EXPECT_THAT(withError, Not(ParseResultIs(42)));

  EXPECT_THAT(withResult, ParseResultIs(_));
}

TEST(ParseResultTestUtils, ResultAndErrorMatcher) {
  ParseResult<int> withBoth = []() -> ParseResult<int> {
    ParseError error;
    error.reason = "Test error please ignore";
    return ParseResult<int>(42, std::move(error));
  }();

  EXPECT_THAT(withBoth, ParseResultAndError(42, ParseErrorIs("Test error please ignore")));
  EXPECT_THAT(withBoth, ParseResultAndError(_, _));

  ParseResult<int> withResult = 42;
  ParseResult<int> withError = ParseError();

  EXPECT_THAT(withResult, Not(ParseResultAndError(_, _)));
  EXPECT_THAT(withError, Not(ParseResultAndError(_, _)));
}

}  // namespace donner::parser
