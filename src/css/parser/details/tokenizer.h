#pragma once

#include <utfcpp/source/utf8.h>

#include <string_view>

#include "src/base/parser/number_parser.h"
#include "src/base/parser/parse_result.h"
#include "src/css/parser/details/common.h"
#include "src/css/token.h"

namespace donner {
namespace css {
namespace details {

class Tokenizer {
public:
  Tokenizer(std::string_view str) : str_(str), remaining_(str) {}

  ParseResult<Token> next() {
    if (auto error = tryConsumeComment()) {
      return std::move(error.value());
    }

    if (isEOF()) {
      return Token(Token::EOFToken(), currentOffset());
    }

    switch (remaining_[0]) {
      // Whitespace defined by https://www.w3.org/TR/css-syntax-3/#whitespace
      // Note that this also includes whitespace that is normally converted during the preprocessing
      // step which is not performed here.
      case ' ':
      case '\t':
      case '\f':
      case '\r':
      case '\n': return consumeWhitespace(); break;

      case '"':
      case '\'': return consumeQuotedString(); break;

      case '#': {
        const size_t remainingChars = remaining_.size();
        if ((remainingChars > 1 && isNameCodepoint(remaining_[1])) ||
            (remainingChars > 2 && isValidEscape(remaining_[1], remaining_[2]))) {
          const Token::Hash::Type type = isIdentifierStart(remaining_.substr(1))
                                             ? Token::Hash::Type::Id
                                             : Token::Hash::Type::Unrestricted;

          auto [name, charsConsumed] = consumeName(remaining_.substr(1));
          return token<Token::Hash>(1 + charsConsumed, type, std::move(name));
        } else {
          return token<Token::Delim>(1, '#');
        }
        break;
      }

      case '+':
      case '.': {
        if (isNumberStart(remaining_)) {
          return consumeNumericToken();
        } else {
          return token<Token::Delim>(1, remaining_[0]);
        }
      }

      case '-': {
        if (isNumberStart(remaining_)) {
          return consumeNumericToken();
        } else if (remaining_.starts_with("-->")) {
          return token<Token::CDC>(3);
        } else if (isIdentifierStart(remaining_)) {
          return consumeIdentLikeToken();
        } else {
          return token<Token::Delim>(1, '-');
        }
      }

      case '<': {
        // If the next 3 input code points are U+0021 EXCLAMATION MARK U+002D HYPHEN-MINUS U+002D
        // HYPHEN-MINUS (!--), consume them and return a <CDO-token>.
        if (remaining_.starts_with("<!--")) {
          return token<Token::CDO>(4);
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return token<Token::Delim>(1, remaining_[0]);
      }

      case '@': {
        // If the next 3 input code points would start an identifier, consume a name, create an
        // <at-keyword-token> with its value set to the returned value, and return it.
        if (isIdentifierStart(remaining_.substr(1))) {
          auto [name, nameCharsConsumed] = consumeName(remaining_.substr(1));
          return token<Token::AtKeyword>(nameCharsConsumed + 1, std::move(name));
        }

        // Otherwise, return a <delim-token> with its value set to the current input code point.
        return token<Token::Delim>(1, remaining_[0]);
      }

      case '\\': {
        // If the input stream starts with a valid escape, reconsume the current input code point,
        // consume an ident-like token, and return it.
        if (remaining_.size() > 1 && isValidEscape('\\', remaining_[1])) {
          return consumeIdentLikeToken();
        }

        // Otherwise, this is a parse error. Return a <delim-token> with its value set to the
        // current input code point.
        return token<Token::Delim>(1, remaining_[0]);
      }

      case '(': return token<Token::Parenthesis>(1);
      case ')': return token<Token::CloseParenthesis>(1);
      case '[': return token<Token::SquareBracket>(1);
      case ']': return token<Token::CloseSquareBracket>(1);
      case '{': return token<Token::CurlyBracket>(1);
      case '}': return token<Token::CloseCurlyBracket>(1);
      case ',': return token<Token::Comma>(1);
      case ':': return token<Token::Colon>(1);
      case ';': return token<Token::Semicolon>(1);
    }

    if (std::isdigit(remaining_[0])) {
      return consumeNumericToken();
    } else if (isNameStartCodepoint(remaining_[0])) {
      return consumeIdentLikeToken();
    } else {
      return token<Token::Delim>(1, remaining_[0]);
    }
  }

  bool isEOF() const { return remaining_.empty(); }

private:
  size_t currentOffset() { return remaining_.data() - str_.data(); }

  template <typename T, typename... Args>
  Token token(size_t length, Args... args) {
    const size_t offset = currentOffset();
    remaining_.remove_prefix(length);
    return Token(T(std::forward<Args>(args)...), offset);
  }

  std::optional<ParseError> tryConsumeComment() {
    if (!remaining_.starts_with("/*")) {
      return std::nullopt;
    }

    for (size_t i = 2; i + 1 < remaining_.size(); ++i) {
      if (remaining_[i] == '*' && remaining_[i + 1] == '/') {
        remaining_.remove_prefix(i + 2);
        return std::nullopt;
      }
    }

    ParseError err;
    err.reason = "Unterminated comment";
    err.offset = currentOffset();

    remaining_.remove_prefix(remaining_.size());
    return err;
  }

  Token consumeWhitespace() {
    assert(isWhitespace(remaining_[0]));

    for (size_t i = 1; i < remaining_.size(); ++i) {
      if (!isWhitespace(remaining_[i])) {
        return token<Token::Whitespace>(i, std::string(remaining_.substr(0, i)));
      }
    }

    return token<Token::Whitespace>(remaining_.size(), std::string(remaining_));
  }

  /// Consume a string token per https://www.w3.org/TR/css-syntax-3/#consume-a-string-token
  ParseResult<Token> consumeQuotedString() {
    const char quote = remaining_[0];
    assert(quote == '"' || quote == '\'');

    // TODO: Introduce RefCountedString type.
    std::vector<char> str;

    const size_t remainingSize = remaining_.size();
    for (size_t i = 1; i < remainingSize; ++i) {
      const char ch = remaining_[i];
      if (ch == quote) {
        // ending code point: Return the <string-token>.
        return token<Token::String>(i + 1, std::string(str.begin(), str.end()));
      } else if (isNewline(ch)) {
        // newline: This is a parse error. Reconsume the current input code point, create a
        // <bad-string-token>, and return it.
        return token<Token::BadString>(i + 1, std::string(str.begin(), str.end()));
      } else if (ch == '\\') {
        // U+005C REVERSE SOLIDUS (\): If the next input code point is EOF, do nothing.
        if (i + 1 == remainingSize) {
          break;
        }

        const char nextCh = remaining_[i + 1];
        // Otherwise, if the next input code point is a newline, consume it.
        if (isNewline(nextCh)) {
          ++i;
        } else {
          // Otherwise, (the stream starts with a valid escape) consume an escaped code point and
          // append the returned code point to the <string-token>'s value.
          const auto [codepoint, bytesConsumed] = consumeEscapedCodepoint(remaining_.substr(i + 1));
          utf8::append(codepoint, std::back_inserter(str));
          i += bytesConsumed;
        }
      } else {
        // anything else: Append the current input code point to the <string-token>'s value.
        str.push_back(ch);
      }
    }

    ParseError err;
    err.reason = "Unterminated string";
    err.offset = currentOffset() + remaining_.size() - 1;
    return err;
  }

  /// Consume a name, per https://www.w3.org/TR/css-syntax-3/#consume-name
  ///
  /// @return A tuple containing the parsed name and the number of bytes consumed.
  static std::tuple<std::string, size_t> consumeName(std::string_view remaining) {
    // TODO: Introduce RefCountedString type.
    std::vector<char> str;

    const size_t remainingSize = remaining.size();
    size_t i = 0;
    while (i < remainingSize) {
      const char ch = remaining[i];

      if (isNameCodepoint(ch)) {
        // name code point: Append the code point to result.
        str.push_back(ch);
        ++i;
      } else if (i + 1 < remainingSize && isValidEscape(ch, remaining[i + 1])) {
        // the stream starts with a valid escape: Consume an escaped code point. Append the returned
        // code point to result.
        const auto [codepoint, bytesConsumed] = consumeEscapedCodepoint(remaining.substr(i + 1));
        utf8::append(codepoint, std::back_inserter(str));
        i += 1 + bytesConsumed;
      } else {
        // anything else: Reconsume the current input code point. Return result.
        break;
      }
    }

    return {std::string(str.begin(), str.end()), i};
  }

  /// Consume an escaped code point, per
  /// https://www.w3.org/TR/css-syntax-3/#consume-an-escaped-code-point
  ///
  /// @return A tuple containing the parsed codepoint and the number of bytes consumed.
  static std::tuple<char32_t, size_t> consumeEscapedCodepoint(std::string_view remaining) {
    if (remaining.empty()) {
      // EOF: This is a parse error. Return U+FFFD REPLACEMENT CHARACTER (�).
      return {kUnicodeReplacementCharacter, 0};
    } else if (std::isxdigit(remaining[0])) {
      char32_t number = hexDigitToDecimal(remaining[0]);
      size_t i = 1;

      // Consume as many hex digits as possible, but no more than 5 (more).
      const size_t maxLength = std::min(remaining.size(), size_t(6));
      while (i < maxLength && std::isxdigit(remaining[i])) {
        number = (number << 4) | hexDigitToDecimal(remaining[i]);
        ++i;
      }

      // If the next input codepoint is whitespace, consume it as well.
      if (i < remaining.size() && isWhitespace(remaining[i])) {
        ++i;
      }

      // If this number is zero, or is for a surrogate, or is greater than the maximum allowed code
      // point, return U+FFFD REPLACEMENT CHARACTER (�).
      if (number == 0 || isSurrogateCodepoint(number) || number > kMaximumAllowedCodepoint) {
        return {kUnicodeReplacementCharacter, i};
      }

      // Otherwise, return the code point with that value.
      return {number, i};
    } else {
      return {static_cast<unsigned char>(remaining[0]), 1};
    }
  }

  /// Consume a numeric token, per https://www.w3.org/TR/css-syntax-3/#consume-numeric-token
  Token consumeNumericToken() {
    NumberParser::Options options;
    options.forbid_out_of_range = false;

    ParseResult<NumberParser::Result> numberResult = NumberParser::Parse(remaining_, options);
    assert(numberResult.hasResult());  // Should not hit due to isNumberStart() precondition.

    NumberParser::Result number = numberResult.result();

    std::string_view remainingAfterNumber = remaining_.substr(number.consumed_chars);
    if (isIdentifierStart(remainingAfterNumber)) {
      auto [name, nameConsumedChars] = consumeName(remainingAfterNumber);
      return token<Token::Dimension>(number.consumed_chars + nameConsumedChars, number.number,
                                     name);
    } else if (remainingAfterNumber.starts_with("%")) {
      return token<Token::Percentage>(number.consumed_chars + 1, number.number);
    } else {
      return token<Token::Number>(number.consumed_chars, number.number);
    }
  }

  /// Consume an ident-like token, per https://www.w3.org/TR/css-syntax-3/#consume-ident-like-token
  Token consumeIdentLikeToken() {
    auto [name, nameCharsConsumed] = consumeName(remaining_);

    const std::string_view afterName = remaining_.substr(nameCharsConsumed);
    const bool hasParen = afterName.starts_with("(");

    // If `name`'s value is an ASCII case-insensitive match for "url", and the next input code point
    // is U+0028 LEFT PARENTHESIS ((), consume it.
    if (stringLowercaseEq(name, "url") && hasParen) {
      size_t i = 1;
      size_t remainingSize = afterName.size();

      // While the next two input code points are whitespace, consume the next input code point.
      while (i + 1 < remainingSize && isWhitespace(afterName[i]) &&
             isWhitespace(afterName[i + 1])) {
        ++i;
      }

      // If the next one or two input code points are U+0022 QUOTATION MARK ("), U+0027 APOSTROPHE
      // ('), or whitespace followed by U+0022 QUOTATION MARK (") or U+0027 APOSTROPHE ('), then
      // create a <function-token> with its value set to `name` and return it.
      if (isQuote(afterName[i]) ||
          (i + 1 < remainingSize && isWhitespace(afterName[i]) && isQuote(afterName[i + 1]))) {
        return token<Token::Function>(nameCharsConsumed + 1, std::move(name));
      }

      // Otherwise, consume a url token, and return it.
      return consumeUrlToken(afterName.substr(1), nameCharsConsumed + 1);
    } else if (hasParen) {
      // Otherwise, if the next input code point is U+0028 LEFT PARENTHESIS ((), consume it. Create
      // a <function-token> with its value set to `name` and return it.
      return token<Token::Function>(nameCharsConsumed + 1, std::move(name));
    } else {
      // Otherwise, create an <ident-token> with its value set to `name` and return it.
      return token<Token::Ident>(nameCharsConsumed, std::move(name));
    }
  }

  /// Consume a url token, per https://www.w3.org/TR/css-syntax-3/#consume-url-token
  Token consumeUrlToken(const std::string_view afterUrl, size_t charsConsumedBefore) {
    // Consume as much whitespace as possible.
    size_t i = 0;
    while (i < afterUrl.size() && isWhitespace(afterUrl[i])) {
      ++i;
    }

    // TODO: Introduce RefCountedString type.
    std::vector<char> str;

    while (i < afterUrl.size()) {
      const char ch = afterUrl[i];

      if (ch == ')') {
        return token<Token::Url>(i + charsConsumedBefore + 1, std::string(str.begin(), str.end()));
      } else if (isWhitespace(ch)) {
        ++i;
        while (i < afterUrl.size() && isWhitespace(afterUrl[i])) {
          ++i;
        }

        // Consume as much whitespace as possible. If the next input code point is U+0029 RIGHT
        // PARENTHESIS ()) or EOF, consume it and return the <url-token>.
        if (i == afterUrl.size() || afterUrl[i] == ')') {
          continue;
        } else {
          // Otherwise, consume the remnants of a bad url, create a <bad-url-token>, and return it.
          return consumeRemnantsOfBadUrl(afterUrl.substr(i), charsConsumedBefore + i);
        }
      } else if (isQuote(ch) || ch == '(' || isNonPrintableCodepoint(ch)) {
        // This is a parse error. Consume the remnants of a bad url, create a <bad-url-token>, and
        // return it.
        return consumeRemnantsOfBadUrl(afterUrl.substr(i), charsConsumedBefore + i);
      } else if (i + 1 < afterUrl.size() && isValidEscape(afterUrl[i], afterUrl[i + 1])) {
        // U+005C REVERSE SOLIDUS (\): If the stream starts with a valid escape, consume an escaped
        // code point and append the returned code point to the <url-token>'s value.
        const auto [codepoint, bytesConsumed] = consumeEscapedCodepoint(afterUrl.substr(i + 1));
        utf8::append(codepoint, std::back_inserter(str));
        i += bytesConsumed + 1;
      } else {
        // anything else: Append the current input code point to the <url-token>'s value.
        str.push_back(afterUrl[i]);
        ++i;
      }
    }

    // EOF: This is a parse error. Return the <url-token>.
    return token<Token::Url>(i + charsConsumedBefore, std::string(str.begin(), str.end()));
  }

  /// Consume the remnants of a bad url, per
  /// https://www.w3.org/TR/css-syntax-3/#consume-remnants-of-bad-url
  Token consumeRemnantsOfBadUrl(const std::string_view badUrl, size_t charsConsumedBefore) {
    size_t i = 0;

    while (i < badUrl.size()) {
      const char ch = badUrl[i];

      if (ch == ')') {
        ++i;
        break;
      } else if (i + 1 < badUrl.size() && isValidEscape(badUrl[i], badUrl[i + 1])) {
        // the input stream starts with a valid escape: Consume an escaped code point. This allows
        // an escaped right parenthesis ("\)") to be encountered without ending the <bad-url-token>.
        // This is otherwise identical to the "anything else" clause.
        const auto [codepoint, bytesConsumed] = consumeEscapedCodepoint(badUrl.substr(i + 1));
        i += bytesConsumed + 1;
      } else {
        ++i;
      }
    }

    return token<Token::BadUrl>(i + charsConsumedBefore);
  }

  static bool isWhitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\f' || ch == '\r' || ch == '\n';
  }

  static bool isNewline(char ch) { return ch == '\r' || ch == '\n'; }

  static bool isQuote(char ch) { return ch == '"' || ch == '\''; }

  /// Returns true if the character is a valid name start codepoint, per
  /// https://www.w3.org/TR/css-syntax-3/#name-start-code-point
  static bool isNameStartCodepoint(char ch) {
    return std::isalpha(ch) || static_cast<unsigned char>(ch) >= 0x80 || ch == '_';
  }

  static bool isNameCodepoint(char ch) {
    return isNameStartCodepoint(ch) || std::isdigit(ch) || ch == '-';
  }

  /// Returns true if the codepoint is a surrogate, per
  /// https://infra.spec.whatwg.org/#surrogate
  static bool isSurrogateCodepoint(char32_t ch) { return ch >= 0xD800 && ch <= 0xDFFF; }

  /// Returns true if the codepoint is non-printable, per
  /// https://www.w3.org/TR/css-syntax-3/#non-printable-code-point
  static bool isNonPrintableCodepoint(char ch) {
    return (ch >= 0 && ch <= 0x08) || ch == 0x0B || (ch >= 0x0E && ch <= 0x1F) || ch == 0x7F;
  }

  static uint8_t hexDigitToDecimal(char ch) {
    switch (ch) {
      case '0': return 0;
      case '1': return 1;
      case '2': return 2;
      case '3': return 3;
      case '4': return 4;
      case '5': return 5;
      case '6': return 6;
      case '7': return 7;
      case '8': return 8;
      case '9': return 9;
      case 'a':
      case 'A': return 10;
      case 'b':
      case 'B': return 11;
      case 'c':
      case 'C': return 12;
      case 'd':
      case 'D': return 13;
      case 'e':
      case 'E': return 14;
      case 'f':
      case 'F': return 15;
    }

    assert(false && "Should be unreachable.");
  }

  /// Check if two code points are a valid escape, per
  /// https://www.w3.org/TR/css-syntax-3/#starts-with-a-valid-escape
  static bool isValidEscape(char first, char second) {
    // If the first code point is not U+005C REVERSE SOLIDUS (\), return false.
    // Otherwise, if the second code point is a newline, return false.
    // Otherwise, return true.
    return (first == '\\' && !isNewline(second));
  }

  /// Check if three code points would start an identifier, per
  /// https://www.w3.org/TR/css-syntax-3/#check-if-three-code-points-would-start-an-identifier
  static bool isIdentifierStart(std::string_view remaining) {
    if (remaining.empty()) {
      return false;
    }

    const size_t remainingSize = remaining.size();
    if (remaining[0] == '-') {
      // U+002D HYPHEN-MINUS: If the second code point is a name-start code point or a U+002D
      // HYPHEN-MINUS, or the second and third code points are a valid escape, return true.
      // Otherwise, return false.
      return ((remainingSize > 1 && (isNameStartCodepoint(remaining[1]) || remaining[1] == '-')) ||
              (remainingSize > 2 && isValidEscape(remaining[1], remaining[2])));
    } else if (isNameStartCodepoint(remaining[0])) {
      return true;
    } else if (remainingSize > 1 && isValidEscape(remaining[0], remaining[1])) {
      // U+005C REVERSE SOLIDUS (\): If the first and second code points are a valid escape, return
      // true. Otherwise, return false.
      return true;
    }

    return false;
  }

  /// Check if three code points would start a number, per
  /// https://www.w3.org/TR/css-syntax-3/#starts-with-a-number
  static bool isNumberStart(std::string_view remaining) {
    if (remaining.empty()) {
      return false;
    }

    const size_t remainingSize = remaining.size();
    if (remaining[0] == '+' || remaining[0] == '-') {
      // U+002B PLUS SIGN (+) or U+002D HYPHEN-MINUS (-): If the second code point is a digit,
      // return true.
      if (remainingSize > 1 && std::isdigit(remaining[1])) {
        return true;
      }

      // Otherwise, if the second code point is a U+002E FULL STOP (.) and the third code point is a
      // digit, return true.
      if (remainingSize > 2 && remaining[1] == '.' && std::isdigit(remaining[2])) {
        return true;
      }

      return false;
    } else if (remaining[0] == '.') {
      // If the second code point is a digit, return true. Otherwise, return false.
      return (remainingSize > 1 && std::isdigit(remaining[1]));
    } else if (std::isdigit(remaining[0])) {
      return true;
    } else {
      return false;
    }
  }

  /// U+FFFD REPLACEMENT CHARACTER (�)
  static constexpr char32_t kUnicodeReplacementCharacter = 0xFFFD;

  /// The greatest codepoint defined by unicode, per
  /// https://www.w3.org/TR/css-syntax-3/#maximum-allowed-code-point
  static constexpr char32_t kMaximumAllowedCodepoint = 0x10FFFF;

  const std::string_view str_;
  std::string_view remaining_;
};

}  // namespace details
}  // namespace css
}  // namespace donner