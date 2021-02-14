#pragma once

#include <gmock/gmock.h>

#include <ostream>
#include <type_traits>

#include "src/css/declaration.h"
#include "src/css/stylesheet.h"
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
void PrintTo(const Function& func, std::ostream* os);
void PrintTo(const SimpleBlock& block, std::ostream* os);
void PrintTo(const ComponentValue& component, std::ostream* os);

void PrintTo(const Declaration& declaration, std::ostream* os);
void PrintTo(const AtRule& rule, std::ostream* os);
void PrintTo(const InvalidRule& invalidRule, std::ostream* os);
void PrintTo(const DeclarationOrAtRule& declOrAt, std::ostream* os);
void PrintTo(const QualifiedRule& qualifiedRule, std::ostream* os);
void PrintTo(const Rule& rule, std::ostream* os);

MATCHER_P(TokenIsImpl, token, "") {
  using TokenType = std::remove_cvref_t<decltype(token)>;
  using ArgType = std::remove_cvref_t<decltype(arg)>;

  if constexpr (std::is_same_v<ArgType, ComponentValue> ||
                std::is_same_v<ArgType, DeclarationOrAtRule>) {
    if (const Token* argToken = std::get_if<Token>(&arg.value)) {
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
auto TokenIsErrorToken(Args... args) {
  return TokenIsImpl(Token::ErrorToken(std::forward<Args>(args)...));
}

template <typename... Args>
auto TokenIsEofToken(Args... args) {
  return TokenIsImpl(Token::EofToken(std::forward<Args>(args)...));
}

MATCHER_P3(DeclarationIsImpl, nameMatcher, valuesMatcher, importantMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;
  const Declaration* decl = nullptr;

  if constexpr (std::is_same_v<ArgType, DeclarationOrAtRule>) {
    decl = std::get_if<Declaration>(&arg.value);
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
  return DeclarationIsImpl(nameMatcher, valuesMatcher, false);
}

MATCHER_P3(AtRuleIsImpl, nameMatcher, preludeMatcher, blockMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;
  const AtRule* rule = nullptr;

  if constexpr (std::is_same_v<ArgType, DeclarationOrAtRule> || std::is_same_v<ArgType, Rule>) {
    rule = std::get_if<AtRule>(&arg.value);
  } else {
    rule = &arg;
  }

  if (!rule) {
    return false;
  }

  return testing::ExplainMatchResult(nameMatcher, rule->name, result_listener) &&
         testing::ExplainMatchResult(preludeMatcher, rule->prelude, result_listener) &&
         testing::ExplainMatchResult(blockMatcher, rule->block, result_listener);
}

template <typename NameMatcher, typename PreludeMatcher, typename BlockMatcher>
auto AtRuleIs(NameMatcher nameMatcher, PreludeMatcher preludeMatcher, BlockMatcher blockMatcher) {
  return AtRuleIsImpl(nameMatcher, preludeMatcher, testing::Optional(blockMatcher));
}

template <typename NameMatcher, typename PreludeMatcher>
auto AtRuleIs(NameMatcher nameMatcher, PreludeMatcher preludeMatcher) {
  return AtRuleIsImpl(nameMatcher, preludeMatcher, testing::Eq(std::nullopt));
}

MATCHER(InvalidRuleTypeImpl, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;
  const InvalidRule* rule = nullptr;

  if constexpr (std::is_same_v<ArgType, DeclarationOrAtRule>) {
    rule = std::get_if<InvalidRule>(&arg.value);
  } else {
    rule = &arg;
  }

  return rule != nullptr;
}

inline auto InvalidRuleType() {
  return InvalidRuleTypeImpl();
}

MATCHER_P2(FunctionIs, nameMatcher, valuesMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;
  const Function* func = nullptr;

  if constexpr (std::is_same_v<ArgType, ComponentValue>) {
    func = std::get_if<Function>(&arg.value);
  } else {
    func = &arg;
  }

  if (!func) {
    return false;
  }

  return testing::ExplainMatchResult(nameMatcher, func->name, result_listener) &&
         testing::ExplainMatchResult(valuesMatcher, func->values, result_listener);
}

MATCHER_P2(SimpleBlockIs, associatedTokenMatcher, valuesMatcher, "") {
  using ArgType = std::remove_cvref_t<decltype(arg)>;
  const SimpleBlock* block = nullptr;

  if constexpr (std::is_same_v<ArgType, ComponentValue>) {
    block = std::get_if<SimpleBlock>(&arg.value);
  } else {
    block = &arg;
  }

  if (!block) {
    return false;
  }

  return testing::ExplainMatchResult(associatedTokenMatcher, block->associatedToken,
                                     result_listener) &&
         testing::ExplainMatchResult(valuesMatcher, block->values, result_listener);
}

template <typename ValuesMatcher>
auto SimpleBlockIsCurly(ValuesMatcher valuesMatcher) {
  return SimpleBlockIs(Token::indexOf<Token::CurlyBracket>(), valuesMatcher);
}

template <typename ValuesMatcher>
auto SimpleBlockIsSquare(ValuesMatcher valuesMatcher) {
  return SimpleBlockIs(Token::indexOf<Token::SquareBracket>(), valuesMatcher);
}

template <typename ValuesMatcher>
auto SimpleBlockIsParenthesis(ValuesMatcher valuesMatcher) {
  return SimpleBlockIs(Token::indexOf<Token::Parenthesis>(), valuesMatcher);
}

}  // namespace css
}  // namespace donner
