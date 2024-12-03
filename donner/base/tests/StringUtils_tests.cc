#include "donner/base/StringUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/RcString.h"

using namespace std::string_view_literals;
using testing::ElementsAre;

namespace donner {

TEST(CaseInsensitiveCharTraits, Eq) {
  EXPECT_TRUE(CaseInsensitiveCharTraits::eq('a', 'A'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::eq('A', 'a'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::eq('a', 'a'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::eq('A', 'A'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::eq('a', 'b'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::eq('b', 'a'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::eq('a', 'B'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::eq('B', 'a'));
}

TEST(CaseInsensitiveCharTraits, Ne) {
  EXPECT_FALSE(CaseInsensitiveCharTraits::ne('a', 'A'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::ne('A', 'a'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::ne('a', 'a'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::ne('A', 'A'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::ne('a', 'b'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::ne('b', 'a'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::ne('a', 'B'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::ne('B', 'a'));
}

TEST(CaseInsensitiveCharTraits, Lt) {
  EXPECT_FALSE(CaseInsensitiveCharTraits::lt('a', 'A'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::lt('A', 'a'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::lt('a', 'a'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::lt('A', 'A'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::lt('a', 'b'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::lt('b', 'a'));
  EXPECT_TRUE(CaseInsensitiveCharTraits::lt('a', 'B'));
  EXPECT_FALSE(CaseInsensitiveCharTraits::lt('B', 'a'));
}

TEST(CaseInsensitiveCharTraits, Compare) {
  EXPECT_EQ(CaseInsensitiveCharTraits::compare("abc", "ABC", 3), 0);
  EXPECT_EQ(CaseInsensitiveCharTraits::compare("AbC", "aBc", 3), 0);
  EXPECT_EQ(CaseInsensitiveCharTraits::compare("abc", "abd", 3), -1);
  EXPECT_EQ(CaseInsensitiveCharTraits::compare("xyz", "abc", 3), 1);
}

TEST(CaseInsensitiveCharTraits, Find) {
  const char* kStr = "aBc";

  EXPECT_EQ(CaseInsensitiveCharTraits::find(kStr, 3, 'a'), kStr);
  EXPECT_EQ(CaseInsensitiveCharTraits::find(kStr, 3, 'A'), kStr);
  EXPECT_EQ(CaseInsensitiveCharTraits::find(kStr, 3, 'b'), kStr + 1);
  EXPECT_EQ(CaseInsensitiveCharTraits::find(kStr, 3, 'B'), kStr + 1);
  EXPECT_EQ(CaseInsensitiveCharTraits::find(kStr, 3, 'd'), nullptr);
}

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

TEST(StringUtils, EqualsIgnoreCase) {
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

TEST(StringUtils, StartsWithIgnoreCase) {
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

TEST(StringUtils, EndsWithIgnoreCase) {
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

TEST(StringUtils, ContainsIgnoreCase) {
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

TEST(StringUtils, Find) {
  EXPECT_EQ(StringUtils::Find("hello world"sv, "world"sv), 6u);
  EXPECT_EQ(StringUtils::Find("hello"sv, "ell"sv), 1u);
  EXPECT_EQ(StringUtils::Find("hello"sv, "hello"sv), 0u);

  EXPECT_EQ(StringUtils::Find("short"sv, "longer string"sv), std::string_view::npos)
      << "Should return npos when search string is longer than source";
  EXPECT_EQ(StringUtils::Find("hello"sv, "HELLO"sv), std::string_view::npos)
      << "Comparison should be case-sensitive";
  EXPECT_EQ(StringUtils::Find("hello"sv, "world"sv), std::string_view::npos)
      << "Should return npos when substring is not found";

  EXPECT_EQ(StringUtils::Find(""sv, ""sv), 0u)
      << "Empty string is found at position 0 in empty string";
  EXPECT_EQ(StringUtils::Find("hello"sv, ""sv), 0u)
      << "Empty string is found at position 0 in any string";
  EXPECT_EQ(StringUtils::Find(""sv, "hello"sv), std::string_view::npos)
      << "Non-empty string is not found in empty string";
}

TEST(StringUtils, FindIgnoreCase) {
  EXPECT_EQ(StringUtils::Find<StringComparison::IgnoreCase>("heLlo woRLD"sv, "WORLD"sv), 6u)
      << "Should find substring regardless of case";
  EXPECT_EQ(StringUtils::Find<StringComparison::IgnoreCase>("HELLO"sv, "ell"sv), 1u)
      << "Should find lowercase in uppercase string";
  EXPECT_EQ(StringUtils::Find<StringComparison::IgnoreCase>("hello"sv, "ELLO"sv), 1u)
      << "Should find uppercase in lowercase string";

  EXPECT_EQ(StringUtils::Find<StringComparison::IgnoreCase>("test"sv, "invalid-length"sv),
            std::string_view::npos)
      << "Should return npos when search string is longer than source";

  EXPECT_EQ(StringUtils::Find<StringComparison::IgnoreCase>(""sv, ""sv), 0u)
      << "Empty string is found at position 0 in empty string";
  EXPECT_EQ(StringUtils::Find<StringComparison::IgnoreCase>("HeLLo"sv, ""sv), 0u)
      << "Empty string is found at position 0 in any string";
}

TEST(StringUtils, FindMixedStringTypes) {
  EXPECT_EQ(StringUtils::Find("test string"sv, std::string("string")), 5u)
      << "Should work with std::string argument";
  EXPECT_EQ(StringUtils::Find(RcString("test string"), "string"sv), 5u)
      << "Should work with RcString source";
  EXPECT_EQ(StringUtils::Find(std::string("test string"), RcString("string")), 5u)
      << "Should work with mixed string types";
}

TEST(StringUtils, TrimWhitespace) {
  EXPECT_EQ(StringUtils::TrimWhitespace(""sv), ""sv);
  EXPECT_EQ(StringUtils::TrimWhitespace(" "sv), ""sv);
  EXPECT_EQ(StringUtils::TrimWhitespace("  "sv), ""sv);
  EXPECT_EQ(StringUtils::TrimWhitespace("  \t\n\r\v\f  "sv), ""sv);
  EXPECT_EQ(StringUtils::TrimWhitespace("  \t\n\r\v\f  hello world  \t\n\r\v\f  "sv),
            "hello world"sv);
}

// Test multiple occurrences for Contains
TEST(StringUtils, ContainsMultipleOccurrences) {
  EXPECT_TRUE(StringUtils::Contains("hello hello world"sv, "hello"sv))
      << "Should find substring when it appears multiple times";
  EXPECT_TRUE(StringUtils::Contains<StringComparison::IgnoreCase>("HELLO hello HELLO"sv, "hello"sv))
      << "Should find case-insensitive substring when it appears multiple times";
}

// Test overlapping matches for Contains
TEST(StringUtils, ContainsOverlapping) {
  EXPECT_TRUE(StringUtils::Contains("aaaaa"sv, "aaa"sv))
      << "Should find overlapping substring matches";
}

// Test with non-ASCII characters
TEST(StringUtils, NonAsciiCharacters) {
  EXPECT_TRUE(StringUtils::Equals("über"sv, "über"sv));
  EXPECT_TRUE(StringUtils::StartsWith("über"sv, "üb"sv));
  EXPECT_TRUE(StringUtils::EndsWith("über"sv, "er"sv));
  EXPECT_TRUE(StringUtils::Contains("über"sv, "be"sv));

  // Case insensitive tests with non-ASCII might be platform dependent
  // due to locale handling differences, but basic ASCII case folding should work
  EXPECT_TRUE(StringUtils::Contains<StringComparison::IgnoreCase>("über"sv, "BER"sv));
}

// Test consecutive separators in Split
TEST(StringUtils, SplitConsecutiveSeparators) {
  EXPECT_THAT(StringUtils::Split("a,,b,,,c"sv, ','), ElementsAre("a"sv, "b"sv, "c"sv))
      << "Should handle consecutive separators";

  EXPECT_THAT(StringUtils::Split(",a,b,c,"sv, ','), ElementsAre("a"sv, "b"sv, "c"sv))
      << "Should handle leading and trailing separators";
}

// Test TrimWhitespace with mixed whitespace
TEST(StringUtils, TrimWhitespaceMixed) {
  EXPECT_EQ(StringUtils::TrimWhitespace("\t \n hello \r\n"sv), "hello"sv)
      << "Should trim mixed whitespace characters";

  EXPECT_EQ(StringUtils::TrimWhitespace("hello\t"sv), "hello"sv)
      << "Should trim single whitespace character";

  EXPECT_EQ(StringUtils::TrimWhitespace(" hello world "sv), "hello world"sv)
      << "Should preserve internal spaces while trimming edges";
}

// Test with string length edge cases
TEST(StringUtils, StringLengthEdgeCases) {
  // Create a string that's longer than any likely small-string optimization
  std::string longString(1000, 'a');  // 1000 'a' characters

  EXPECT_TRUE(StringUtils::Contains(longString, "aaa"sv)) << "Should handle long strings";

  EXPECT_TRUE(StringUtils::StartsWith(longString, "aaa"sv))
      << "Should handle long strings in StartsWith";

  EXPECT_TRUE(StringUtils::EndsWith(longString, "aaa"sv))
      << "Should handle long strings in EndsWith";
}

// Test EqualsLowercase with more edge cases
TEST(StringUtils, EqualsLowercaseEdgeCases) {
  EXPECT_FALSE(StringUtils::EqualsLowercase("hello123"sv, "HELLO123"sv))
      << "Should not match when second string contains uppercase";

  EXPECT_TRUE(StringUtils::EqualsLowercase("123"sv, "123"sv)) << "Should match numeric strings";

  EXPECT_TRUE(StringUtils::EqualsLowercase("hello!@#"sv, "hello!@#"sv))
      << "Should match special characters";
}

}  // namespace donner
