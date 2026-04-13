#pragma once
/// @file
/// @brief Pull-based cursor over a sequence of \ref donner::css::ComponentValue.
///
/// See `docs/design_docs/0019-css_token_stream.md` for the design rationale. The
/// short version: CSS subparsers (currently \ref donner::css::parser::SelectorParser,
/// planned for others) walk a pre-materialized list of ComponentValues with a
/// one-way cursor, peeking ahead by small constant offsets and never rewinding.
/// `ComponentValueStream` lifts that cursor into a named, testable abstraction
/// so the parser code reads as "peek, check, advance" rather than as raw span
/// arithmetic.

#include <cstddef>
#include <optional>
#include <span>

#include "donner/base/FileOffset.h"
#include "donner/css/ComponentValue.h"
#include "donner/css/Token.h"

namespace donner::css::parser::details {

/**
 * Pull-based cursor over a contiguous sequence of \ref ComponentValue, wrapping a
 * `std::span<const ComponentValue>` with a peek/advance API.
 *
 * The stream is **one-way**: once advanced past an element, that element cannot
 * be revisited. Subparsers that need to decide among alternatives should peek at
 * small forward offsets (`peekAs<T>(1)`, `peekIs<T>(2)`) and only `advance` when
 * they commit to a consumption.
 *
 * Lifetime: the referenced span must outlive the stream. Typically the underlying
 * `std::vector<ComponentValue>` is owned by the caller.
 */
class ComponentValueStream {
public:
  /**
   * Construct a stream over the given span of ComponentValues.
   *
   * @param components The span to iterate over. Must outlive this stream.
   */
  explicit ComponentValueStream(std::span<const ComponentValue> components)
      : components_(components) {}

  /// True if the cursor has reached the end of the input.
  [[nodiscard]] bool isEOF() const { return components_.empty(); }

  /// Number of components remaining from the current cursor position.
  [[nodiscard]] size_t remaining() const { return components_.size(); }

  /**
   * Advance the cursor past `n` components.
   *
   * @param n Number of components to skip. Must be `<= remaining()`.
   */
  void advance(size_t n = 1) { components_ = components_.subspan(n); }

  /**
   * Source offset of the element at the current cursor position, or
   * \ref FileOffset::EndOfString if the stream is at EOF. Used primarily for
   * error reporting.
   */
  [[nodiscard]] FileOffset currentOffset() const {
    return components_.empty() ? FileOffset::EndOfString() : components_.front().sourceOffset();
  }

  /**
   * Peek at the component at offset `n` from the current cursor position without
   * consuming anything.
   *
   * @param n Offset from the current cursor position.
   * @return Pointer to the component, or `nullptr` if `n >= remaining()`.
   */
  [[nodiscard]] const ComponentValue* peek(size_t n = 0) const {
    return n < components_.size() ? &components_[n] : nullptr;
  }

  /**
   * Peek at offset `n` and return the value as a pointer to its inner variant
   * alternative `T` (e.g., `Token`, `Function`, `SimpleBlock`), or `nullptr` if
   * the peeked element is not of that type or the cursor is past EOF.
   */
  template <typename T>
  [[nodiscard]] const T* peekAs(size_t n = 0) const {
    if (n >= components_.size() || !components_[n].is<T>()) {
      return nullptr;
    }
    return &components_[n].get<T>();
  }

  /**
   * True if the element at offset `n` is of the variant alternative `T`.
   *
   * @tparam T One of `Token`, `Function`, `SimpleBlock`.
   */
  template <typename T>
  [[nodiscard]] bool peekIs(size_t n = 0) const {
    return n < components_.size() && components_[n].is<T>();
  }

  /**
   * True if the element at offset `n` is a `Token` of subtype `TokenType`.
   *
   * This is the common case for subparser grammar checks ("is the next thing an
   * identifier?"). Equivalent to `peekIs<Token>(n) && peekAs<Token>(n)->is<TokenType>()`
   * but more compact at the call site.
   *
   * @tparam TokenType One of `Token::Ident`, `Token::Whitespace`, `Token::Colon`, etc.
   */
  template <typename TokenType>
  [[nodiscard]] bool peekIsToken(size_t n = 0) const {
    return n < components_.size() && components_[n].isToken<TokenType>();
  }

  /**
   * If the element at offset `n` is a `Token::Delim`, return its character value;
   * otherwise return `std::nullopt`.
   */
  [[nodiscard]] std::optional<char> peekDelim(size_t n = 0) const {
    if (const Token* token = peekAs<Token>(n)) {
      if (const auto* delim = token->tryGet<Token::Delim>()) {
        return delim->value;
      }
    }
    return std::nullopt;
  }

  /**
   * True if the element at offset `n` is a `Token::Delim` with the given character value.
   */
  [[nodiscard]] bool peekDelimIs(char value, size_t n = 0) const {
    return peekDelim(n) == value;
  }

  /**
   * Consume leading `Token::Whitespace` components until a non-whitespace component
   * or EOF is reached.
   */
  void skipWhitespace() {
    while (!components_.empty() && components_.front().isToken<Token::Whitespace>()) {
      components_ = components_.subspan(1);
    }
  }

private:
  std::span<const ComponentValue> components_;
};

}  // namespace donner::css::parser::details
