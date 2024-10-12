#pragma once

#include <concepts>  // IWYU pragma: keep, std::same_as

namespace donner::css {

// Forward declaration.
struct Token;

}  // namespace donner::css

namespace donner::css::parser::details {

enum class ParseMode { Keep, Discard };

template <typename T, typename TokenType = Token>
concept TokenizerLike = requires(T t) {
  { t.next() } -> std::same_as<TokenType>;
  { t.isEOF() } -> std::same_as<bool>;
};

}  // namespace donner::css::parser::details
