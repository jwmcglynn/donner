#include "donner/base/xml/XMLEscape.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/xml/XMLParser.h"

namespace donner::xml {

using testing::Eq;
using testing::Optional;

// -----------------------------------------------------------------------------
// ASCII passthrough and XML entity escapes
// -----------------------------------------------------------------------------

TEST(XMLEscape, EmptyString) {
  EXPECT_THAT(EscapeAttributeValue(""), Optional(RcString("")));
}

TEST(XMLEscape, PlainAsciiPassesThrough) {
  EXPECT_THAT(EscapeAttributeValue("hello world"), Optional(RcString("hello world")));
  EXPECT_THAT(EscapeAttributeValue("abcXYZ0123!?@#$%^*()"),
              Optional(RcString("abcXYZ0123!?@#$%^*()")));
}

TEST(XMLEscape, LessThanAndGreaterThan) {
  EXPECT_THAT(EscapeAttributeValue("<"), Optional(RcString("&lt;")));
  EXPECT_THAT(EscapeAttributeValue(">"), Optional(RcString("&gt;")));
  EXPECT_THAT(EscapeAttributeValue("a<b>c"), Optional(RcString("a&lt;b&gt;c")));
}

TEST(XMLEscape, Ampersand) {
  EXPECT_THAT(EscapeAttributeValue("&"), Optional(RcString("&amp;")));
  EXPECT_THAT(EscapeAttributeValue("a & b"), Optional(RcString("a &amp; b")));
  // Escape must not be idempotent-lossy — a pre-escaped `&amp;` becomes `&amp;amp;`.
  EXPECT_THAT(EscapeAttributeValue("&amp;"), Optional(RcString("&amp;amp;")));
}

TEST(XMLEscape, QuoteCharEscapedBasedOnDelimiter) {
  // Default quoteChar is '"', so `"` must escape but `'` passes through.
  EXPECT_THAT(EscapeAttributeValue("\""), Optional(RcString("&quot;")));
  EXPECT_THAT(EscapeAttributeValue("'"), Optional(RcString("'")));

  // Single-quote delimiter: `'` must escape, `"` passes through.
  EXPECT_THAT(EscapeAttributeValue("'", '\''), Optional(RcString("&apos;")));
  EXPECT_THAT(EscapeAttributeValue("\"", '\''), Optional(RcString("\"")));
}

TEST(XMLEscape, WhitespaceIsEscapedToNumericEntities) {
  // `\t`, `\n`, `\r` round-trip through a numeric character reference so
  // the XML attribute-value normalization does not collapse them to a
  // single space on reparse.
  EXPECT_THAT(EscapeAttributeValue("\t"), Optional(RcString("&#9;")));
  EXPECT_THAT(EscapeAttributeValue("\n"), Optional(RcString("&#10;")));
  EXPECT_THAT(EscapeAttributeValue("\r"), Optional(RcString("&#13;")));
  EXPECT_THAT(EscapeAttributeValue("line1\nline2"),
              Optional(RcString("line1&#10;line2")));
}

// -----------------------------------------------------------------------------
// Rejected inputs
// -----------------------------------------------------------------------------

TEST(XMLEscape, NulByteRejected) {
  EXPECT_THAT(EscapeAttributeValue(std::string_view("a\0b", 3)), Eq(std::nullopt));
}

TEST(XMLEscape, C0ControlCharsRejected) {
  // Every C0 character other than \t \n \r is forbidden in XML 1.0.
  for (char c = 1; c < 0x20; ++c) {
    if (c == '\t' || c == '\n' || c == '\r') continue;
    const std::string s(1, c);
    EXPECT_THAT(EscapeAttributeValue(s), Eq(std::nullopt))
        << "C0 char 0x" << std::hex << static_cast<int>(c) << " should be rejected";
  }
}

TEST(XMLEscape, InvalidUtf8Rejected) {
  // Lone continuation byte.
  EXPECT_THAT(EscapeAttributeValue("\x80"), Eq(std::nullopt));
  // Lead byte without continuation.
  EXPECT_THAT(EscapeAttributeValue("\xC2"), Eq(std::nullopt));
  // Overlong encoding of U+0041 as a 2-byte sequence (the shortest form is 1 byte).
  EXPECT_THAT(EscapeAttributeValue("\xC1\x81"), Eq(std::nullopt));
  // 5-byte sequence is never legal UTF-8.
  EXPECT_THAT(EscapeAttributeValue("\xF8\x80\x80\x80\x80"), Eq(std::nullopt));
  // Invalid bytes 0xFE / 0xFF.
  EXPECT_THAT(EscapeAttributeValue("\xFE"), Eq(std::nullopt));
  EXPECT_THAT(EscapeAttributeValue("\xFF"), Eq(std::nullopt));
}

TEST(XMLEscape, LoneSurrogatesRejected) {
  // U+D800 encoded as 3 bytes UTF-8 = 0xED 0xA0 0x80.
  EXPECT_THAT(EscapeAttributeValue("\xED\xA0\x80"), Eq(std::nullopt));
  // U+DFFF encoded as 3 bytes UTF-8 = 0xED 0xBF 0xBF.
  EXPECT_THAT(EscapeAttributeValue("\xED\xBF\xBF"), Eq(std::nullopt));
}

TEST(XMLEscape, NonCharactersRejected) {
  // U+FFFE and U+FFFF are non-characters.
  EXPECT_THAT(EscapeAttributeValue("\xEF\xBF\xBE"), Eq(std::nullopt));
  EXPECT_THAT(EscapeAttributeValue("\xEF\xBF\xBF"), Eq(std::nullopt));
  // U+FFFD (replacement character) is legal and must pass through.
  EXPECT_THAT(EscapeAttributeValue("\xEF\xBF\xBD"), Optional(RcString("\xEF\xBF\xBD")));
}

// -----------------------------------------------------------------------------
// UTF-8 passthrough
// -----------------------------------------------------------------------------

TEST(XMLEscape, ValidUtf8PassesThrough) {
  // 2-byte: é (U+00E9)
  EXPECT_THAT(EscapeAttributeValue("caf\xC3\xA9"), Optional(RcString("caf\xC3\xA9")));
  // 3-byte: 你 (U+4F60)
  EXPECT_THAT(EscapeAttributeValue("\xE4\xBD\xA0"), Optional(RcString("\xE4\xBD\xA0")));
  // 4-byte: 😀 (U+1F600)
  EXPECT_THAT(EscapeAttributeValue("\xF0\x9F\x98\x80"),
              Optional(RcString("\xF0\x9F\x98\x80")));
  // Mixed ASCII + multibyte.
  EXPECT_THAT(EscapeAttributeValue("Hello 你好!"), Optional(RcString("Hello 你好!")));
}

// -----------------------------------------------------------------------------
// Round-trip through XMLParser
// -----------------------------------------------------------------------------

TEST(XMLEscape, RoundTripThroughParser) {
  // The whole point of this function is that escaped output round-trips
  // through XMLParser::Parse to recover the original bytes. Build a tiny
  // XML document that wraps the escaped value in a `key="value"` attribute,
  // parse it, and assert the recovered attribute equals the original.
  const std::string_view cases[] = {
      "plain",
      "a<b>c&d\"e'f",
      "with & ampersand",
      "tab\there",
      "line1\nline2",
      "cr\rhere",
      "caf\xC3\xA9",        // é
      "\xE4\xBD\xA0\xE5\xA5\xBD",  // 你好
      "\xF0\x9F\x98\x80",    // 😀
      "",
  };

  for (std::string_view original : cases) {
    auto escaped = EscapeAttributeValue(original);
    ASSERT_TRUE(escaped.has_value()) << "Escape failed for " << original;

    std::string xml = "<e a=\"";
    xml.append(std::string_view(*escaped));
    xml.append("\"/>");

    auto parsed = XMLParser::Parse(xml);
    ASSERT_TRUE(parsed.hasResult()) << "Parse failed for " << xml;

    auto rootChild = parsed.result().root().firstChild();
    ASSERT_TRUE(rootChild.has_value());
    const auto attrValue = rootChild->getAttribute(XMLQualifiedNameRef("", "a"));
    ASSERT_TRUE(attrValue.has_value());
    EXPECT_EQ(std::string_view(*attrValue), original)
        << "Round-trip mismatch: escaped=" << *escaped << " xml=" << xml;
  }
}

TEST(XMLEscape, RoundTripWithSingleQuoteDelimiter) {
  // Same idea but with single-quoted attributes.
  const std::string_view cases[] = {
      "contains \" double quote",
      "plain",
      "both \" and ' quotes",
  };

  for (std::string_view original : cases) {
    auto escaped = EscapeAttributeValue(original, '\'');
    ASSERT_TRUE(escaped.has_value()) << "Escape failed for " << original;

    std::string xml = "<e a='";
    xml.append(std::string_view(*escaped));
    xml.append("'/>");

    auto parsed = XMLParser::Parse(xml);
    ASSERT_TRUE(parsed.hasResult()) << "Parse failed for " << xml;

    auto rootChild = parsed.result().root().firstChild();
    ASSERT_TRUE(rootChild.has_value());
    const auto attrValue = rootChild->getAttribute(XMLQualifiedNameRef("", "a"));
    ASSERT_TRUE(attrValue.has_value());
    EXPECT_EQ(std::string_view(*attrValue), original);
  }
}

}  // namespace donner::xml
