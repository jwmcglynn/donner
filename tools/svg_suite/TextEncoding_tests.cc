#include "tools/svg_suite/TextEncoding.h"

#include <gtest/gtest.h>

#include <vector>

namespace donner::svg_suite {
namespace {
TEST(TextEncodingTest, AcceptsCanonicalScalarsWithoutChangingIdentity) {
  for (const std::string value :
       {"", "plain\\xff", "\xC2\xA2", "\xE2\x82\xAC", "\xF0\x9F\x8E\xA8", "\xEF\xBF\xBD", "\n\t"}) {
    EXPECT_TRUE(IsValidIdentityText(value));
    EXPECT_EQ(DiagnosticUtf8(value), value);
  }
}

TEST(TextEncodingTest, RejectsMalformedScalarsAndIdentityNul) {
  const std::vector<std::string> malformed = {"\x80",
                                              "\xFF",
                                              "\xC2",
                                              "\xE2\x82",
                                              "\xC0\x80",
                                              "\xE0\x80\xAF",
                                              "\xED\xA0\x80",
                                              "\xF4\x90\x80\x80",
                                              "\xF5\x80\x80\x80",
                                              "\xE2x",
                                              std::string("a\0b", 3)};
  for (const auto& value : malformed) {
    EXPECT_FALSE(IsValidIdentityText(value));
  }
  EXPECT_EQ(DiagnosticUtf8(std::string("a\0b", 3)), std::string("a\0b", 3));
}

TEST(TextEncodingTest, BoundsScalarsAndArgumentArraysBeforeConversion) {
  EXPECT_TRUE(IsValidIdentityText(std::string(4096, 'x'), 4096));
  EXPECT_FALSE(IsValidIdentityText(std::string(4097, 'x'), 4096));
  std::vector<std::string> arguments(16, std::string(kMaxIdentityBytes, 'x'));
  EXPECT_EQ(ArgumentBytes(arguments), kMaxArgumentBytes);
  arguments.emplace_back("x");
  EXPECT_EQ(ArgumentBytes(arguments), std::nullopt);
  EXPECT_EQ(ArgumentBytes(std::vector<std::string>(kMaxArguments + 1)), std::nullopt);
  EXPECT_EQ(ArgumentBytes(std::vector<std::string>{std::string(kMaxIdentityBytes + 1, 'x')}),
            std::nullopt);
}

TEST(TextEncodingTest, DiagnosticReplacementIsSeparateAndBounded) {
  const std::string invalid(1, char(0xFF));
  const std::string literal = "\\xff";
  const std::string replacement = "\xEF\xBF\xBD";
  EXPECT_FALSE(IsValidIdentityText(invalid));
  EXPECT_TRUE(IsValidIdentityText(literal));
  EXPECT_TRUE(IsValidIdentityText(replacement));
  EXPECT_NE(literal, replacement);
  EXPECT_EQ(DiagnosticUtf8(invalid), replacement);
  EXPECT_EQ(DiagnosticUtf8(literal), literal);
  const auto oversized = DiagnosticUtf8(std::string(kMaxDiagnosticBytes + 1, char(0xFF)));
  EXPECT_EQ(oversized.size(),
            3 * kMaxDiagnosticBytes + std::string("[diagnostic truncated]").size());
  EXPECT_TRUE(IsValidIdentityText(oversized, oversized.size()));
}

TEST(TextEncodingTest, PathConversionPreservesUnicodeAndRejectsInvalidNativeValues) {
  const std::filesystem::path unicode(std::u8string(u8"art-\u00e9-\U0001f3a8.svg"));
  EXPECT_EQ(Utf8Path(unicode), "art-\xC3\xA9-\xF0\x9F\x8E\xA8.svg");
  std::filesystem::path::string_type malformed(1,
                                               static_cast<std::filesystem::path::value_type>(0));
  EXPECT_EQ(Utf8Path(std::filesystem::path(malformed)), std::nullopt);
#if defined(_WIN32)
  EXPECT_EQ(Utf8Path(std::filesystem::path(std::wstring(1, wchar_t(0xD800)))), std::nullopt);
#else
  EXPECT_EQ(Utf8Path(std::filesystem::path(std::string(1, char(0xFF)))), std::nullopt);
#endif
}
}  // namespace
}  // namespace donner::svg_suite
