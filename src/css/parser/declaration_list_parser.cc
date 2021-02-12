#include "src/css/parser/declaration_list_parser.h"

#include <concepts>
#include <optional>
#include <span>

#include "src/base/parser/details/parser_base.h"
#include "src/css/parser/details/tokenizer.h"

namespace donner {
namespace css {

namespace {

template <typename T>
concept TokenizerLike = requires(T t) {
  // clang-format off
  { t.next() } -> std::same_as<ParseResult<Token>>;
  { t.isEOF() } -> std::same_as<bool>;
  // clang-format on
};

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

enum class ParseMode { Keep, Discard };

class DeclarationListParserImpl {
public:
  DeclarationListParserImpl(std::string_view str) : tokenizer_(str) {}

  ParseResult<std::vector<DeclarationOrAtRule>> parse() {
    std::vector<DeclarationOrAtRule> result;

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

      return consumeDeclaration(SubTokenizer(declarationInput, eofOffset), std::move(ident));
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

  /// Consume a simple block, per https://www.w3.org/TR/css-syntax-3/#consume-simple-block
  template <TokenizerLike T>
  static ParseResult<SimpleBlock> consumeSimpleBlock(T& tokenizer, Token&& firstToken,
                                                     ParseMode mode) {
    const TokenIndex endingTokenIndex = simpleBlockEnding(firstToken.tokenIndex());
    SimpleBlock result(firstToken.tokenIndex());

    while (!tokenizer.isEOF()) {
      auto tokenResult = tokenizer.next();
      if (tokenResult.hasError()) {
        return std::move(tokenResult.error());
      }

      auto token = std::move(tokenResult.result());
      if (token.tokenIndex() == endingTokenIndex) {
        return result;
      } else {
        // anything else: Reconsume the current input token. Consume a component value and append
        // it to the value of the block.
        auto componentResult = consumeComponentValue(tokenizer, std::move(token), mode);
        if (componentResult.hasError()) {
          return std::move(componentResult.error());
        }

        if (mode == ParseMode::Keep) {
          result.values.emplace_back(std::move(componentResult.result()));
        }
      }
    }

    // <EOF-token>: This is a parse error. Return the block.
    return result;
  }

  /// Consume a component value, per https://www.w3.org/TR/css-syntax-3/#consume-component-value
  template <TokenizerLike T>
  static ParseResult<ComponentValue> consumeComponentValue(T& tokenizer, Token&& token,
                                                           ParseMode mode) {
    if (token.is<Token::CurlyBracket>() || token.is<Token::SquareBracket>() ||
        token.is<Token::Parenthesis>()) {
      // If the current input token is a <{-token>, <[-token>, or <(-token>, consume a simple
      // block and return it.
      return consumeSimpleBlock(tokenizer, std::move(token), mode)
          .template map<ComponentValue>(
              [](SimpleBlock&& block) { return ComponentValue(std::move(block)); });
    } else if (token.is<Token::Function>()) {
      // Otherwise, if the current input token is a <function-token>, consume a function and
      // return it.
      return consumeFunction(tokenizer, std::move(token.get<Token::Function>()), mode)
          .template map<ComponentValue>(
              [](Function&& func) { return ComponentValue(std::move(func)); });
    } else {
      return ComponentValue(token);
    }
  }

  /// Consume a function, per https://www.w3.org/TR/css-syntax-3/#consume-function
  template <TokenizerLike T>
  static ParseResult<Function> consumeFunction(T& tokenizer, Token::Function&& functionToken,
                                               ParseMode mode) {
    Function result(std::move(functionToken.name));

    while (!tokenizer.isEOF()) {
      auto tokenResult = tokenizer.next();
      if (tokenResult.hasError()) {
        return std::move(tokenResult.error());
      }

      Token token = std::move(tokenResult.result());
      if (token.is<Token::CloseParenthesis>()) {
        return result;
      } else {
        // anything else: Reconsume the current input token. Consume a component value and append
        // the returned value to the function's value.
        auto componentValueResult = consumeComponentValue(tokenizer, std::move(token), mode);
        if (componentValueResult.hasError()) {
          return std::move(componentValueResult.error());
        }

        if (mode == ParseMode::Keep) {
          result.values.emplace_back(std::move(componentValueResult.result()));
        }
      }
    }

    // <EOF-token>: This is a parse error. Return the function.
    return result;
  }

  /// Consume a declaration, per https://www.w3.org/TR/css-syntax-3/#consume-declaration
  static std::optional<Declaration> consumeDeclaration(SubTokenizer tokenizer,
                                                       Token::Ident&& ident) {
    Declaration declaration(std::move(ident.value));

    std::vector<ComponentValue> rawValues;

    while (!tokenizer.isEOF()) {
      Token token = tokenizer.nextNoError();
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
      Token token = tokenizer.nextNoError();
      if (token.is<Token::Whitespace>()) {
        // While the next input token is a <whitespace-token>, consume the next input token.
        lastWasImportantBang = false;
      } else {
        // As long as the next input token is anything other than an <EOF-token>, consume a
        // component value and append it to the declaration's value.
        auto componentValueResult =
            consumeComponentValue(tokenizer, std::move(token), ParseMode::Keep);
        assert(!componentValueResult.hasError());

        auto componentValue = std::move(componentValueResult.result());
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

  static TokenIndex simpleBlockEnding(TokenIndex startTokenIndex) {
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

private:
  Tokenizer tokenizer_;
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
