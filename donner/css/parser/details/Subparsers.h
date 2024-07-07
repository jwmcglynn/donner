#pragma once

#include <concepts>
#include <functional>

#include "donner/css/Declaration.h"
#include "donner/css/Token.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser::details {

enum class ParseMode { Keep, Discard };

enum class WhitespaceHandling { Keep, TrimLeadingAndTrailing };

template <typename T, typename TokenType = Token>
concept TokenizerLike = requires(T t) {
  { t.next() } -> std::same_as<TokenType>;
  { t.isEOF() } -> std::same_as<bool>;
};

[[maybe_unused]] static inline TokenIndex simpleBlockEnding(TokenIndex startTokenIndex) {
  if (startTokenIndex == Token::indexOf<Token::CurlyBracket>()) {
    return Token::indexOf<Token::CloseCurlyBracket>();
  } else if (startTokenIndex == Token::indexOf<Token::SquareBracket>()) {
    return Token::indexOf<Token::CloseSquareBracket>();
  } else if (startTokenIndex == Token::indexOf<Token::Parenthesis>()) {
    return Token::indexOf<Token::CloseParenthesis>();
  } else {
    UTILS_UNREACHABLE();
  }
}

// Forward declarations.
template <TokenizerLike<Token> T>
SimpleBlock consumeSimpleBlock(T& tokenizer, Token&& firstToken, ParseMode mode);
template <TokenizerLike<Token> T>
Function consumeFunction(T& tokenizer, Token::Function&& functionToken,
                         const parser::FileOffset& offset, ParseMode mode);

/// Consume a component value, per https://www.w3.org/TR/css-syntax-3/#consume-component-value
template <TokenizerLike<Token> T>
ComponentValue consumeComponentValue(T& tokenizer, Token&& token, ParseMode mode) {
  if (token.is<Token::CurlyBracket>() || token.is<Token::SquareBracket>() ||
      token.is<Token::Parenthesis>()) {
    // If the current input token is a <{-token>, <[-token>, or <(-token>, consume a simple
    // block and return it.
    return ComponentValue(consumeSimpleBlock(tokenizer, std::move(token), mode));
  } else if (token.is<Token::Function>()) {
    // Otherwise, if the current input token is a <function-token>, consume a function and
    // return it.
    return ComponentValue(
        consumeFunction(tokenizer, std::move(token.get<Token::Function>()), token.offset(), mode));
  } else {
    return ComponentValue(token);
  }
}

/// Parse a list of component values, per
/// https://www.w3.org/TR/css-syntax-3/#parse-list-of-component-values
template <TokenizerLike<Token> T>
std::vector<ComponentValue> parseListOfComponentValues(
    T& tokenizer, WhitespaceHandling whitespace = WhitespaceHandling::Keep) {
  std::vector<ComponentValue> result;
  while (!tokenizer.isEOF()) {
    Token token = tokenizer.next();
    if (whitespace == WhitespaceHandling::TrimLeadingAndTrailing && result.empty() &&
        token.is<Token::Whitespace>()) {
      continue;
    }

    if (!token.is<Token::EofToken>()) {
      result.emplace_back(consumeComponentValue(tokenizer, std::move(token), ParseMode::Keep));
    }
  }

  if (whitespace == WhitespaceHandling::TrimLeadingAndTrailing) {
    while (!result.empty() && result.back().isToken<Token::Whitespace>()) {
      result.pop_back();
    }
  }

  return result;
}

/// Consume a simple block, per https://www.w3.org/TR/css-syntax-3/#consume-simple-block
template <TokenizerLike<Token> T>
SimpleBlock consumeSimpleBlock(T& tokenizer, Token&& firstToken, ParseMode mode) {
  const TokenIndex endingTokenIndex = simpleBlockEnding(firstToken.tokenIndex());
  SimpleBlock result(firstToken.tokenIndex(), firstToken.offset());

  while (!tokenizer.isEOF()) {
    auto token = tokenizer.next();
    if (token.tokenIndex() == endingTokenIndex) {
      return result;
    } else {
      // anything else: Reconsume the current input token. Consume a component value and append
      // it to the value of the block.
      auto component = consumeComponentValue(tokenizer, std::move(token), mode);
      if (mode == ParseMode::Keep) {
        result.values.emplace_back(std::move(component));
      }
    }
  }

  // <EOF-token>: This is a parse error. Return the block.
  return result;
}

/// Consume a function, per https://www.w3.org/TR/css-syntax-3/#consume-function
template <TokenizerLike<Token> T>
Function consumeFunction(T& tokenizer, Token::Function&& functionToken,
                         const parser::FileOffset& offset, ParseMode mode) {
  Function result(std::move(functionToken.name), offset);

  while (!tokenizer.isEOF()) {
    Token token = tokenizer.next();
    if (token.is<Token::CloseParenthesis>()) {
      return result;
    } else {
      // anything else: Reconsume the current input token. Consume a component value and append
      // the returned value to the function's value.
      auto componentValue = consumeComponentValue(tokenizer, std::move(token), mode);

      if (mode == ParseMode::Keep) {
        result.values.emplace_back(std::move(componentValue));
      }
    }
  }

  // <EOF-token>: This is a parse error. Return the function.
  return result;
}

/// Consume an at-rule, per https://www.w3.org/TR/css-syntax-3/#consume-at-rule
template <TokenizerLike<Token> T>
AtRule consumeAtRule(T& tokenizer, Token::AtKeyword&& atKeyword, ParseMode mode) {
  AtRule result(std::move(atKeyword.value));

  while (!tokenizer.isEOF()) {
    Token token = tokenizer.next();

    if (token.template is<Token::Semicolon>()) {
      // Return the at-rule.
      return result;
    } else if (token.template is<Token::CurlyBracket>()) {
      // <{-token>: Consume a simple block and assign it to the at-rule's block. Return the
      // at-rule.
      result.block = consumeSimpleBlock(tokenizer, std::move(token), mode);
      return result;
    } else {
      // anything else: Reconsume the current input token. Consume a component value. Append the
      // returned value to the at-rule's prelude.
      auto component = consumeComponentValue(tokenizer, std::move(token), mode);

      if (mode == ParseMode::Keep) {
        result.prelude.emplace_back(std::move(component));
      }
    }
  }

  // <EOF-token>: This is a parse error. Return the at-rule.
  return result;
}

template <typename T, typename U>
concept DecayedSameAs = std::is_same_v<std::decay_t<T>, U>;

template <typename T, typename ItemType>
concept DeclarationTokenizerItem = requires(T t, ParseMode parseMode) {
  { t.value } -> DecayedSameAs<ItemType>;
  { t.offset() } -> std::same_as<parser::FileOffset>;
  { t.asComponentValue(parseMode) } -> std::same_as<ComponentValue>;
};

template <typename T>
concept DeclarationTokenizer = requires(T t) {
  { t.next() } -> DeclarationTokenizerItem<typename T::ItemType>;
  { t.isEOF() } -> std::same_as<bool>;
};

template <TokenizerLike<Token> T>
struct DeclarationTokenTokenizer {
  struct Item {
    Token value;
    std::reference_wrapper<T> tokenizer;

    Item(Token&& value, T& tokenizer) : value(std::move(value)), tokenizer(tokenizer) {}

    // Default copy and move.
    Item(Item&&) noexcept = default;
    Item& operator=(Item&&) noexcept = default;

    Item(const Item& other) = default;
    Item& operator=(const Item& other) = default;

    template <typename TokenType>
    bool isToken() const {
      return value.is<TokenType>();
    }

    parser::FileOffset offset() const { return value.offset(); }

    ComponentValue asComponentValue(ParseMode parseMode = ParseMode::Keep) {
      return consumeComponentValue(tokenizer.get(), std::move(value), parseMode);
    }
  };

  using ItemType = Token;

  explicit DeclarationTokenTokenizer(T& tokenizer) : tokenizer_(tokenizer) {}

  bool isEOF() const { return tokenizer_.isEOF(); }
  Item next() { return Item(tokenizer_.next(), tokenizer_); }

private:
  T& tokenizer_;
};

template <TokenizerLike<ComponentValue> T>
struct DeclarationComponentValueTokenizer {
  struct Item {
    ComponentValue value;

    template <typename TokenType>
    bool isToken() const {
      return value.isToken<TokenType>();
    }

    parser::FileOffset offset() const { return value.sourceOffset(); }

    ComponentValue asComponentValue(ParseMode parseMode = ParseMode::Keep) {
      return std::move(value);
    }
  };

  using ItemType = ComponentValue;

  explicit DeclarationComponentValueTokenizer(T& tokenizer) : tokenizer_(tokenizer) {}

  bool isEOF() const { return tokenizer_.isEOF(); }
  Item next() { return Item{tokenizer_.next()}; }

private:
  T& tokenizer_;
};

/// Consume a declaration, per https://www.w3.org/TR/css-syntax-3/#consume-declaration
template <DeclarationTokenizer T>
std::optional<Declaration> consumeDeclarationGeneric(T& tokenizer, Token::Ident&& ident,
                                                     const parser::FileOffset& offset) {
  {
    bool hadColon = false;

    while (!tokenizer.isEOF() && !hadColon) {
      typename T::Item item = tokenizer.next();

      if (item.template isToken<Token::Whitespace>()) {
        // While the next input token is a <whitespace-token>, consume the next input token.
        continue;
      } else if (!item.template isToken<Token::Colon>()) {
        // If the next input token is anything other than a <colon-token>, this is a parse error.
        // Return nothing.
        return std::nullopt;
      } else {
        hadColon = true;
      }
    }

    if (!hadColon) {
      return std::nullopt;
    }
  }

  Declaration declaration(std::move(ident.value), {}, offset);

  bool lastWasImportantBang = false;
  bool hitNonWhitespace = false;
  int trailingWhitespace = 0;

  while (!tokenizer.isEOF()) {
    typename T::Item token = tokenizer.next();

    if (token.template isToken<Token::Whitespace>()) {
      // While the next input token is a <whitespace-token>, consume the next input token.
      if (hitNonWhitespace) {
        declaration.values.emplace_back(std::move(token.asComponentValue()));
        ++trailingWhitespace;
      }
    } else {
      hitNonWhitespace = true;

      // As long as the next input token is anything other than an <EOF-token>, consume a
      // component value and append it to the declaration's value.
      auto componentValue = token.asComponentValue();

      // Scan for important.
      if (Token* valueToken = std::get_if<Token>(&componentValue.value)) {
        if (lastWasImportantBang && valueToken->is<Token::Ident>() &&
            valueToken->get<Token::Ident>().value.equalsLowercase("important")) {
          declaration.important = true;
          lastWasImportantBang = false;
        } else {
          lastWasImportantBang =
              (valueToken->is<Token::Delim>() && valueToken->get<Token::Delim>().value == '!');
          if (!lastWasImportantBang || declaration.important) {
            trailingWhitespace = 0;
          }
          declaration.important = false;
        }
      } else {
        lastWasImportantBang = false;
        declaration.important = false;
        trailingWhitespace = 0;
      }

      declaration.values.emplace_back(std::move(componentValue));
    }
  }

  if (declaration.important) {
    assert(declaration.values.size() >= 2);
    declaration.values.pop_back();
    declaration.values.pop_back();
  }

  while (trailingWhitespace > 0) {
    --trailingWhitespace;
    declaration.values.pop_back();
  }

  return declaration;
}

/// Consume a declaration, per https://www.w3.org/TR/css-syntax-3/#consume-declaration
template <TokenizerLike<Token> T>
std::optional<Declaration> consumeDeclaration(T& tokenizer, Token::Ident&& ident,
                                              const parser::FileOffset& offset) {
  DeclarationTokenTokenizer declarationTokenizer(tokenizer);
  return consumeDeclarationGeneric(declarationTokenizer, std::move(ident), offset);
}

/// Consume a declaration, starting with an partially parsed set of ComponentValues.
template <TokenizerLike<ComponentValue> T>
std::optional<Declaration> consumeDeclaration(T& tokenizer, Token::Ident&& ident,
                                              const parser::FileOffset& offset) {
  DeclarationComponentValueTokenizer declarationTokenizer(tokenizer);
  return consumeDeclarationGeneric(declarationTokenizer, std::move(ident), offset);
}

}  // namespace donner::css::parser::details
