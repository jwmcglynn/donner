#pragma once

#include "src/css/declaration.h"
#include "src/css/parser/details/tokenizer.h"
#include "src/css/token.h"

namespace donner {
namespace css {
namespace details {

enum class ParseMode { Keep, Discard };

template <typename T>
concept TokenizerLike = requires(T t) {
  // clang-format off
  { t.next() } -> std::same_as<Token>;
  { t.isEOF() } -> std::same_as<bool>;
  // clang-format on
};

static inline TokenIndex simpleBlockEnding(TokenIndex startTokenIndex) {
  if (startTokenIndex == Token::indexOf<Token::CurlyBracket>()) {
    return Token::indexOf<Token::CloseCurlyBracket>();
  } else if (startTokenIndex == Token::indexOf<Token::SquareBracket>()) {
    return Token::indexOf<Token::CloseSquareBracket>();
  } else if (startTokenIndex == Token::indexOf<Token::Parenthesis>()) {
    return Token::indexOf<Token::CloseParenthesis>();
  } else {
    assert(false && "Should be unreachable");
  }
}

// Forward declarations.
template <TokenizerLike T>
SimpleBlock consumeSimpleBlock(T& tokenizer, Token&& firstToken, ParseMode mode);
template <TokenizerLike T>
Function consumeFunction(T& tokenizer, Token::Function&& functionToken, ParseMode mode);

/// Consume a component value, per https://www.w3.org/TR/css-syntax-3/#consume-component-value
template <TokenizerLike T>
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
        consumeFunction(tokenizer, std::move(token.get<Token::Function>()), mode));
  } else {
    return ComponentValue(token);
  }
}

/// Consume a simple block, per https://www.w3.org/TR/css-syntax-3/#consume-simple-block
template <TokenizerLike T>
SimpleBlock consumeSimpleBlock(T& tokenizer, Token&& firstToken, ParseMode mode) {
  const TokenIndex endingTokenIndex = simpleBlockEnding(firstToken.tokenIndex());
  SimpleBlock result(firstToken.tokenIndex());

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
template <TokenizerLike T>
Function consumeFunction(T& tokenizer, Token::Function&& functionToken, ParseMode mode) {
  Function result(std::move(functionToken.name));

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

/// Consume a declaration, per https://www.w3.org/TR/css-syntax-3/#consume-declaration
template <TokenizerLike T>
std::optional<Declaration> consumeDeclaration(T& tokenizer, Token::Ident&& ident) {
  Declaration declaration(std::move(ident.value));

  std::vector<ComponentValue> rawValues;

  while (!tokenizer.isEOF()) {
    Token token = tokenizer.next();

    if (token.is<Token::Whitespace>()) {
      // While the next input token is a <whitespace-token>, consume the next input token.
      continue;
    } else if (!token.is<Token::Colon>()) {
      // If the next input token is anything other than a <colon-token>, this is a parse error.
      // Return nothing.
      return std::nullopt;
    } else {
      break;
    }
  }

  bool lastWasImportantBang = false;
  while (!tokenizer.isEOF()) {
    Token token = tokenizer.next();

    if (token.is<Token::Whitespace>()) {
      // While the next input token is a <whitespace-token>, consume the next input token.
      lastWasImportantBang = false;
    } else {
      // As long as the next input token is anything other than an <EOF-token>, consume a
      // component value and append it to the declaration's value.
      auto componentValue = consumeComponentValue(tokenizer, std::move(token), ParseMode::Keep);

      // Scan for important.
      if (Token* valueToken = std::get_if<Token>(&componentValue.value)) {
        if (lastWasImportantBang && valueToken->is<Token::Ident>() &&
            stringLowercaseEq(valueToken->get<Token::Ident>().value, "important")) {
          declaration.important = true;
          lastWasImportantBang = false;
        } else {
          lastWasImportantBang =
              (valueToken->is<Token::Delim>() && valueToken->get<Token::Delim>().value == '!');
          declaration.important = false;
        }
      } else {
        lastWasImportantBang = false;
        declaration.important = false;
      }

      declaration.values.emplace_back(std::move(componentValue));
    }
  }

  if (declaration.important) {
    assert(declaration.values.size() >= 2);
    declaration.values.pop_back();
    declaration.values.pop_back();
  }

  return declaration;
}

}  // namespace details
}  // namespace css
}  // namespace donner
