#pragma once
/// @file

#include <string_view>

#include "donner/base/RcString.h"
#include "donner/css/Token.h"

namespace donner::css::parser::details {

/**
 * Tokenizer for CSS, which is called internally by parsers, based on the CSS3 spec:
 * https://www.w3.org/TR/2021/CRD-css-syntax-3-20211224/
 *
 * Compared to the spec, this implementation does not perform the preprocessing step,
 * https://www.w3.org/TR/2021/CRD-css-syntax-3-20211224/#input-preprocessing, which would do the
 * following things:
 * - Simplify newline codepoints, collapsing newline `\r`, `\r\n`, and `\f` into `\n`.
 * - Replace some unicode codepoints, such as U+0000 NULL with U+FFFD REPLACEMENT CHARACTER.
 *
 * As a result, the tokens may include codepoints such as `\r\n` and `\0`, which would not be
 * present in other parsers.
 */
class Tokenizer {
public:
  /**
   * Create the tokenizer with a string to tokenize.
   *
   * @param str The string to tokenize.
   */
  explicit Tokenizer(std::string_view str);

  /// Destructor.
  ~Tokenizer();

  // No copy or move.
  Tokenizer(const Tokenizer&) = delete;
  Tokenizer& operator=(const Tokenizer&) = delete;
  Tokenizer(Tokenizer&&) = delete;
  Tokenizer& operator=(Tokenizer&&) = delete;

  /**
   * Get the next token from the input string. If the end of the input is reached, the token will be
   * of type \ref Token::EofToken.
   *
   * @return Token from the input string.
   */
  Token next();

  /**
   * Returns true if the tokenizer has reached the end of the input string. If \ref next() is
   * called, it will return \ref Token::EofToken.
   */
  bool isEOF() const;

private:
  /// Get the current offset in the input string.
  size_t currentOffset() const;

  /**
   * Create a token and consume \ref length characters from the input string.
   *
   * @tparam T Type of the token.
   * @tparam Args Types of the arguments to pass to the token constructor.
   * @param length Number of characters to consume.
   * @param args Arguments to pass to the token constructor.
   */
  template <typename T, typename... Args>
  Token token(size_t length, Args... args) {
    const size_t offset = currentOffset();
    remaining_.remove_prefix(length);
    return Token(T(std::forward<Args>(args)...), offset);
  }

  /// Consume and discard comments in the input string. Returns an error token if the comment is
  /// unterminated.
  std::optional<Token::ErrorToken> consumeComments();

  /// Consume a whitespace token.
  Token consumeWhitespace();

  /// Consume a string token per https://www.w3.org/TR/css-syntax-3/#consume-a-string-token
  Token consumeQuotedString();

  /// Consume a numeric token, per https://www.w3.org/TR/css-syntax-3/#consume-numeric-token
  Token consumeNumericToken();

  /// Consume an ident-like token, per https://www.w3.org/TR/css-syntax-3/#consume-ident-like-token
  Token consumeIdentLikeToken();

  /// Consume a url token, per https://www.w3.org/TR/css-syntax-3/#consume-url-token
  Token consumeUrlToken(const std::string_view afterUrl, size_t charsConsumedBefore);

  /// Consume the remnants of a bad url, per
  /// https://www.w3.org/TR/css-syntax-3/#consume-remnants-of-bad-url
  Token consumeRemnantsOfBadUrl(const std::string_view badUrl, size_t charsConsumedBefore);

  std::string_view str_;  ///< Original input string.
  std::string_view
      remaining_;  ///< Remaining input string, as the tokenizer advances this is updated.

  std::optional<Token> nextToken_;  ///< Next token to return, if already computed.
};

}  // namespace donner::css::parser::details
