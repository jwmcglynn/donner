#include "donner/base/Utf8.h"

#include <gtest/gtest.h>

#include <concepts>
#include <sstream>
#include <string>
#include <vector>

namespace donner::base {

namespace {

/// Prints an integer value as a hexadecimal string, e.g. `0x1234`.
template <std::integral T>
std::string asHex(T value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << static_cast<std::make_unsigned_t<T>>(value);
  return oss.str();
}

}  // namespace

TEST(Utf8Test, SequenceLength) {
  // Test single-byte characters (ASCII)
  for (int i = 0; i < 0x80; ++i) {
    EXPECT_EQ(Utf8::SequenceLength(char(i)), 1) << "i = " << i;
  }

  // Continuation bytes (should be invalid as leading bytes)
  for (int i = 0x80; i < 0xC0; ++i) {
    EXPECT_EQ(Utf8::SequenceLength(char(i)), 0) << "i = " << i;
  }

  // Leading bytes for 2-byte sequences
  for (int i = 0xC0; i < 0xE0; ++i) {
    EXPECT_EQ(Utf8::SequenceLength(char(i)), 2) << "i = " << i;
  }

  // Leading bytes for 3-byte sequences
  for (int i = 0xE0; i < 0xF0; ++i) {
    EXPECT_EQ(Utf8::SequenceLength(char(i)), 3) << "i = " << i;
  }

  // Leading bytes for 4-byte sequences
  for (int i = 0xF0; i < 0xF8; ++i) {
    EXPECT_EQ(Utf8::SequenceLength(char(i)), 4) << "i = " << i;
  }

  // Invalid leading bytes
  for (int i = 0xF8; i <= 0xFF; ++i) {
    EXPECT_EQ(Utf8::SequenceLength(char(i)), 0) << "i = " << i;
  }
}

TEST(Utf8Test, IsSurrogateCodepoint) {
  // Test surrogate codepoints
  for (char32_t ch = 0xD800; ch <= 0xDFFF; ++ch) {
    EXPECT_TRUE(Utf8::IsSurrogateCodepoint(ch)) << "ch = " << asHex(ch);
  }

  // Test non-surrogate codepoints
  EXPECT_FALSE(Utf8::IsSurrogateCodepoint(0xD7FF));
  EXPECT_FALSE(Utf8::IsSurrogateCodepoint(0xE000));
  EXPECT_FALSE(Utf8::IsSurrogateCodepoint(0x10FFFF));
  EXPECT_FALSE(Utf8::IsSurrogateCodepoint(0x0));
}

TEST(Utf8Test, IsValidCodepoint) {
  // Valid codepoints
  EXPECT_TRUE(Utf8::IsValidCodepoint(0x0000));
  EXPECT_TRUE(Utf8::IsValidCodepoint(0x0041));  // 'A'
  EXPECT_TRUE(Utf8::IsValidCodepoint(0x07FF));
  EXPECT_TRUE(Utf8::IsValidCodepoint(0x0800));
  EXPECT_TRUE(Utf8::IsValidCodepoint(0xFFFF));
  EXPECT_TRUE(Utf8::IsValidCodepoint(0x10000));
  EXPECT_TRUE(Utf8::IsValidCodepoint(0x10FFFF));

  // Invalid codepoints (surrogates)
  for (char32_t ch = 0xD800; ch <= 0xDFFF; ++ch) {
    EXPECT_FALSE(Utf8::IsValidCodepoint(ch)) << "ch = " << asHex(ch);
  }

  // Invalid codepoints (beyond maximum allowed)
  EXPECT_FALSE(Utf8::IsValidCodepoint(0x110000));
  EXPECT_FALSE(Utf8::IsValidCodepoint(0xFFFFFFFF));
}

TEST(Utf8Test, NextCodepoint) {
  struct TestCase {
    std::string_view input;
    char32_t expectedCodepoint;
    int expectedLength;
  };

  const std::vector<TestCase> testCases = {
      {"A", 'A', 1},
      {"\xC3\xA9", 0x00E9, 2},           // 'é'
      {"\xE2\x82\xAC", 0x20AC, 3},       // Euro sign
      {"\xF0\x9F\x98\x81", 0x1F601, 4},  // Emoji 😁

      // Invalid sequences
      {"\xF0\x28\x8C\x28", Utf8::kUnicodeReplacementCharacter, 1},
      {"\xC0\xAF", Utf8::kUnicodeReplacementCharacter, 1},
      {"\xED\xA0\x80", Utf8::kUnicodeReplacementCharacter, 1},  // Surrogate half
  };

  for (const auto& testCase : testCases) {
    auto [codepoint, length] = Utf8::NextCodepoint(testCase.input);
    EXPECT_EQ(codepoint, testCase.expectedCodepoint) << "input = " << testCase.input;
    EXPECT_EQ(length, testCase.expectedLength) << "input = " << testCase.input;
  }
}

TEST(Utf8Test, NextCodepointLenient) {
  struct TestCase {
    std::string_view input;
    char32_t expectedCodepoint;
    int expectedLength;
  };

  const std::vector<TestCase> testCases = {
      // Valid sequences
      {"A", 'A', 1},                     // ASCII
      {"\xC3\xA9", 0x00E9, 2},           // 'é'
      {"\xE2\x82\xAC", 0x20AC, 3},       // Euro sign
      {"\xF0\x9F\x98\x81", 0x1F601, 4},  // Emoji 😁

      // Empty string
      {"", Utf8::kUnicodeReplacementCharacter, 0},

      // Truncated sequences should return replacement character
      {"\xC3", Utf8::kUnicodeReplacementCharacter, 1},          // Truncated 2-byte sequence
      {"\xE2\x82", Utf8::kUnicodeReplacementCharacter, 1},      // Truncated 3-byte sequence
      {"\xF0\x9F\x98", Utf8::kUnicodeReplacementCharacter, 1},  // Truncated 4-byte sequence

      // Unlike NextCodepoint, NextCodepointLenient appears to attempt to decode invalid sequences
      {"\xC3\x28", 0x00E8, 2},           // Invalid second byte, but attempts decode
      {"\xE2\x28\xAC", 0x2A2C, 3},       // Invalid second byte, but attempts decode
      {"\xF0\x28\x98\x81", 0x28601, 4},  // Invalid second byte, but attempts decode

      // Overlong sequences
      {"\xC0\x80", 0x0000, 2},          // Overlong NULL
      {"\xE0\x80\x80", 0x0000, 3},      // Overlong NULL
      {"\xF0\x80\x80\x80", 0x0000, 4},  // Overlong NULL

      // Surrogate codepoints
      {"\xED\xA0\x80", 0xD800, 3},  // Leading surrogate
      {"\xED\xBF\xBF", 0xDFFF, 3},  // Trailing surrogate
  };

  for (const auto& testCase : testCases) {
    auto [codepoint, length] = Utf8::NextCodepointLenient(testCase.input);
    EXPECT_EQ(codepoint, testCase.expectedCodepoint)
        << "input = " << testCase.input << ", expected = " << asHex(testCase.expectedCodepoint)
        << ", got = " << asHex(codepoint);
    EXPECT_EQ(length, testCase.expectedLength)
        << "input = " << testCase.input << ", expected length = " << testCase.expectedLength
        << ", got = " << length;
  }
}

TEST(Utf8Test, Append) {
  struct TestCase {
    char32_t codepoint;
    std::string expectedOutput;
  };

  const std::vector<TestCase> testCases = {
      {0x0041, "A"},
      {0x00E9, "\xC3\xA9"},           // 'é'
      {0x20AC, "\xE2\x82\xAC"},       // Euro sign
      {0x1F601, "\xF0\x9F\x98\x81"},  // Emoji 😁
  };

  for (const auto& testCase : testCases) {
    std::string output;
    Utf8::Append(testCase.codepoint, std::back_inserter(output));
    EXPECT_EQ(output, testCase.expectedOutput) << "codepoint = " << asHex(testCase.codepoint);
  }

  // Test invalid codepoints (should assert in debug mode)
#ifndef NDEBUG
  EXPECT_DEATH(
      {
        std::string output;
        Utf8::Append(0xD800, std::back_inserter(output));  // Surrogate codepoint
      },
      "");
  EXPECT_DEATH(
      {
        std::string output;
        Utf8::Append(0x110000, std::back_inserter(output));  // Beyond max codepoint
      },
      "");
#endif
}

}  // namespace donner::base
