#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner {
namespace css {

void PrintTo(const Token& token, std::ostream* os) {
  *os << "Token { ";
  token.visit([&os](auto&& value) { *os << value; });
  *os << " offset: " << token.offset();
  *os << " }";
}

TEST(Tokenizer, Empty) {
  Tokenizer tokenizer("");
  EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 0)));
}

TEST(Tokenizer, Comment) {
  {
    Tokenizer tokenizer("/**/");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 4)));
  }

  {
    Tokenizer tokenizer("/* test */");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 10)));
  }

  {
    Tokenizer tokenizer("/* test");
    EXPECT_THAT(tokenizer.next(), ParseErrorIs("Unterminated comment"));
  }

  {
    Tokenizer tokenizer("/*/");
    EXPECT_THAT(tokenizer.next(), ParseErrorIs("Unterminated comment"));
  }
}

TEST(Tokenizer, String) {
  {
    Tokenizer tokenizer("\"\"");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::String(""), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 2)));
  }

  {
    Tokenizer tokenizer("\"asdf\"");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::String("asdf"), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 6)));
  }

  {
    Tokenizer tokenizer("'test''multiple'");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::String("test"), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::String("multiple"), 6)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 16)));
  }

  // Test unterminated strings.
  EXPECT_THAT(Tokenizer("'").next(), ParseErrorIs("Unterminated string"));
  EXPECT_THAT(Tokenizer("'unterminated").next(), ParseErrorIs("Unterminated string"));

  EXPECT_THAT(Tokenizer("'skip\\\nnewline'").next(),
              ParseResultIs(Token(Token::String("skipnewline"), 0)));

  // String containing newlines are considered bad.
  {
    Tokenizer tokenizer("'newline\n");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::BadString("newline"), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 9)));
  }

  {
    Tokenizer tokenizer("'bad\n'good'");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::BadString("bad"), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::String("good"), 5)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 11)));
  }
}

TEST(Tokenizer, StringEscapedCodepoint) {
  {
    Tokenizer tokenizer("'\\D'");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::String("\r"), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 4)));
  }

  // Hex characters are converted.
  EXPECT_THAT(Tokenizer("'\\20'").next(), ParseResultIs(Token(Token::String(" "), 0)));

  // Non-hex are passed through without the slash.
  EXPECT_THAT(Tokenizer("'\\r'").next(), ParseResultIs(Token(Token::String("r"), 0)));
  EXPECT_THAT(Tokenizer("'\\\\'").next(), ParseResultIs(Token(Token::String("\\"), 0)));

  EXPECT_THAT(Tokenizer("'N\\65\\61t'").next(), ParseResultIs(Token(Token::String("Neat"), 0)));
  EXPECT_THAT(Tokenizer("'\\x\\y\\z'").next(), ParseResultIs(Token(Token::String("xyz"), 0)));

  // EOF after slash.
  EXPECT_THAT(Tokenizer("'\\").next(), ParseErrorIs("Unterminated string"));

  // Escaped ending.
  EXPECT_THAT(Tokenizer("'\\'").next(), ParseErrorIs("Unterminated string"));

  // Escaped ending with " quotes.
  EXPECT_THAT(Tokenizer("\"\\\"").next(), ParseErrorIs("Unterminated string"));

  // Escaped quote is okay.
  EXPECT_THAT(Tokenizer("'\\''").next(), ParseResultIs(Token(Token::String("'"), 0)));

  // Escaped non-matching quote is okay.
  EXPECT_THAT(Tokenizer("'\\\"'").next(), ParseResultIs(Token(Token::String("\""), 0)));

  // Validate 1 to 6 hex chars are allowed.
  EXPECT_THAT(
      Tokenizer("'\\A\\BB\\CCC\\D000\\1FB00\\100000'").next(),
      ParseResultIs(Token(Token::String("\u000A\u00BB\u0CCC\uD000\U0001FB00\U00100000"), 0)));

  // It should stop parsing after 6, the 'A' becomes a normal codepoint.
  EXPECT_THAT(Tokenizer("'\\100000A'").next(),
              ParseResultIs(Token(Token::String("\U00100000A"), 0)));

  // Whitespace at the end is skipped.
  EXPECT_THAT(Tokenizer("'\\A \\BB\r \\CCC\\D000 abc'").next(),
              ParseResultIs(Token(Token::String("\u000A\u00BB \u0CCC\uD000abc"), 0)));
}

TEST(Tokenizer, Hash) {
  {
    Tokenizer tokenizer("#a");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "a"), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 2)));
  }

  {
    Tokenizer tokenizer("#my-identifier_name#second");
    EXPECT_THAT(tokenizer.next(),
                ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "my-identifier_name"), 0)));
    EXPECT_THAT(tokenizer.next(),
                ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "second"), 19)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 26)));
  }

  // Name-allowable characters.
  EXPECT_THAT(Tokenizer("#abc_DEF-0123456789_-").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "abc_DEF-0123456789_-"), 0)));

  // Any number of dashes.
  EXPECT_THAT(Tokenizer("#-abc").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "-abc"), 0)));
  EXPECT_THAT(Tokenizer("#--def").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "--def"), 0)));

  // Just dashes is also okay, but two are required to be considered an "id" type.
  EXPECT_THAT(Tokenizer("#-").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Unrestricted, "-"), 0)));
  EXPECT_THAT(Tokenizer("#--").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "--"), 0)));

  // Escaped characters can occur at any point and still be considered an "id" type.
  EXPECT_THAT(Tokenizer("#\\20").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, " "), 0)));
  EXPECT_THAT(Tokenizer("#-\\20").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "- "), 0)));
  EXPECT_THAT(Tokenizer("#--\\O").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "--O"), 0)));

  // Identifiers that start with a digit are not "id" type.
  EXPECT_THAT(Tokenizer("#0start").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Unrestricted, "0start"), 0)));
  EXPECT_THAT(Tokenizer("#-0start").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Unrestricted, "-0start"), 0)));

  // If there are two dashes, any name-qualified characters are considered an "id".
  EXPECT_THAT(Tokenizer("#--0start").next(),
              ParseResultIs(Token(Token::Hash(Token::Hash::Type::Id, "--0start"), 0)));
}

}  // namespace css
}  // namespace donner
