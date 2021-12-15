#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <range/v3/range/conversion.hpp>

#include "src/base/rc_string.h"
#include "src/base/string_utils.h"

using namespace std::string_view_literals;
using testing::ElementsAre;

namespace donner {

TEST(StringUtils, EqualsLowercase) {
  EXPECT_TRUE(StringUtils::EqualsLowercase(""sv, ""sv));
  EXPECT_TRUE(StringUtils::EqualsLowercase("heLlo"sv, "hello"sv));
  EXPECT_TRUE(StringUtils::EqualsLowercase("NONE"sv, "none"sv));
  EXPECT_TRUE(StringUtils::EqualsLowercase("test-STRING"sv, "test-string"sv));

  EXPECT_FALSE(StringUtils::EqualsLowercase("short"sv, "long string"sv));
  EXPECT_TRUE(StringUtils::EqualsLowercase("test STRING that is longer than 30 characters"sv,
                                           "test string that is longer than 30 characters"sv));

  EXPECT_FALSE(StringUtils::EqualsLowercase("test-STRING"sv, "string"sv));
  EXPECT_FALSE(StringUtils::EqualsLowercase("test-STRING"sv, "test-STRING"sv))
      << "Should return false since the argument is not lowercase.";
  EXPECT_FALSE(StringUtils::EqualsLowercase("test"sv, "invalid-length"sv));
  EXPECT_FALSE(StringUtils::EqualsLowercase("test STRING that is longer than 30 characters"sv,
                                            "other string"sv));
}

TEST(StringUtils, Equals) {
  EXPECT_TRUE(StringUtils::Equals(""sv, ""sv));
  EXPECT_TRUE(StringUtils::Equals("hello"sv, "hello"sv));

  EXPECT_FALSE(StringUtils::Equals("heLlo"sv, "hello"sv));
  EXPECT_FALSE(StringUtils::Equals("short"sv, "longer string"sv));

  EXPECT_TRUE(StringUtils::Equals("test string that is longer than 30 characters"sv,
                                  "test string that is longer than 30 characters"sv));
  EXPECT_FALSE(StringUtils::Equals("test string that is LONGER than 30 characters"sv,
                                   "test STRING that is longer than 30 characters"sv));
}

TEST(StringUtils, Equals_IgnoreCase) {
  EXPECT_TRUE(StringUtils::Equals<StringComparison::IgnoreCase>(""sv, ""sv));
  EXPECT_TRUE(StringUtils::Equals<StringComparison::IgnoreCase>("heLlo"sv, "hello"sv));
  EXPECT_TRUE(StringUtils::Equals<StringComparison::IgnoreCase>("none"sv, "NONE"sv));
  EXPECT_TRUE(StringUtils::Equals<StringComparison::IgnoreCase>("test-STRING"sv, "TEST-string"sv));
  EXPECT_TRUE(StringUtils::Equals<StringComparison::IgnoreCase>(
      "test string that is LONGER than 30 characters"sv,
      "test STRING that is longer than 30 characters"sv));

  EXPECT_FALSE(StringUtils::Equals<StringComparison::IgnoreCase>("test-STRING"sv, "string"sv));
  EXPECT_TRUE(StringUtils::Equals<StringComparison::IgnoreCase>("test-STRING"sv, "test-STRING"sv));
  EXPECT_FALSE(StringUtils::Equals<StringComparison::IgnoreCase>("test"sv, "invalid-length"sv));
  EXPECT_FALSE(StringUtils::Equals<StringComparison::IgnoreCase>(
      "test STRING that is longer than 30 characters"sv, "other string"sv));
}

TEST(StringUtils, MixedStringTypes) {
  EXPECT_TRUE(StringUtils::Equals("str"sv, std::string("str")));
  EXPECT_TRUE(StringUtils::Equals(RcString("str"), "str"sv));
}

TEST(StringUtils, StartsWith) {
  EXPECT_TRUE(StringUtils::StartsWith("hello"sv, "hello"sv));
  EXPECT_TRUE(StringUtils::StartsWith("hello"sv, "hel"sv));
  EXPECT_FALSE(StringUtils::StartsWith("HELLO"sv, "hel"sv))
      << "Comparison should be case-sensitive";

  EXPECT_FALSE(StringUtils::StartsWith("short"sv, "longer string"sv));
  EXPECT_FALSE(StringUtils::StartsWith("hello"sv, "ello"sv));
  EXPECT_FALSE(StringUtils::StartsWith(""sv, "hello"sv));

  EXPECT_TRUE(StringUtils::StartsWith(""sv, ""sv))
      << "A string always starts with the empty string";
  EXPECT_TRUE(StringUtils::StartsWith("hello"sv, ""sv))
      << "A string always starts with the empty string";
}

TEST(StringUtils, StartsWith_IgnoreCase) {
  EXPECT_TRUE(StringUtils::StartsWith<StringComparison::IgnoreCase>("Hello"sv, "hello"sv));
  EXPECT_TRUE(StringUtils::StartsWith<StringComparison::IgnoreCase>("hello"sv, "HEL"sv));
}

TEST(StringUtils, EndsWith) {
  EXPECT_TRUE(StringUtils::EndsWith("hello"sv, "hello"sv));
  EXPECT_TRUE(StringUtils::EndsWith("hello"sv, "llo"sv));
  EXPECT_FALSE(StringUtils::EndsWith("HELLO"sv, "llo"sv)) << "Comparison should be case-sensitive";

  EXPECT_FALSE(StringUtils::EndsWith("short"sv, "longer string"sv));
  EXPECT_FALSE(StringUtils::EndsWith("hello"sv, "hel"sv));
  EXPECT_FALSE(StringUtils::EndsWith(""sv, "hello"sv));

  EXPECT_TRUE(StringUtils::EndsWith(""sv, ""sv)) << "A string always ends with the empty string";
  EXPECT_TRUE(StringUtils::EndsWith("hello"sv, ""sv))
      << "A string always ends with the empty string";
}

TEST(StringUtils, EndsWith_IgnoreCase) {
  EXPECT_TRUE(StringUtils::EndsWith<StringComparison::IgnoreCase>("hellO"sv, "llo"sv));
  EXPECT_TRUE(StringUtils::EndsWith<StringComparison::IgnoreCase>("hello"sv, "ELLO"sv));
}

TEST(StringUtils, Contains) {
  EXPECT_TRUE(StringUtils::Contains("hello"sv, "ello"sv));
  EXPECT_TRUE(StringUtils::Contains("hello"sv, "ell"sv));
  EXPECT_TRUE(StringUtils::Contains("hello"sv, "ello"sv));

  EXPECT_FALSE(StringUtils::Contains("short"sv, "longer string"sv));
  EXPECT_FALSE(StringUtils::Contains("hello"sv, "HELLO"sv))
      << "Comparison should be case-sensitive";
  EXPECT_FALSE(StringUtils::Contains("hello"sv, "world"sv));

  EXPECT_TRUE(StringUtils::Contains(""sv, ""sv)) << "A string always contains the empty string";
  EXPECT_TRUE(StringUtils::Contains("hello"sv, ""sv))
      << "A string always contains the empty string";
}

TEST(StringUtils, Contains_IgnoreCase) {
  EXPECT_TRUE(StringUtils::Contains<StringComparison::IgnoreCase>("heLlo"sv, "Ello"sv));
  EXPECT_TRUE(StringUtils::Contains<StringComparison::IgnoreCase>("HELLO"sv, "ell"sv));
  EXPECT_TRUE(StringUtils::Contains<StringComparison::IgnoreCase>("hello"sv, "ELLO"sv));
}

TEST(StringUtils, Split) {
  EXPECT_THAT(StringUtils::Split("hello world"sv), ElementsAre("hello"sv, "world"sv));
  EXPECT_THAT(StringUtils::Split("the   quick  brown"sv, ' '),
              ElementsAre("the"sv, "quick"sv, "brown"));

  // Test the comma separator.
  EXPECT_THAT(StringUtils::Split("fox,jumped"sv, ','), ElementsAre("fox"sv, "jumped"sv));

  // Nothing to split.
  EXPECT_THAT(StringUtils::Split(""sv), ElementsAre()) << "Empty string generates one entry";
  EXPECT_THAT(StringUtils::Split("    "sv), ElementsAre())
      << "Nothing to split generates one entry";

  {
    // Range-based for loop with a string.

    // The input string must be kept alive, since Split returns string views that index into the
    // original string.
    const std::string input("test string please ignore");
    std::vector<std::string_view> result;
    for (const auto& str : StringUtils::Split(input)) {
      result.push_back(str);
    }

    EXPECT_THAT(result, ElementsAre("test"sv, "string"sv, "please"sv, "ignore"sv));
  }

  {
    // Range-based for loop with a string_view.
    std::vector<std::string_view> result;
    for (const auto& str : StringUtils::Split("test data is hard"sv)) {
      result.push_back(str);
    }

    EXPECT_THAT(result, ElementsAre("test"sv, "data"sv, "is"sv, "hard"sv));
  }
}

}  // namespace donner
