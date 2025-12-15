#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "donner/base/ParseError.h"
#include "donner/base/ParseResult.h"

namespace donner::parser {
namespace {

enum class ProbeEnum {
  kAlpha = 7,
};

constexpr bool expectedPipeline() {
  std::expected<int, int> value = 3;
  const std::expected<int, int> result =
      value.and_then([](int current) { return std::expected<int, int>(current * 2); })
          .transform([](int doubled) { return doubled + 1; });

  const std::expected<int, int> failure = std::unexpected(9);

  return result && result.value() == 7 && !failure && failure.error() == 9;
}

}  // namespace

TEST(CompilerFeatureProbe, ExpectedAvailable) {
  static_assert(expectedPipeline());
  EXPECT_TRUE(expectedPipeline());
}

TEST(CompilerFeatureProbe, ToUnderlyingAvailable) {
  constexpr auto value = std::to_underlying(ProbeEnum::kAlpha);
  static_assert(value == 7);
  EXPECT_EQ(value, 7);
}

TEST(CompilerFeatureProbe, StringContainsAvailable) {
  const std::string text = "css-tokenizer";
  const std::string_view view = text;

  EXPECT_TRUE(text.contains("token"));
  EXPECT_TRUE(view.contains('-'));
  EXPECT_FALSE(text.contains("xml"));
}

TEST(CompilerFeatureProbe, ParseResultToExpectedBridge) {
  ParseResult<int> success = 11;
  const auto expectedSuccess = std::move(success).toExpected();
  EXPECT_TRUE(expectedSuccess.has_value());
  EXPECT_EQ(expectedSuccess.value(), 11);

  ParseError error;
  error.reason = "expected bridge";
  ParseResult<int> failure = ParseError(error);

  const auto expectedFailure = std::move(failure).toExpected();
  EXPECT_FALSE(expectedFailure.has_value());
  EXPECT_EQ(expectedFailure.error().reason, "expected bridge");
}

}  // namespace donner::parser
