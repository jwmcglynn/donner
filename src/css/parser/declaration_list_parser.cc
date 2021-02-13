#include "src/css/parser/declaration_list_parser.h"

#include <optional>
#include <span>

#include "src/base/parser/details/parser_base.h"
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

  Token nextNoError() {
    if (elements_.empty()) {
      return Token(Token::EOFToken(), eofOffset_);
    } else {
      Token result(std::move(elements_.front()));
      elements_ = elements_.subspan(1);
      return result;
    }
  }

  ParseResult<Token> next() { return nextNoError(); }

  bool isEOF() const { return elements_.empty(); }

private:
  std::span<Token> elements_;
  size_t eofOffset_;
};

class DeclarationListParserImpl {
public:
  DeclarationListParserImpl(std::string_view str) : tokenizer_(str) {}

  ParseResult<std::vector<DeclarationOrAtRule>> parse() {
    std::vector<DeclarationOrAtRule> result;

    while (!tokenizer_.isEOF()) {
      auto tokenResult = tokenizer_.next();
      if (tokenResult.hasError()) {
        // TODO: Return current result and this error as a warning.
        return std::move(tokenResult.error());
      }

      auto token = std::move(tokenResult.result());
      if (token.is<Token::AtKeyword>()) {
        // <at-keyword-token>: Reconsume the current input token. Consume an at-rule. Append the
        // returned rule to the list of declarations.
        auto atRuleResult =
            consumeAtRule(std::move(token.get<Token::AtKeyword>()), ParseMode::Keep);
        if (atRuleResult.hasError()) {
          return std::move(atRuleResult.error());
        }

        result.emplace_back(std::move(atRuleResult.result()));
      } else {
        auto maybeDeclarationResult = parseCommon(std::move(token));
        if (maybeDeclarationResult.hasError()) {
          return std::move(maybeDeclarationResult.error());
        }

        if (maybeDeclarationResult.result().has_value()) {
          result.emplace_back(std::move(maybeDeclarationResult.result().value()));
        }
      }
    }

    return result;
  }

  ParseResult<std::vector<Declaration>> parseDeclarations() {
    std::vector<Declaration> result;

    while (!tokenizer_.isEOF()) {
      auto tokenResult = tokenizer_.next();
      if (tokenResult.hasError()) {
        return std::move(tokenResult.error());
      }

      auto token = std::move(tokenResult.result());
      if (token.is<Token::AtKeyword>()) {
        // <at-keyword-token>: Reconsume the current input token. Consume an at-rule. Append the
        // returned rule to the list of declarations.
        auto atRuleResult =
            consumeAtRule(std::move(token.get<Token::AtKeyword>()), ParseMode::Discard);
        if (atRuleResult.hasError()) {
          return std::move(atRuleResult.error());
        }
      } else {
        auto maybeDeclarationResult = parseCommon(std::move(token));
        if (maybeDeclarationResult.hasError()) {
          return std::move(maybeDeclarationResult.error());
        }

        if (maybeDeclarationResult.result().has_value()) {
          result.emplace_back(std::move(maybeDeclarationResult.result().value()));
        }
      }
    }

    return result;
  }

  ParseResult<std::optional<Declaration>> parseCommon(Token&& token) {
    using ResultType = ParseResult<std::optional<Declaration>>;

    if (token.is<Token::Whitespace>() || token.is<Token::Semicolon>()) {
      return ResultType(std::nullopt);
    } else if (token.is<Token::Ident>()) {
      // <ident-token>: Initialize a temporary list initially filled with the current input token.
      Token::Ident ident = std::move(token.get<Token::Ident>());
      std::vector<Token> declarationInput;
      size_t eofOffset = 0;

      // As long as the next input token is anything other than a <semicolon-token> or
      // <EOF-token>, consume a component value and append it to the temporary list. Consume a
      // declaration from the temporary list. If anything was returned, append it to the list of
      // declarations.
      while (true) {
        auto listTokenResult = tokenizer_.next();
        if (listTokenResult.hasError()) {
          return std::move(listTokenResult.error());
        }

        auto listToken = std::move(listTokenResult.result());
        if (!listToken.is<Token::Semicolon>() && !listToken.is<Token::EOFToken>()) {
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
      if (auto componentValueResult =
              consumeComponentValue(tokenizer_, std::move(token), ParseMode::Discard);
          componentValueResult.hasError()) {
        return std::move(componentValueResult.error());
      }

      while (!tokenizer_.isEOF()) {
        auto tokenResult = tokenizer_.next();
        if (tokenResult.hasError()) {
          return std::move(tokenResult.error());
        }

        auto subToken = std::move(tokenResult.result());
        if (subToken.is<Token::Semicolon>()) {
          break;
        }

        if (auto componentValueResult =
                consumeComponentValue(tokenizer_, std::move(subToken), ParseMode::Discard);
            componentValueResult.hasError()) {
          return std::move(componentValueResult.error());
        }
      }

      return ResultType(std::nullopt);
    }
  }

  /// Consume an at-rule, per https://www.w3.org/TR/css-syntax-3/#consume-at-rule
  ParseResult<AtRule> consumeAtRule(Token::AtKeyword&& atKeyword, ParseMode mode) {
    AtRule result(std::move(atKeyword.value));

    while (!tokenizer_.isEOF()) {
      auto tokenResult = tokenizer_.next();
      if (tokenResult.hasError()) {
        return std::move(tokenResult.error());
      }

      auto token = std::move(tokenResult.result());

      if (token.is<Token::Semicolon>()) {
        // Return the at-rule.
        return result;
      } else if (token.is<Token::CurlyBracket>()) {
        // <{-token>: Consume a simple block and assign it to the at-rule's block. Return the
        // at-rule.
        auto blockResult = consumeSimpleBlock(tokenizer_, std::move(token), mode);
        if (blockResult.hasError()) {
          return std::move(blockResult.error());
        }

        result.block = ComponentValue(std::move(blockResult.result()));
        return result;
      } else {
        // anything else: Reconsume the current input token. Consume a component value. Append the
        // returned value to the at-rule's prelude.
        auto componentResult = consumeComponentValue(tokenizer_, std::move(token), mode);
        if (componentResult.hasError()) {
          return std::move(componentResult.error());
        }

        if (mode == ParseMode::Keep) {
          result.prelude.emplace_back(std::move(componentResult.result()));
        }
      }
    }

    // <EOF-token>: This is a parse error. Return the at-rule.
    return result;
  }

private:
  details::Tokenizer tokenizer_;
};

}  // namespace

ParseResult<std::vector<DeclarationOrAtRule>> DeclarationListParser::Parse(std::string_view str) {
  DeclarationListParserImpl parser(str);
  return parser.parse();
}

ParseResult<std::vector<Declaration>> DeclarationListParser::ParseOnlyDeclarations(
    std::string_view str) {
  DeclarationListParserImpl parser(str);
  return parser.parseDeclarations();
}

}  // namespace css
}  // namespace donner
