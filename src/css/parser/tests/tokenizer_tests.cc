#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/base/parser/tests/parse_result_test_utils.h"
#include "src/css/parser/details/tokenizer.h"
#include "src/css/parser/tests/token_test_utils.h"

using testing::ElementsAre;

namespace donner {
namespace css {

using details::Tokenizer;

ParseResult<std::vector<Token>> AllTokens(Tokenizer&& tokenizer) {
  std::vector<Token> tokens;

  while (!tokenizer.isEOF()) {
    auto tokenResult = tokenizer.next();
    if (tokenResult.hasError()) {
      return std::move(tokenResult.error());
    } else {
      tokens.emplace_back(std::move(tokenResult.result()));
    }
  }

  return tokens;
}

TEST(Tokenizer, Empty) {
  Tokenizer tokenizer("");
  EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 0)));
}

TEST(Tokenizer, Whitespace) {
  Tokenizer tokenizer(" \t\f\r\n");
  EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Whitespace(" \t\f\r\n"), 0)));
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

  EXPECT_THAT(Tokenizer("/* test").next(), ParseErrorIs("Unterminated comment"));
  EXPECT_THAT(Tokenizer("/*/").next(), ParseErrorIs("Unterminated comment"));
}

TEST(Tokenizer, CommentAndWhitespace) {
  Tokenizer tokenizer("/**/ /**/\f\r\n/*\n*/");
  EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Whitespace(" "), 4)));
  EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Whitespace("\f\r\n"), 9)));
  EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 17)));
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

TEST(Tokenizer, Number) {
  EXPECT_THAT(AllTokens(Tokenizer("0")), ParseResultIs(ElementsAre(Token(Token::Number(0.), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("01234")),
              ParseResultIs(ElementsAre(Token(Token::Number(01234.), 0))));

  EXPECT_THAT(
      AllTokens(Tokenizer(".1234/* */987")),
      ParseResultIs(ElementsAre(Token(Token::Number(.1234), 0), Token(Token::Number(987.), 10))));

  EXPECT_THAT(AllTokens(Tokenizer("..1")),
              ParseResultIs(ElementsAre(Token(Token::Delim('.'), 0), Token(Token::Number(.1), 1))));
}

TEST(Tokenizer, NumberSigns) {
  EXPECT_THAT(Tokenizer("+").next(), ParseResultIs(Token(Token::Delim('+'), 0)));
  EXPECT_THAT(Tokenizer("-").next(), ParseResultIs(Token(Token::Delim('-'), 0)));
  EXPECT_THAT(AllTokens(Tokenizer("+-")),
              ParseResultIs(ElementsAre(Token(Token::Delim('+'), 0), Token(Token::Delim('-'), 1))));
  EXPECT_THAT(AllTokens(Tokenizer("+.")),
              ParseResultIs(ElementsAre(Token(Token::Delim('+'), 0), Token(Token::Delim('.'), 1))));

  EXPECT_THAT(Tokenizer("+0").next(), ParseResultIs(Token(Token::Number(0.), 0)));
  EXPECT_THAT(Tokenizer("-0").next(), ParseResultIs(Token(Token::Number(-0.), 0)));

  EXPECT_THAT(
      AllTokens(Tokenizer("+-0")),
      ParseResultIs(ElementsAre(Token(Token::Delim('+'), 0), Token(Token::Number(-0.), 1))));
  EXPECT_THAT(AllTokens(Tokenizer("-+0")),
              ParseResultIs(ElementsAre(Token(Token::Delim('.'), 0), Token(Token::Number(0.), 1))));
}

TEST(Tokenizer, NumberDecimal) {
  EXPECT_THAT(AllTokens(Tokenizer(".")), ParseResultIs(ElementsAre(Token(Token::Delim('.'), 0))));
  EXPECT_THAT(AllTokens(Tokenizer(".+")),
              ParseResultIs(ElementsAre(Token(Token::Delim('.'), 0), Token(Token::Delim('+'), 1))));

  EXPECT_THAT(AllTokens(Tokenizer(".0")), ParseResultIs(ElementsAre(Token(Token::Number(0.), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("-.1")),
              ParseResultIs(ElementsAre(Token(Token::Number(-.1), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("+.1")), ParseResultIs(ElementsAre(Token(Token::Number(.1), 0))));

  // Numbers should not end with a dot, it should create two tokens.
  EXPECT_THAT(AllTokens(Tokenizer("0.")),
              ParseResultIs(ElementsAre(Token(Token::Number(0), 0), Token(Token::Delim('.'), 1))));

  EXPECT_THAT(
      AllTokens(Tokenizer("0.6.5")),
      ParseResultIs(ElementsAre(Token(Token::Number(0.6), 0), Token(Token::Number(0.5), 3))));
}

TEST(Tokenizer, NumberExponent) {
  EXPECT_THAT(AllTokens(Tokenizer("1e0")), ParseResultIs(ElementsAre(Token(Token::Number(1.), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("-1e0")),
              ParseResultIs(ElementsAre(Token(Token::Number(-1.), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("-10e+2")),
              ParseResultIs(ElementsAre(Token(Token::Number(-1000.), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("10e-2")),
              ParseResultIs(ElementsAre(Token(Token::Number(.1), 0))));

  // Words for Inf and NaN should not be numbers.
  EXPECT_THAT(AllTokens(Tokenizer("Inf")),
              ParseResultIs(ElementsAre(Token(Token::Ident("Inf"), 0))));
  EXPECT_THAT(
      AllTokens(Tokenizer("+Inf")),
      ParseResultIs(ElementsAre(Token(Token::Delim('+'), 0), Token(Token::Ident("Inf"), 1))));
  EXPECT_THAT(AllTokens(Tokenizer("-Inf")),
              ParseResultIs(ElementsAre(Token(Token::Ident("-Inf"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("NaN")),
              ParseResultIs(ElementsAre(Token(Token::Ident("NaN"), 0))));

  // Infinite numbers should still parse.
  EXPECT_THAT(
      AllTokens(Tokenizer("99e999999999999999")),
      ParseResultIs(ElementsAre(Token(Token::Number(std::numeric_limits<double>::infinity()), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("-99e999999999999999")),
              ParseResultIs(
                  ElementsAre(Token(Token::Number(-std::numeric_limits<double>::infinity()), 0))));
}

TEST(Tokenizer, CharTokens) {
  EXPECT_THAT(Tokenizer("(").next(), ParseResultIs(Token(Token::Parenthesis(), 0)));
  EXPECT_THAT(Tokenizer(")").next(), ParseResultIs(Token(Token::CloseParenthesis(), 0)));
  EXPECT_THAT(Tokenizer("[").next(), ParseResultIs(Token(Token::SquareBracket(), 0)));
  EXPECT_THAT(Tokenizer("]").next(), ParseResultIs(Token(Token::CloseSquareBracket(), 0)));
  EXPECT_THAT(Tokenizer("{").next(), ParseResultIs(Token(Token::CurlyBracket(), 0)));
  EXPECT_THAT(Tokenizer("}").next(), ParseResultIs(Token(Token::CloseCurlyBracket(), 0)));
  EXPECT_THAT(Tokenizer(",").next(), ParseResultIs(Token(Token::Comma(), 0)));
  EXPECT_THAT(Tokenizer(":").next(), ParseResultIs(Token(Token::Colon(), 0)));
  EXPECT_THAT(Tokenizer(";").next(), ParseResultIs(Token(Token::Semicolon(), 0)));

  {
    Tokenizer tokenizer("()[]{},:;");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Parenthesis(), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::CloseParenthesis(), 1)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::SquareBracket(), 2)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::CloseSquareBracket(), 3)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::CurlyBracket(), 4)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::CloseCurlyBracket(), 5)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Comma(), 6)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Colon(), 7)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Semicolon(), 8)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 9)));
  }
}

TEST(Tokenizer, CDCAndCDO) {
  EXPECT_THAT(AllTokens(Tokenizer("<!--")), ParseResultIs(ElementsAre(Token(Token::CDO(), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("-->")), ParseResultIs(ElementsAre(Token(Token::CDC(), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("<!---->")),
              ParseResultIs(ElementsAre(Token(Token::CDO(), 0), Token(Token::CDC(), 4))));
  EXPECT_THAT(AllTokens(Tokenizer("<!-- -->")),
              ParseResultIs(ElementsAre(Token(Token::CDO(), 0), Token(Token::Whitespace(" "), 4),
                                        Token(Token::CDC(), 5))));
  EXPECT_THAT(Tokenizer("<").next(), ParseResultIs(Token(Token::Delim('<'), 0)));
}

TEST(Tokenizer, AtKeyword) {
  EXPECT_THAT(AllTokens(Tokenizer("@test")),
              ParseResultIs(ElementsAre(Token(Token::AtKeyword("test"), 0))));
  EXPECT_THAT(Tokenizer("@").next(), ParseResultIs(Token(Token::Delim('@'), 0)));
}

TEST(Tokenizer, IdentLikeToken) {
  EXPECT_THAT(AllTokens(Tokenizer("ident")),
              ParseResultIs(ElementsAre(Token(Token::Ident("ident"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("--ident")),
              ParseResultIs(ElementsAre(Token(Token::Ident("--ident"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("\\20ident")),
              ParseResultIs(ElementsAre(Token(Token::Ident(" ident"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("\\")), ParseResultIs(ElementsAre(Token(Token::Delim('\\'), 0))));

  EXPECT_THAT(AllTokens(Tokenizer("func()")),
              ParseResultIs(ElementsAre(Token(Token::Function("func"), 0),
                                        Token(Token::CloseParenthesis(), 5))));
  EXPECT_THAT(
      AllTokens(Tokenizer("func('test')")),
      ParseResultIs(ElementsAre(Token(Token::Function("func"), 0), Token(Token::String("test"), 5),
                                Token(Token::CloseParenthesis(), 11))));
  EXPECT_THAT(AllTokens(Tokenizer("func(  'test')")),
              ParseResultIs(ElementsAre(
                  Token(Token::Function("func"), 0), Token(Token::Whitespace("  "), 5),
                  Token(Token::String("test"), 7), Token(Token::CloseParenthesis(), 13))));

  EXPECT_THAT(AllTokens(Tokenizer("func ()")),
              ParseResultIs(ElementsAre(
                  Token(Token::Ident("func"), 0), Token(Token::Whitespace(" "), 4),
                  Token(Token::Parenthesis(), 5), Token(Token::CloseParenthesis(), 6))));
}

TEST(Tokenizer, Url) {
  EXPECT_THAT(AllTokens(Tokenizer("url()")), ParseResultIs(ElementsAre(Token(Token::Url(""), 0))));

  EXPECT_THAT(AllTokens(Tokenizer("url(test)")),
              ParseResultIs(ElementsAre(Token(Token::Url("test"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("uRL(mixed-case)")),
              ParseResultIs(ElementsAre(Token(Token::Url("mixed-case"), 0))));

  // If EOF is hit, returns the remaining data.
  EXPECT_THAT(AllTokens(Tokenizer("url(")), ParseResultIs(ElementsAre(Token(Token::Url(""), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("url(asdf")),
              ParseResultIs(ElementsAre(Token(Token::Url("asdf"), 0))));

  // Whitespace is allowed, both before and after the argument.
  EXPECT_THAT(AllTokens(Tokenizer("url( before)")),
              ParseResultIs(ElementsAre(Token(Token::Url("before"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("url(after )")),
              ParseResultIs(ElementsAre(Token(Token::Url("after"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("url( \t  both \n )")),
              ParseResultIs(ElementsAre(Token(Token::Url("both"), 0))));

  // Whitespace in the middle is not allowed.
  EXPECT_THAT(AllTokens(Tokenizer("url(whitespace in middle)")),
              ParseResultIs(ElementsAre(Token(Token::BadUrl(), 0))));

  // Quotes in middle or non-printable characters are not allowed.
  EXPECT_THAT(AllTokens(Tokenizer("url(mid'quotes)")),
              ParseResultIs(ElementsAre(Token(Token::BadUrl(), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("url(not\u001Fprintable)")),
              ParseResultIs(ElementsAre(Token(Token::BadUrl(), 0))));

  // `(` is not allowed in the URL either.
  EXPECT_THAT(AllTokens(Tokenizer("url(()")),
              ParseResultIs(ElementsAre(Token(Token::BadUrl(), 0))));

  // Escapes are allowed.
  EXPECT_THAT(AllTokens(Tokenizer("url(\\20)")),
              ParseResultIs(ElementsAre(Token(Token::Url(" "), 0))));

  // Allow escaping a `)`.
  EXPECT_THAT(AllTokens(Tokenizer("url(\\))")),
              ParseResultIs(ElementsAre(Token(Token::Url(")"), 0))));
  EXPECT_THAT(AllTokens(Tokenizer("url(bad url \\))")),
              ParseResultIs(ElementsAre(Token(Token::BadUrl(), 0))));
}

TEST(Tokenizer, Delim) {
  EXPECT_THAT(Tokenizer("!").next(), ParseResultIs(Token(Token::Delim('!'), 0)));
  EXPECT_THAT(Tokenizer("$").next(), ParseResultIs(Token(Token::Delim('$'), 0)));
  EXPECT_THAT(Tokenizer("%").next(), ParseResultIs(Token(Token::Delim('%'), 0)));
  EXPECT_THAT(Tokenizer("^").next(), ParseResultIs(Token(Token::Delim('^'), 0)));
  EXPECT_THAT(Tokenizer("&").next(), ParseResultIs(Token(Token::Delim('&'), 0)));

  {
    Tokenizer tokenizer("!$");
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Delim('!'), 0)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::Delim('$'), 1)));
    EXPECT_THAT(tokenizer.next(), ParseResultIs(Token(Token::EOFToken(), 2)));
  }
}

}  // namespace css
}  // namespace donner
