#include "src/css/parser/declaration_list_parser.h"

#include <cassert>
#include <optional>
#include <span>

#include "src/css/parser/details/subparsers.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner::css {

namespace {

using details::consumeComponentValue;
using details::consumeDeclaration;
using details::consumeSimpleBlock;
using details::ParseMode;

template <typename T>
class SubTokenizer {
public:
  SubTokenizer(std::span<T> elements) : elements_(elements) {}

  T next() {
    assert(!elements_.empty());

    T result(std::move(elements_.front()));
    elements_ = elements_.subspan(1);
    return result;
  }

  bool isEOF() const { return elements_.empty(); }

private:
  std::span<T> elements_;
};

template <typename T>
class TokenizerConvertToToken {
public:
  explicit TokenizerConvertToToken(T& tokenizer) : tokenizer_(tokenizer) {}

  Token next() { return tokenizer_.next().value; }
  bool isEOF() const { return tokenizer_.isEOF(); }

private:
  T& tokenizer_;
};

template <details::DeclarationTokenizer T>
struct ParseUntilSemicolonOrEOF {
  using Item = typename T::Item;
  using ItemType = typename T::ItemType;

  explicit ParseUntilSemicolonOrEOF(T& tokenizer)
      : tokenizer_(tokenizer), next_(tokenizer_.next()) {}

  ~ParseUntilSemicolonOrEOF() {
    while (!isEOF()) {
      next_ = std::move(tokenizer_.next());
    }
  }

  ItemType next() {
    assert(!isEOF());

    ItemType result(std::move(next_.value));
    if (!tokenizer_.isEOF()) {
      next_ = std::move(tokenizer_.next());
    } else {
      eof_ = true;
    }
    return result;
  }

  bool isEOF() const {
    return eof_ || next_.template isToken<Token::Semicolon>() ||
           next_.template isToken<Token::EofToken>();
  }

private:
  T& tokenizer_;
  Item next_;
  bool eof_ = false;
};

template <details::DeclarationTokenizer T>
std::optional<Declaration> parseDeclarationGeneric(T& tokenizer, Token&& token) {
  if (token.is<Token::Ident>()) {
    // <ident-token>: Initialize a temporary list initially filled with the current input token.
    Token::Ident ident = std::move(token.get<Token::Ident>());

    // A declaration list ends when it reaches a <semicolon-token> or <EOF-token>.
    ParseUntilSemicolonOrEOF<T> declarationInputTokenizer(tokenizer);
    return consumeDeclaration(declarationInputTokenizer, std::move(ident), token.offset());
  } else {
    // anything else: This is a parse error. Reconsume the current input token. As long as the
    // next input token is anything other than a <semicolon-token> or <EOF-token>, consume a
    // component value and throw away the returned value.
    if constexpr (std::is_same_v<typename T::ItemType, Token>) {
      // Only consume if we're parsing tokens, not ComponentValues which already did this.
      TokenizerConvertToToken tokenizerConvertToToken(tokenizer);
      std::ignore =
          consumeComponentValue(tokenizerConvertToToken, std::move(token), ParseMode::Discard);
    }

    while (!tokenizer.isEOF()) {
      auto subToken = tokenizer.next();
      if (subToken.template isToken<Token::Semicolon>()) {
        break;
      }

      std::ignore = subToken.asComponentValue(ParseMode::Discard);
    }

    return std::nullopt;
  }
}

class DeclarationListParserImpl {
public:
  DeclarationListParserImpl(std::string_view str) : tokenizer_(str) {}

  std::vector<DeclarationOrAtRule> parse() {
    std::vector<DeclarationOrAtRule> result;

    while (!tokenizer_.isEOF()) {
      auto token = tokenizer_.next();

      if (token.is<Token::AtKeyword>()) {
        // <at-keyword-token>: Reconsume the current input token. Consume an at-rule. Append the
        // returned rule to the list of declarations.
        auto atRule =
            consumeAtRule(tokenizer_, std::move(token.get<Token::AtKeyword>()), ParseMode::Keep);
        result.emplace_back(std::move(atRule));
      } else if (token.is<Token::Whitespace>() || token.is<Token::Semicolon>()) {
        // Skip.
      } else {
        details::DeclarationTokenTokenizer declarationTokenizer(tokenizer_);
        auto maybeDeclaration = parseDeclarationGeneric(declarationTokenizer, std::move(token));

        if (maybeDeclaration.has_value()) {
          result.emplace_back(std::move(maybeDeclaration.value()));
        } else {
          result.emplace_back(InvalidRule());
        }
      }
    }

    return result;
  }

  std::vector<Declaration> parseDeclarations() {
    std::vector<Declaration> result;

    while (!tokenizer_.isEOF()) {
      auto token = tokenizer_.next();

      if (token.is<Token::AtKeyword>()) {
        // <at-keyword-token>: Reconsume the current input token. Consume an at-rule. Append the
        // returned rule to the list of declarations.
        // In this case we ignore the result since only declarations are desired.
        std::ignore =
            consumeAtRule(tokenizer_, std::move(token.get<Token::AtKeyword>()), ParseMode::Discard);
      } else if (token.is<Token::Whitespace>() || token.is<Token::Semicolon>()) {
        // Skip.
      } else {
        details::DeclarationTokenTokenizer declarationTokenizer(tokenizer_);

        auto maybeDeclaration = parseDeclarationGeneric(declarationTokenizer, std::move(token));
        if (maybeDeclaration.has_value()) {
          result.emplace_back(std::move(maybeDeclaration.value()));
        }
      }
    }

    return result;
  }

private:
  details::Tokenizer tokenizer_;
};

}  // namespace

std::vector<DeclarationOrAtRule> DeclarationListParser::Parse(std::string_view str) {
  DeclarationListParserImpl parser(str);
  return parser.parse();
}

std::vector<Declaration> DeclarationListParser::ParseOnlyDeclarations(std::string_view str) {
  DeclarationListParserImpl parser(str);
  return parser.parseDeclarations();
}

std::vector<Declaration> DeclarationListParser::ParseRuleDeclarations(
    std::span<ComponentValue> components) {
  std::vector<Declaration> result;

  SubTokenizer<ComponentValue> tokenizer(components);
  details::DeclarationComponentValueTokenizer declarationTokenizer(tokenizer);

  while (!declarationTokenizer.isEOF()) {
    auto token = declarationTokenizer.next();

    if (token.template isToken<Token::Whitespace>() || token.template isToken<Token::Semicolon>()) {
      // Skip.
    } else if (Token* innerToken = std::get_if<Token>(&token.value.value)) {
      auto maybeDeclaration = parseDeclarationGeneric(declarationTokenizer, std::move(*innerToken));
      if (maybeDeclaration.has_value()) {
        result.emplace_back(std::move(maybeDeclaration.value()));
      }
    }

    // Note that this does not need to handle AtRules, since those are parsed by the RuleParser
    // before this is called.
  }

  return result;
}

}  // namespace donner::css
