#include "src/css/parser/declaration_list_parser.h"

#include <optional>
#include <span>

#include "src/css/parser/details/subparsers.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner {
namespace css {

namespace {

using details::consumeComponentValue;
using details::consumeDeclaration;
using details::consumeSimpleBlock;
using details::ParseMode;

class SubTokenizer {
public:
  SubTokenizer(std::span<Token> elements, size_t eofOffset)
      : elements_(elements), eofOffset_(eofOffset) {}

  Token next() {
    if (elements_.empty()) {
      return Token(Token::EofToken(), eofOffset_);
    } else {
      Token result(std::move(elements_.front()));
      elements_ = elements_.subspan(1);
      return result;
    }
  }

  bool isEOF() const { return elements_.empty(); }

private:
  std::span<Token> elements_;
  size_t eofOffset_;
};

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
        auto maybeDeclaration = parseCommon(std::move(token));

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
        auto maybeDeclaration = parseCommon(std::move(token));
        if (maybeDeclaration.has_value()) {
          result.emplace_back(std::move(maybeDeclaration.value()));
        }
      }
    }

    return result;
  }

  std::optional<Declaration> parseCommon(Token&& token) {
    if (token.is<Token::Ident>()) {
      // <ident-token>: Initialize a temporary list initially filled with the current input token.
      Token::Ident ident = std::move(token.get<Token::Ident>());
      std::vector<Token> declarationInput;
      size_t eofOffset = 0;

      // As long as the next input token is anything other than a <semicolon-token> or
      // <EOF-token>, consume a component value and append it to the temporary list. Consume a
      // declaration from the temporary list. If anything was returned, append it to the list of
      // declarations.
      while (true) {
        auto listToken = tokenizer_.next();
        if (!listToken.is<Token::Semicolon>() && !listToken.is<Token::EofToken>()) {
          declarationInput.emplace_back(std::move(std::move(listToken)));
        } else {
          eofOffset = listToken.offset();
          break;
        }
      }

      SubTokenizer subTokenizer(declarationInput, eofOffset);
      return consumeDeclaration(subTokenizer, std::move(ident));
    } else {
      // anything else: This is a parse error. Reconsume the current input token. As long as the
      // next input token is anything other than a <semicolon-token> or <EOF-token>, consume a
      // component value and throw away the returned value.
      std::ignore = consumeComponentValue(tokenizer_, std::move(token), ParseMode::Discard);

      while (!tokenizer_.isEOF()) {
        auto subToken = tokenizer_.next();
        if (subToken.is<Token::Semicolon>()) {
          break;
        }

        std::ignore = consumeComponentValue(tokenizer_, std::move(subToken), ParseMode::Discard);
      }

      return std::nullopt;
    }
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

}  // namespace css
}  // namespace donner
