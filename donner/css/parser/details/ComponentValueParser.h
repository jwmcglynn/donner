#pragma once

#include <concepts>
#include <functional>

#include "donner/css/Declaration.h"
#include "donner/css/Token.h"
#include "donner/css/parser/details/Common.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser::details {

enum class WhitespaceHandling { Keep, TrimLeadingAndTrailing };

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
Function consumeFunction(T& tokenizer, Token::Function&& functionToken,
                         const parser::FileOffset& offset, ParseMode mode);
template <TokenizerLike<Token> T>
SimpleBlock consumeSimpleBlock(T& tokenizer, Token&& firstToken, ParseMode mode);

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

}  // namespace donner::css::parser::details
