#pragma once

#include <string_view>

#include "src/base/parser/length_parser.h"
#include "src/base/parser/number_parser.h"
#include "src/base/rc_string.h"
#include "src/base/utils.h"
#include "src/css/parser/details/common.h"
#include "src/css/token.h"

namespace donner::css {
namespace details {

class Tokenizer {
public:
  explicit Tokenizer(std::string_view str) : str_(str), remaining_(str) {}

  Token next() {
    if (nextToken_) {
      auto result = std::move(nextToken_.value());
      nextToken_ = std::nullopt;
      return result;
    }

    if (auto errorToken = consumeComments()) {
      return token<Token::ErrorToken>(remaining_.size(), errorToken.value());
    }

    if (isEOF()) {
      return Token(Token::EofToken(), currentOffset());
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

  bool isEOF() const { return remaining_.empty() && !nextToken_; }

private:
  size_t currentOffset() { return remaining_.data() - str_.data(); }

  template <typename T, typename... Args>
  Token token(size_t length, Args... args) {
    const size_t offset = currentOffset();
    remaining_.remove_prefix(length);
    return Token(T(std::forward<Args>(args)...), offset);
  }

  std::optional<Token::ErrorToken> consumeComments() {
    while (remaining_.starts_with("/*")) {
      bool foundEnd = false;

      for (size_t i = 2; i + 1 < remaining_.size(); ++i) {
        if (remaining_[i] == '*' && remaining_[i + 1] == '/') {
          remaining_.remove_prefix(i + 2);
          foundEnd = true;
          break;
        }
      }

      if (!foundEnd) {
        return Token::ErrorToken(Token::ErrorToken::Type::EofInComment);
      }
    }

    return std::nullopt;
  }

  Token consumeWhitespace() {
    assert(isWhitespace(remaining_[0]));

    for (size_t i = 1; i < remaining_.size(); ++i) {
      if (!isWhitespace(remaining_[i])) {
        return token<Token::Whitespace>(i, RcString(remaining_.substr(0, i)));
      }
    }

    return token<Token::Whitespace>(remaining_.size(), RcString(remaining_));
  }

  /// Consume a string token per https://www.w3.org/TR/css-syntax-3/#consume-a-string-token
  Token consumeQuotedString() {
    const char quote = remaining_[0];
    assert(quote == '"' || quote == '\'');

    std::vector<char> str;
    const size_t remainingSize = remaining_.size();
    for (size_t i = 1; i < remainingSize; ++i) {
      const char ch = remaining_[i];
      if (ch == quote) {
        // ending code point: Return the <string-token>.
        return token<Token::String>(i + 1, RcString::fromVector(std::move(str)));
      } else if (isNewline(ch)) {
        // newline: This is a parse error. Reconsume the current input code point, create a
        // <bad-string-token>, and return it.
        return token<Token::BadString>(i, RcString::fromVector(std::move(str)));
      } else if (ch == '\\') {
        // U+005C REVERSE SOLIDUS (\): If the next input code point is EOF, do nothing.
        if (i + 1 == remainingSize) {
          break;
        }

        const char nextCh = remaining_[i + 1];
        // Otherwise, if the next input code point is a newline, consume it.
        if (isNewline(nextCh)) {
          i += isTwoCharacterNewline(remaining_.substr(i + 1)) ? 2 : 1;
        } else {
          // Otherwise, (the stream starts with a valid escape) consume an escaped code point and
          // append the returned code point to the <string-token>'s value.
          const auto [codepoint, bytesConsumed] = consumeEscapedCodepoint(remaining_.substr(i + 1));
          details::Utf8Append(codepoint, std::back_inserter(str));
          i += bytesConsumed;
        }
      } else if (ch == '\0') {
        details::Utf8Append(kUnicodeReplacementCharacter, std::back_inserter(str));
      } else {
        // anything else: Append the current input code point to the <string-token>'s value.
        str.push_back(ch);
      }
    }

    auto result = token<Token::String>(remaining_.size(), RcString::fromVector(std::move(str)));
    nextToken_ = token<Token::ErrorToken>(0, Token::ErrorToken::Type::EofInString);
    return result;
  }

  /// Consume a name, per https://www.w3.org/TR/css-syntax-3/#consume-name
  ///
  /// @return A tuple containing the parsed name and the number of bytes consumed.
  static std::tuple<RcString, size_t> consumeName(std::string_view remaining) {
    std::vector<char> str;

    const size_t remainingSize = remaining.size();
    size_t i = 0;
    while (i < remainingSize) {
      const char ch = remaining[i];

      if (isNameCodepoint(ch)) {
        // name code point: Append the code point to result.
        if (ch != '\0') {
          str.push_back(ch);
        } else {
          details::Utf8Append(kUnicodeReplacementCharacter, std::back_inserter(str));
        }
        ++i;
      } else if (isValidEscape(remaining.substr(i))) {
        // the stream starts with a valid escape: Consume an escaped code point. Append the returned
        // code point to result.
        const auto [codepoint, bytesConsumed] = consumeEscapedCodepoint(remaining.substr(i + 1));
        details::Utf8Append(codepoint, std::back_inserter(str));
        i += 1 + bytesConsumed;
      } else {
        // anything else: Reconsume the current input code point. Return result.
        break;
      }
    }

    return {RcString::fromVector(std::move(str)), i};
  }

  /// Consume an escaped code point, per
  /// https://www.w3.org/TR/css-syntax-3/#consume-an-escaped-code-point
  ///
  /// @return A tuple containing the parsed codepoint and the number of bytes consumed.
  static std::tuple<char32_t, int> consumeEscapedCodepoint(std::string_view remaining) {
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
      while (i < remaining.size() && isWhitespace(remaining[i])) {
        ++i;
      }

      // If this number is zero, or is for a surrogate, or is greater than the maximum allowed code
      // point, return U+FFFD REPLACEMENT CHARACTER (�).
      if (number == 0 || !IsValidCodepoint(number)) {
        return {kUnicodeReplacementCharacter, i};
      }

      // Otherwise, return the code point with that value.
      return {number, i};
    } else {
      if (remaining[0] != '\0') {
        const auto [codepoint, bytesConsumed] = details::Utf8NextCodepoint(remaining);
        return {IsValidCodepoint(codepoint) ? codepoint : kUnicodeReplacementCharacter,
                bytesConsumed};
      } else {
        // Transform \0 to the unicode replacement character, since the proprocess step has been
        // skipped.
        return {kUnicodeReplacementCharacter, 1};
      }
    }
  }

  /// Consume a numeric token, per https://www.w3.org/TR/css-syntax-3/#consume-numeric-token
  Token consumeNumericToken() {
    NumberParser::Options options;
    options.forbidOutOfRange = false;

    ParseResult<NumberParser::Result> numberResult = NumberParser::Parse(remaining_, options);
    assert(numberResult.hasResult());  // Should not hit due to isNumberStart() precondition.

    NumberParser::Result number = numberResult.result();

    RcString numberString(remaining_.substr(0, number.consumedChars));
    NumberType type = NumberType::Integer;
    for (char ch : numberString) {
      if (ch == '.' || ch == 'E' || ch == 'e') {
        type = NumberType::Number;
        break;
      }
    }

    std::string_view remainingAfterNumber = remaining_.substr(number.consumedChars);
    if (isIdentifierStart(remainingAfterNumber)) {
      auto [name, nameConsumedChars] = consumeName(remainingAfterNumber);
      return token<Token::Dimension>(number.consumedChars + nameConsumedChars, number.number, name,
                                     LengthParser::ParseUnit(name), std::move(numberString), type);
    } else if (remainingAfterNumber.starts_with("%")) {
      return token<Token::Percentage>(number.consumedChars + 1, number.number,
                                      std::move(numberString), type);
    } else {
      return token<Token::Number>(number.consumedChars, number.number, std::move(numberString),
                                  type);
    }
  }

  /// Consume an ident-like token, per https://www.w3.org/TR/css-syntax-3/#consume-ident-like-token
  Token consumeIdentLikeToken() {
    auto [name, nameCharsConsumed] = consumeName(remaining_);

    const std::string_view afterName = remaining_.substr(nameCharsConsumed);
    const bool hasParen = afterName.starts_with("(");

    // If `name`'s value is an ASCII case-insensitive match for "url", and the next input code point
    // is U+0028 LEFT PARENTHESIS ((), consume it.
    if (StringLowercaseEq(name, "url") && hasParen) {
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

    std::vector<char> str;
    while (i < afterUrl.size()) {
      const char ch = afterUrl[i];

      if (ch == ')') {
        return token<Token::Url>(i + charsConsumedBefore + 1, RcString::fromVector(std::move(str)));
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
      } else if (ch == '\0') {
        details::Utf8Append(kUnicodeReplacementCharacter, std::back_inserter(str));
        ++i;
      } else if (isQuote(ch) || ch == '(' || isNonPrintableCodepoint(ch)) {
        // This is a parse error. Consume the remnants of a bad url, create a <bad-url-token>, and
        // return it.
        return consumeRemnantsOfBadUrl(afterUrl.substr(i), charsConsumedBefore + i);
      } else if (isValidEscape(afterUrl.substr(i))) {
        // U+005C REVERSE SOLIDUS (\): If the stream starts with a valid escape, consume an escaped
        // code point and append the returned code point to the <url-token>'s value.
        const auto [codepoint, bytesConsumed] = consumeEscapedCodepoint(afterUrl.substr(i + 1));
        details::Utf8Append(codepoint, std::back_inserter(str));
        i += bytesConsumed + 1;
      } else {
        // anything else: Append the current input code point to the <url-token>'s value.
        str.push_back(afterUrl[i]);
        ++i;
      }
    }

    // EOF: This is a parse error. Return the <url-token>.
    auto result = token<Token::Url>(i + charsConsumedBefore, RcString::fromVector(std::move(str)));
    nextToken_ = token<Token::ErrorToken>(0, Token::ErrorToken::Type::EofInUrl);
    return result;
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

  /// Returns if a character is a newline. Since this tokenizer skips the preprocessing step also
  /// consider U+000D CARRIAGE RETURN (CR) and U+000C FORM FEED (FF) as newlines.
  static bool isNewline(char ch) { return ch == '\r' || ch == '\n' || ch == '\f'; }

  /// Returns if the string starts with a \r\n, which should be treated as one character instead of
  /// two. Normally these are collapsed during the preprocessing step.
  static bool isTwoCharacterNewline(std::string_view str) { return str.starts_with("\r\n"); }

  static bool isQuote(char ch) { return ch == '"' || ch == '\''; }

  /// Returns true if the character is a valid name start codepoint, per
  /// https://www.w3.org/TR/css-syntax-3/#name-start-code-point
  static bool isNameStartCodepoint(char ch) {
    // Also include \0, since this tokenizer postpones the preprocessing step at
    // https://www.w3.org/TR/css-syntax-3/#input-preprocessing which transforms \u0000 to \uFFFD,
    // which would pass this check.
    return std::isalpha(ch) || static_cast<unsigned char>(ch) >= 0x80 || ch == '_' || ch == '\0';
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

    UTILS_UNREACHABLE();  // LCOV_EXCL_LINE: All cases should be handled above.
  }

  /// Check if two code points are a valid escape, per
  /// https://www.w3.org/TR/css-syntax-3/#starts-with-a-valid-escape
  static bool isValidEscape(char first, char second) {
    // If the first code point is not U+005C REVERSE SOLIDUS (\), return false.
    // Otherwise, if the second code point is a newline, return false.
    // Otherwise, return true.
    return (first == '\\' && !isNewline(second));
  }

  /// Check if up to two code points are a valid escape.
  static bool isValidEscape(std::string_view str) {
    assert(!str.empty());
    return (str[0] == '\\' && (str.size() == 1 || !isNewline(str[1])));
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

  const std::string_view str_;
  std::string_view remaining_;

  std::optional<Token> nextToken_;
};

}  // namespace details
}  // namespace donner::css
