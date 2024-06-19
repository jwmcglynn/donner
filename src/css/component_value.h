#pragma once
/// @file

#include <iostream>
#include <variant>
#include <vector>

#include "src/base/parser/file_offset.h"
#include "src/base/rc_string.h"
#include "src/css/token.h"

namespace donner::css {

struct ComponentValue;

/**
 * A CSS function, such as `rgb(255, 0, 0)`, parsed into a function name and a list of parameter
 * values.
 *
 * Compared to \ref Token::Function, this has the next level of parsing, where the function's
 * parameters have been extracted as a list of \ref ComponentValue. \ref Token::Function would
 * only include "rgb(" part of the function.
 */
struct Function {
  /// Function name, such as "rgb".
  RcString name;

  /// List of parameter values.
  std::vector<ComponentValue> values;

  /// Offset of the function name in the source string.
  parser::FileOffset sourceOffset;

  /**
   * Construct a new Function object with a name and an empty parameter list. To supply parameters,
   * modify the \ref values vector after construction.
   *
   * @param name Function name.
   * @param sourceOffset Offset of the function name in the source string.
   */
  Function(RcString name, const parser::FileOffset& sourceOffset);

  /// Equality operator.
  bool operator==(const Function& other) const;

  /**
   * Output a human-readable representation of the function to a stream.
   *
   * @param os Output stream.
   * @param func Function to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const Function& func);
};

/**
 * A CSS simple block, such as a rule block or a parenthesized expression. A SimpleBlock may start
 * with either '{', '(' or '[', and ends when the matching closing token is found.
 *
 * For example, the following is a valid simple block: `{ color: red; }`, and so is this within a
 * selector: `[href^="https://"]`.
 */
struct SimpleBlock {
  /**
   * The token that starts the simple block. This is either '{', '[' or '(', which corresponds to
   * \ref Token::CurlyBracket, \ref Token::SquareBracket, and \ref Token::Parenthesis respectively.
   */
  TokenIndex associatedToken;

  /// List of component values inside the simple block.
  std::vector<ComponentValue> values;

  /// Offset of the opening token in the source string.
  parser::FileOffset sourceOffset;

  /**
   * Construct a new SimpleBlock object with an opening token and an empty list of component values.
   * To supply component values, modify the \ref values vector after construction.
   *
   * @param associatedToken The token that starts the simple block.
   * @param sourceOffset Offset of the opening token in the source string.
   */
  explicit SimpleBlock(TokenIndex associatedToken, const parser::FileOffset& sourceOffset);

  /// Equality operator.
  bool operator==(const SimpleBlock& other) const;

  /**
   * Output a human-readable representation of the simple block to a stream.
   *
   * @param os Output stream.
   * @param block Simple block to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const SimpleBlock& block);
};

/**
 * A CSS component value, which is either a token, or a parsed function or block.
 *
 * This is the second level of parsing, after \ref Token. A \ref Token is a single lexical unit, and
 * ComponentValue groups those into logical function and block groups, as well as wrapping
 * standalone \ref Token.
 *
 * ComponentValue is the base component traversed when parsing CSS into logical blocks, such as \ref
 * Selector and \ref donner::svg::Property.
 */
struct ComponentValue {
  /// The type of the component value, which is either a base \ref Token or a logical group of
  /// tokens, \ref Function or \ref SimpleBlock.
  using Type = std::variant<Token, Function, SimpleBlock>;

  /// The actual value of the component value.
  Type value;

  /**
   * Construct a new ComponentValue object, taking ownership of a \ref Token.
   *
   * @param value Token to construct from.
   */
  /* implicit */ ComponentValue(Type&& value);

  // Copy and move constructors.
  ComponentValue(const ComponentValue&) = default;
  ComponentValue& operator=(const ComponentValue&) = default;
  ComponentValue(ComponentValue&&) noexcept = default;
  ComponentValue& operator=(ComponentValue&&) noexcept = default;

  /// Equality operator.
  bool operator==(const ComponentValue& other) const;

  /**
   * Check if the component value is of a given type.
   *
   * For example:
   * ```
   * if (component.is<SimpleBlock>()) {
   *   // ...
   * }
   * ```
   *
   * @tparam T Type to check, one of \ref Type.
   * @return True if the component value is of type T.
   */
  template <typename T>
  bool is() const {
    return std::holds_alternative<T>(value);
  }

  /**
   * Shorthand for checking if this component value holds a specific token.
   *
   * For example:
   * ```
   * if (component.isToken<Token::Percentage>()) {
   * ```
   *
   * Which is equivalent to `component.is<Token>() &&
   * component.get<Token>().is<Token::Percentage>()`.
   *
   * @tparam T Token type to check, which must be within the \ref Token::TokenValue list.
   */
  template <typename T>
  bool isToken() const {
    return is<Token>() && get<Token>().is<T>();
  }

  /**
   * Get the inner token value as a pointer, if the component value is a token and matches the
   * requested type, or `nullptr`.
   *
   * See also \ref Token::tryGet().
   *
   * Example:
   * ```
   * if (const Token::Percentage* percentage = component.tryGetToken<Token::Percentage>()) {
   *   // ...
   * }
   * ```
   *
   * @tparam T Token type to check, which must be within the \ref Token::TokenValue list.
   */
  template <typename T>
  const T* tryGetToken() const {
    if (const Token* token = std::get_if<Token>(&value)) {
      return token->tryGet<T>();
    } else {
      return nullptr;
    }
  }

  /**
   * Get the component value as a reference.
   *
   * Example usage:
   * ```
   * SimpleBlock& block = component.get<SimpleBlock>();
   * block.values.push_back(componentValue);
   * ```
   *
   * @tparam T Type to get, one of \ref Type.
   * @pre The component must be of the given type, i.e. `is<T>()` must be true.
   */
  template <typename T>
  T& get() & {
    return std::get<T>(value);
  }

  /**
   * Get the component value as a const-reference.
   *
   * Example usage:
   * ```
   * const SimpleBlock& block = component.get<SimpleBlock>();
   * ```
   *
   * @tparam T Type to get, one of \ref Type.
   * @pre The component must be of the given type, i.e. `is<T>()` must be true.
   */
  template <typename T>
  const T& get() const& {
    return std::get<T>(value);
  }

  /**
   * Get the component value as an rvalue-reference for move semantics.
   *
   * Example usage:
   * ```
   * SimpleBlock block = std::move(component.get<SimpleBlock>());
   * ```
   *
   * @tparam T Type to get, one of \ref Type.
   * @pre The component must be of the given type, i.e. `is<T>()` must be true.
   */
  template <typename T>
  T&& get() && {
    return std::move(std::get<T>(value));
  }

  /**
   * Get the offset of this component value in the original source. For \ref Function and \ref
   * SimpleBlock, returns the offset of the group opening token.
   */
  parser::FileOffset sourceOffset() const {
    return std::visit(
        [](auto&& v) -> parser::FileOffset {
          using T = std::remove_cvref_t<decltype(v)>;

          if constexpr (std::is_same_v<Token, T>) {
            return v.offset();
          } else {
            return v.sourceOffset;
          }
        },
        value);
  }

  /**
   * Output a human-readable representation of the component value to a stream.
   *
   * @param os Output stream.
   * @param component Component value to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const ComponentValue& component) {
    std::visit([&os](auto&& v) { os << v; }, component.value);
    return os;
  }
};

}  // namespace donner::css
