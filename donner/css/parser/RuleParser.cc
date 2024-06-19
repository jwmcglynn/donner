#include "donner/css/parser/RuleParser.h"

#include "donner/css/parser/details/Subparsers.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser {

namespace {

enum class ListOfRulesFlags { None, TopLevel };

using details::consumeAtRule;
using details::ParseMode;

class RuleParserImpl {
public:
  RuleParserImpl(std::string_view str) : tokenizer_(str) {}

  std::vector<Rule> parseStylesheet() { return parseListOfRules(ListOfRulesFlags::TopLevel); }

  std::vector<Rule> parseListOfRules(ListOfRulesFlags flags) {
    return consumeListOfRules(tokenizer_, flags);
  }

  std::optional<Rule> parseRule() {
    std::optional<Rule> result;

    while (true) {
      Token token = tokenizer_.next();
      if (token.is<Token::Whitespace>()) {
        // While the next input token is a <whitespace-token>, consume the next input token.
        continue;
      } else if (token.is<Token::EofToken>()) {
        // If the next input token is an <EOF-token>, return a syntax error.
        break;
      } else if (token.is<Token::AtKeyword>()) {
        // Otherwise, if the next input token is an <at-keyword-token>, consume an at-rule, and let
        // rule be the return value.
        auto atRule =
            consumeAtRule(tokenizer_, std::move(token.get<Token::AtKeyword>()), ParseMode::Keep);
        if (!atRule.name.equalsLowercase("charset")) {
          result = Rule(std::move(atRule));
          break;
        } else {
          return Rule(InvalidRule());
        }
      } else {
        // Otherwise, consume a qualified rule and let rule be the return value. If nothing was
        // returned, return a syntax error.
        if (auto maybeQualifiedRule = consumeQualifiedRule(tokenizer_, std::move(token))) {
          result = Rule(std::move(maybeQualifiedRule.value()));
          break;
        } else {
          return Rule(InvalidRule());
        }
      }
    }

    if (!result) {
      return std::nullopt;
    }

    while (true) {
      Token token = tokenizer_.next();
      if (token.is<Token::Whitespace>()) {
        // While the next input token is a <whitespace-token>, consume the next input token.
        continue;
      } else if (token.is<Token::EofToken>()) {
        // If the next input token is an <EOF-token>, return rule. Otherwise, return a syntax
        // error.
        return std::move(result.value());
      } else {
        return Rule(InvalidRule(InvalidRule::Type::ExtraInput));
      }
    }
  }

  /// Remove a @charset token following the guidelines at
  /// https://www.w3.org/TR/css-syntax-3/#determine-the-fallback-encoding
  static std::string_view maybeRemoveCharset(std::string_view str) {
    using namespace std::literals;

    constexpr std::string_view kCharsetStart = "@charset \""sv;
    if (!str.starts_with(kCharsetStart)) {
      return str;
    }

    const size_t charsetRegion = std::min(str.size(), size_t(1024));
    for (size_t i = kCharsetStart.size(); i < charsetRegion; ++i) {
      if (str.substr(i).starts_with("\";")) {
        return str.substr(i + 2);
      } else if (str[i] == 0x22 || static_cast<uint8_t>(str[i]) > 0x7F) {
        break;
      }
    }

    return str;
  }

  /// Consume a list of rules, per https://www.w3.org/TR/css-syntax-3/#consume-list-of-rules
  static std::vector<Rule> consumeListOfRules(details::Tokenizer& tokenizer,
                                              ListOfRulesFlags flags) {
    std::vector<Rule> result;

    while (!tokenizer.isEOF()) {
      Token token = tokenizer.next();
      if (token.is<Token::Whitespace>() || token.is<Token::EofToken>()) {
        // <whitespace-token>: Do nothing.
        continue;
      } else if (token.is<Token::CDO>() || token.is<Token::CDC>()) {
        if (flags == ListOfRulesFlags::TopLevel) {
          // If the top-level flag is set, do nothing.
          continue;
        } else {
          // Otherwise, reconsume the current input token. Consume a qualified rule. If anything is
          // returned, append it to the list of rules.
          if (auto maybeQualifiedRule = consumeQualifiedRule(tokenizer, std::move(token))) {
            result.emplace_back(std::move(maybeQualifiedRule.value()));
          } else {
            result.emplace_back(InvalidRule());
          }
        }
      } else if (token.is<Token::AtKeyword>()) {
        // <at-keyword-token>: Reconsume the current input token. Consume an at-rule, and append the
        // returned value to the list of rules.
        auto atRule =
            consumeAtRule(tokenizer, std::move(token.get<Token::AtKeyword>()), ParseMode::Keep);
        if (!atRule.name.equalsLowercase("charset")) {
          result.emplace_back(std::move(atRule));
        } else {
          result.emplace_back(InvalidRule());
        }
      } else {
        // anything else: Reconsume the current input token. Consume a qualified rule. If anything
        // is returned, append it to the list of rules.
        if (auto maybeQualifiedRule = consumeQualifiedRule(tokenizer, std::move(token))) {
          result.emplace_back(std::move(maybeQualifiedRule.value()));
        } else {
          result.emplace_back(InvalidRule());
        }
      }
    }

    return result;
  }

  static std::optional<QualifiedRule> consumeQualifiedRule(details::Tokenizer& tokenizer,
                                                           Token&& firstToken) {
    std::vector<ComponentValue> prelude;
    Token token = std::move(firstToken);

    while (true) {
      if (token.is<Token::EofToken>()) {
        // <EOF-token>: This is a parse error. Return nothing.
        return std::nullopt;
      } else if (token.is<Token::CurlyBracket>()) {
        // <{-token>: Consume a simple block and assign it to the qualified rule's block. Return the
        // qualified rule.
        return QualifiedRule(std::move(prelude),
                             consumeSimpleBlock(tokenizer, std::move(token), ParseMode::Keep));
      } else {
        // anything else: Reconsume the current input token. Consume a component value. Append the
        // returned value to the qualified rule's prelude.
        auto component = consumeComponentValue(tokenizer, std::move(token), ParseMode::Keep);
        prelude.emplace_back(std::move(component));
      }

      token = tokenizer.next();
    }
  }

private:
  details::Tokenizer tokenizer_;
};

}  // namespace

std::vector<Rule> RuleParser::ParseStylesheet(std::string_view str) {
  RuleParserImpl parser(RuleParserImpl::maybeRemoveCharset(str));
  return parser.parseStylesheet();
}

std::vector<Rule> RuleParser::ParseListOfRules(std::string_view str) {
  RuleParserImpl parser(RuleParserImpl::maybeRemoveCharset(str));
  return parser.parseListOfRules(ListOfRulesFlags::None);
}

std::optional<Rule> RuleParser::ParseRule(std::string_view str) {
  RuleParserImpl parser(str);
  return parser.parseRule();
}

}  // namespace donner::css::parser
