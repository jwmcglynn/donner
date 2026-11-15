#include "donner/css/Serialize.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css {
namespace {

using parser::details::Tokenizer;

/// Tokenizes @p text and returns its first token, for round-trip checks.
Token FirstToken(std::string_view text) {
  Tokenizer tokenizer(text);
  return tokenizer.next();
}

TEST(Serialize, IdentifierEscapesSelectorSyntaxAndLeadingDigits) {
  EXPECT_THAT(SerializeIdentifier("abc"), testing::Eq("abc"));
  EXPECT_THAT(SerializeIdentifier("a.b"), testing::Eq("a\\.b"));
  EXPECT_THAT(SerializeIdentifier("a:b"), testing::Eq("a\\:b"));
  EXPECT_THAT(SerializeIdentifier("a b"), testing::Eq("a\\ b"));
  EXPECT_THAT(SerializeIdentifier("1a"), testing::Eq("\\31 a"));
  EXPECT_THAT(SerializeIdentifier("-1a"), testing::Eq("-\\31 a"));
  EXPECT_THAT(SerializeIdentifier("-"), testing::Eq("\\-"));
  EXPECT_THAT(SerializeIdentifier("--x_y-9"), testing::Eq("--x_y-9"));
  EXPECT_THAT(SerializeIdentifier("a\tb"), testing::Eq("a\\9 b"));
  EXPECT_THAT(SerializeIdentifier("caf\xC3\xA9"), testing::Eq("caf\xC3\xA9"));
}

TEST(Serialize, IdentifierRoundTripsThroughTheTokenizer) {
  for (const std::string_view id :
       {"a.b", "a:b", "a b", "1a", "-1a", "x)y", "he\"llo", "back\\slash", "caf\xC3\xA9"}) {
    SCOPED_TRACE(id);
    const Token token = FirstToken("#" + SerializeIdentifier(id));
    ASSERT_THAT(token.is<Token::Hash>(), testing::IsTrue()) << token;
    EXPECT_THAT(token.get<Token::Hash>().name, testing::Eq(RcString(id)));
  }
}

TEST(Serialize, IdentifierEdgeCases) {
  EXPECT_THAT(SerializeIdentifier(std::string_view("a\0b", 3)), testing::Eq("a\357\277\275b"));
  EXPECT_THAT(SerializeIdentifier("\x01 b"), testing::Eq("\\1 \\ b"));
  EXPECT_THAT(SerializeIdentifier("a\\"), testing::Eq("a\\\\"));
  EXPECT_THAT(SerializeIdentifier("7"), testing::Eq("\\37 "));
  // Bytes 0x80 and above, valid or not, are copied unchanged.
  EXPECT_THAT(SerializeIdentifier("a\377b"), testing::Eq("a\377b"));
  EXPECT_THAT(SerializeIdentifier("\xF0\x9F\x98\x80"), testing::Eq("\xF0\x9F\x98\x80"));
}

TEST(Serialize, IdentifierFollowedByWhitespaceKeepsTheCombinator) {
  for (const std::string_view id : {"7", "-7", "\x01 b", "a\\", "\xF0\x9F\x98\x80"}) {
    SCOPED_TRACE(id);
    const std::string text = "#" + SerializeIdentifier(id) + " .x";
    Tokenizer tokenizer(text);
    const Token hash = tokenizer.next();
    ASSERT_THAT(hash.is<Token::Hash>(), testing::IsTrue()) << hash;
    EXPECT_THAT(hash.get<Token::Hash>().name, testing::Eq(RcString(id)));
    EXPECT_THAT(tokenizer.next().is<Token::Whitespace>(), testing::IsTrue());
  }
}

TEST(Serialize, StringContentsEscapeOnlyTheDelimiterBackslashAndControls) {
  EXPECT_THAT(SerializeStringContents("he\"llo", '"'), testing::Eq("he\\\"llo"));
  EXPECT_THAT(SerializeStringContents("he\"llo", '\''), testing::Eq("he\"llo"));
  EXPECT_THAT(SerializeStringContents("it's", '\''), testing::Eq("it\\'s"));
  EXPECT_THAT(SerializeStringContents("back\\slash", '"'), testing::Eq("back\\\\slash"));
  EXPECT_THAT(SerializeStringContents("a\nb", '"'), testing::Eq("a\\a b"));
}

TEST(Serialize, StringContentsRoundTripThroughTheTokenizer) {
  for (const char quote : {'"', '\''}) {
    for (const std::string_view value : {"he\"llo", "it's", "back\\slash", "a\nb", "#x)y"}) {
      SCOPED_TRACE(value);
      const std::string text =
          std::string(1, quote) + SerializeStringContents(value, quote) + std::string(1, quote);
      const Token token = FirstToken(text);
      ASSERT_THAT(token.is<Token::String>(), testing::IsTrue()) << token;
      EXPECT_THAT(token.get<Token::String>().value, testing::Eq(RcString(value)));
    }
  }
}

TEST(Serialize, UrlTokenValueRoundTripsThroughTheTokenizer) {
  for (const std::string_view value : {"#a", "#a b", "#x)y", "#he\"llo", "#it's", "#back\\slash",
                                       "#a(b", "#a\nb", "#caf\xC3\xA9"}) {
    SCOPED_TRACE(value);
    const Token token = FirstToken("url(" + SerializeUrlTokenValue(value) + ")");
    ASSERT_THAT(token.is<Token::Url>(), testing::IsTrue()) << token;
    EXPECT_THAT(token.get<Token::Url>().value, testing::Eq(RcString(value)));
  }
}

}  // namespace
}  // namespace donner::css
