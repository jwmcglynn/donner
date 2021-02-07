#pragma once

#include <gmock/gmock.h>

#include <ostream>
#include <type_traits>

#include "src/css/declaration.h"
#include "src/css/token.h"

namespace donner {
namespace css {

namespace details {

template <class T>
struct is_variant : std::false_type {};

template <class... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};

}  // namespace details

void PrintTo(const Token& token, std::ostream* os);

MATCHER_P(TokenIsImpl, token, "") {
  using TokenType = std::remove_cvref_t<decltype(token)>;

  if constexpr (details::is_variant<std::remove_cvref_t<decltype(arg)>>::value) {
    if (const Token* argToken = std::get_if<Token>(&arg)) {
      if (argToken->is<TokenType>()) {
        return argToken->get<TokenType>() == token;
      }
    }
  } else {
    if (arg.template is<TokenType>()) {
      return arg.template get<TokenType>() == token;
    }
  }

  return false;
}

/**
 * Given a Token or a variant containing a token, matches if the token is equal, ignoring the
 * offset.
 *
 * @param token Token subclass to match against.
 */
template <typename T, typename... Args>
auto TokenIs(Args... args) {
  return TokenIsImpl(T(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsIdent(Args... args) {
  return TokenIsImpl(Token::Ident(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsFunction(Args... args) {
  return TokenIsImpl(Token::Function(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsAtKeyword(Args... args) {
  return TokenIsImpl(Token::AtKeyword(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsHash(Args... args) {
  return TokenIsImpl(Token::Hash(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsString(Args... args) {
  return TokenIsImpl(Token::String(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsBadString(Args... args) {
  return TokenIsImpl(Token::BadString(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsUrl(Args... args) {
  return TokenIsImpl(Token::Url(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsBadUrl(Args... args) {
  return TokenIsImpl(Token::BadUrl(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsDelim(Args... args) {
  return TokenIsImpl(Token::Delim(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsNumber(Args... args) {
  return TokenIsImpl(Token::Number(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsPercentage(Args... args) {
  return TokenIsImpl(Token::Percentage(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsDimension(Args... args) {
  return TokenIsImpl(Token::Dimension(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsWhitespace(Args... args) {
  return TokenIsImpl(Token::Whitespace(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsCDO(Args... args) {
  return TokenIsImpl(Token::CDO(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsCDC(Args... args) {
  return TokenIsImpl(Token::CDC(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsColon(Args... args) {
  return TokenIsImpl(Token::Colon(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsSemicolon(Args... args) {
  return TokenIsImpl(Token::Semicolon(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsComma(Args... args) {
  return TokenIsImpl(Token::Comma(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsSquareBracket(Args... args) {
  return TokenIsImpl(Token::SquareBracket(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsParenthesis(Args... args) {
  return TokenIsImpl(Token::Parenthesis(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsCurlyBracket(Args... args) {
  return TokenIsImpl(Token::CurlyBracket(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsCloseSquareBracket(Args... args) {
  return TokenIsImpl(Token::CloseSquareBracket(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsCloseParenthesis(Args... args) {
  return TokenIsImpl(Token::CloseParenthesis(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsCloseCurlyBracket(Args... args) {
  return TokenIsImpl(Token::CloseCurlyBracket(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsEOFToken(Args... args) {
  return TokenIsImpl(Token::EOFToken(std::forward<Args>(args)...));
}

MATCHER_P3(DeclarationIsImpl, nameMatcher, valuesMatcher, importantMatcher, "") {
  const Declaration* decl = nullptr;

  if constexpr (details::is_variant<std::remove_cvref_t<decltype(arg)>>::value) {
    decl = std::get_if<Declaration>(&arg);
  } else {
    decl = &arg;
  }

  if (!decl) {
    return false;
  }

  return testing::ExplainMatchResult(nameMatcher, decl->name, result_listener) &&
         testing::ExplainMatchResult(valuesMatcher, decl->values, result_listener) &&
         testing::ExplainMatchResult(importantMatcher, decl->important, result_listener);
}

template <typename NameMatcher, typename ValuesMatcher, typename ImportantMatcher>
auto DeclarationIs(NameMatcher nameMatcher, ValuesMatcher valuesMatcher,
                   ImportantMatcher importantMatcher) {
  return DeclarationIsImpl(nameMatcher, valuesMatcher, importantMatcher);
}

template <typename NameMatcher, typename ValuesMatcher>
auto DeclarationIs(NameMatcher nameMatcher, ValuesMatcher valuesMatcher) {
  return DeclarationIs(nameMatcher, valuesMatcher, false);
}

}  // namespace css
}  // namespace donner
