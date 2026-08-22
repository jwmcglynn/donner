#pragma once
/// @file

#include <cassert>
#include <coroutine>
#include <optional>
#include <string_view>
#include <utility>

#include "donner/base/Utils.h"
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
  /// Coroutine which lazily yields tokens produced by \ref consumeNextToken().
  class TokenGenerator {
  public:
    class Promise;
    using promise_type = Promise;
    using Handle = std::coroutine_handle<Promise>;

    /// Construct a generator which owns \p coroutine.
    explicit TokenGenerator(Handle coroutine) : coroutine_(coroutine) {}

    /// Destroy the coroutine frame.
    ~TokenGenerator() {
      if (coroutine_) {
        coroutine_.destroy();
      }
    }

    TokenGenerator(const TokenGenerator&) = delete;
    TokenGenerator& operator=(const TokenGenerator&) = delete;

    /// Transfer ownership of a coroutine frame.
    TokenGenerator(TokenGenerator&& other) noexcept
        : coroutine_(std::exchange(other.coroutine_, nullptr)) {}

    TokenGenerator& operator=(TokenGenerator&&) = delete;

    /// Resume until the next token is yielded or the coroutine finishes.
    [[nodiscard]] bool next() {
      if (!coroutine_ || coroutine_.done()) {
        return false;
      }

      coroutine_.resume();
      return !coroutine_.done();
    }

    /// Move the most recently yielded token out of the coroutine frame.
    Token takeValue() {
      assert(coroutine_ && !coroutine_.done());
      return std::move(coroutine_.promise().currentToken_.value());
    }

    /// State and behavior used by the compiler-generated coroutine frame.
    class Promise {
    public:
      TokenGenerator get_return_object() noexcept {
        return TokenGenerator(Handle::from_promise(*this));
      }

      std::suspend_always initial_suspend() const noexcept { return {}; }
      std::suspend_always final_suspend() const noexcept { return {}; }
      void return_void() const noexcept {}

      std::suspend_always yield_value(Token token) noexcept {
        currentToken_.emplace(std::move(token));
        return {};
      }

      [[noreturn]] void unhandled_exception() const {
        UTILS_RELEASE_ASSERT_MSG(false, "Unhandled exception in CSS TokenGenerator");
      }

    private:
      std::optional<Token> currentToken_;
      friend class TokenGenerator;
    };

  private:
    Handle coroutine_;
  };

  /// Create the lazy token generator for this tokenizer.
  TokenGenerator generate();

  /// Consume and return one token from the input string.
  Token consumeNextToken();

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

  /// Consume punctuation whose token kind depends only on the current character.
  std::optional<Token> consumeSimpleToken(char ch);

  /// Dispatch punctuation which requires additional input inspection.
  Token consumeSpecialToken(char ch);

  /// Consume a hash token or a hash delimiter.
  Token consumeHashToken();

  /// Consume a number or the current delimiter.
  Token consumeNumberOrDelimToken();

  /// Consume a number, CDC, identifier, or hyphen delimiter.
  Token consumeHyphenToken();

  /// Consume a CDO token or a less-than delimiter.
  Token consumeLessThanToken();

  /// Consume an at-keyword or an at-sign delimiter.
  Token consumeAtToken();

  /// Consume an escaped identifier or a reverse-solidus delimiter.
  Token consumeReverseSolidusToken();

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
  TokenGenerator generator_;        ///< Lazily produces one token per resume.
};

}  // namespace donner::css::parser::details
