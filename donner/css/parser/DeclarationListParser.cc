#include "donner/css/parser/DeclarationListParser.h"

#include <cassert>
#include <optional>
#include <span>

#include "donner/css/parser/details/Subparsers.h"
#include "donner/css/parser/details/Tokenizer.h"

namespace donner::css::parser {

namespace {

using details::consumeComponentValue;
using details::consumeDeclaration;
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
      (void)next();
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

  std::optional<std::size_t> terminatorOffset() const {
    return !eof_ && isEOF() ? next_.offset().offset : std::nullopt;
  }

private:
  T& tokenizer_;
  Item next_;
  bool eof_ = false;
};

template <details::DeclarationTokenizer T>
std::optional<Declaration> parseDeclarationGeneric(
    T& tokenizer, Token&& token, details::ComponentValueParsingBudget* aggregateBudget = nullptr,
    std::optional<std::size_t>* terminator = nullptr) {
  if (token.is<Token::Ident>()) {
    // <ident-token>: Initialize a temporary list initially filled with the current input token.
    Token::Ident ident = std::move(token.get<Token::Ident>());

    // A declaration list ends when it reaches a <semicolon-token> or <EOF-token>.
    if (tokenizer.isEOF()) {
      return std::nullopt;
    }
    ParseUntilSemicolonOrEOF<T> declarationInputTokenizer(tokenizer);
    if constexpr (std::is_same_v<typename T::ItemType, Token>) {
      auto result = consumeDeclaration(declarationInputTokenizer, std::move(ident), token.offset(),
                                       aggregateBudget);
      if (terminator) {
        *terminator = declarationInputTokenizer.terminatorOffset();
      }
      return result;
    } else {
      return consumeDeclaration(declarationInputTokenizer, std::move(ident), token.offset());
    }
  } else {
    // anything else: This is a parse error. Reconsume the current input token. As long as the
    // next input token is anything other than a <semicolon-token> or <EOF-token>, consume a
    // component value and throw away the returned value.
    if constexpr (std::is_same_v<typename T::ItemType, Token>) {
      // Only consume if we're parsing tokens, not ComponentValues which already did this.
      TokenizerConvertToToken tokenizerConvertToToken(tokenizer);
      details::ComponentValueParsingContext parsingContext(aggregateBudget);

      std::ignore = consumeComponentValue(tokenizerConvertToToken, std::move(token),
                                          ParseMode::Discard, parsingContext);
      if (aggregateBudget != nullptr && aggregateBudget->resourceLimitExceeded()) {
        return std::nullopt;
      }
    }

    while (!tokenizer.isEOF()) {
      auto subToken = tokenizer.next();
      if (subToken.template isToken<Token::Semicolon>()) {
        break;
      }

      std::ignore = subToken.asComponentValue(ParseMode::Discard);
      if (aggregateBudget != nullptr && aggregateBudget->resourceLimitExceeded()) {
        return std::nullopt;
      }
    }

    return std::nullopt;
  }
}

class DeclarationListParserImpl {
public:
  DeclarationListParserImpl(std::string_view str,
                            DeclarationListParser::SecurityStats* securityStats)
      : tokenizer_(str), securityStats_(securityStats) {}

  std::vector<DeclarationOrAtRule> parse() {
    std::vector<DeclarationOrAtRule> result;

    while (!tokenizer_.isEOF()) {
      if (result.size() >= DeclarationListParser::kMaximumDeclarations) {
        if (securityStats_) {
          securityStats_->rejected = true;
        }
        break;
      }
      auto token = tokenizer_.next();

      if (token.is<Token::AtKeyword>()) {
        // <at-keyword-token>: Reconsume the current input token. Consume an at-rule. Append the
        // returned rule to the list of declarations.
        auto atRule = consumeAtRule(tokenizer_, std::move(token.get<Token::AtKeyword>()),
                                    ParseMode::Keep, &aggregateBudget_);
        if (!aggregateBudget_.resourceLimitExceeded()) {
          result.emplace_back(std::move(atRule));
        }
      } else if (token.is<Token::Whitespace>() || token.is<Token::Semicolon>()) {
        // Skip.
      } else {
        details::DeclarationTokenTokenizer declarationTokenizer(tokenizer_, &aggregateBudget_);
        auto maybeDeclaration =
            parseDeclarationGeneric(declarationTokenizer, std::move(token), &aggregateBudget_);

        if (!aggregateBudget_.resourceLimitExceeded() && maybeDeclaration.has_value()) {
          result.emplace_back(std::move(maybeDeclaration.value()));
        } else if (!aggregateBudget_.resourceLimitExceeded()) {
          result.emplace_back(InvalidRule());
        }
      }
      if (aggregateBudget_.resourceLimitExceeded()) {
        if (securityStats_) {
          securityStats_->rejected = true;
        }
        break;
      }
    }

    if (securityStats_) {
      securityStats_->declarations = result.size();
      securityStats_->componentValues = aggregateBudget_.componentValues();
    }
    return result;
  }

  std::vector<Declaration> parseDeclarations(std::string_view source,
                                             std::vector<std::string_view>* spellings) {
    if (spellings) {
      spellings->clear();
    }
    std::vector<Declaration> result;

    while (!tokenizer_.isEOF()) {
      if (result.size() >= DeclarationListParser::kMaximumDeclarations) {
        if (securityStats_) {
          securityStats_->rejected = true;
        }
        break;
      }
      auto token = tokenizer_.next();

      if (token.is<Token::AtKeyword>()) {
        // <at-keyword-token>: Reconsume the current input token. Consume an at-rule. Append the
        // returned rule to the list of declarations.
        // In this case we ignore the result since only declarations are desired.
        std::ignore = consumeAtRule(tokenizer_, std::move(token.get<Token::AtKeyword>()),
                                    ParseMode::Discard, &aggregateBudget_);
      } else if (token.is<Token::Whitespace>() || token.is<Token::Semicolon>()) {
        // Skip.
      } else {
        appendAuthoredDeclaration(std::move(token), source, spellings, result);
      }
      if (aggregateBudget_.resourceLimitExceeded()) {
        if (securityStats_) {
          securityStats_->rejected = true;
        }
        break;
      }
    }

    if (securityStats_) {
      securityStats_->declarations = result.size();
      securityStats_->componentValues = aggregateBudget_.componentValues();
    }
    return result;
  }

private:
  void appendAuthoredDeclaration(Token token, std::string_view source,
                                 std::vector<std::string_view>* spellings,
                                 std::vector<Declaration>& result) {
    details::DeclarationTokenTokenizer declarationTokenizer(tokenizer_, &aggregateBudget_);

    std::optional<std::size_t> terminator;
    auto maybeDeclaration =
        parseDeclarationGeneric(declarationTokenizer, std::move(token), &aggregateBudget_,
                                spellings ? &terminator : nullptr);
    if (maybeDeclaration.has_value()) {
      if (spellings) {
        const auto start = *maybeDeclaration->sourceOffset.offset;
        spellings->push_back(source.substr(start, terminator.value_or(source.size()) - start));
      }
      result.emplace_back(std::move(maybeDeclaration.value()));
    }
  }

  details::Tokenizer tokenizer_;
  details::ComponentValueParsingBudget aggregateBudget_;
  DeclarationListParser::SecurityStats* securityStats_;
};

}  // namespace

void RejectDeclarationList(DeclarationListParser::SecurityStats* securityStats) {
  if (securityStats) {
    securityStats->rejected = true;
  }
}

void RecordDeclarationStats(DeclarationListParser::SecurityStats* securityStats,
                            std::size_t declarations, std::size_t componentValues) {
  if (securityStats) {
    securityStats->declarations = declarations;
    securityStats->componentValues = componentValues;
  }
}

std::vector<DeclarationOrAtRule> DeclarationListParser::Parse(std::string_view str,
                                                              SecurityStats* securityStats) {
  DeclarationListParserImpl parser(str, securityStats);
  return parser.parse();
}

std::vector<Declaration> DeclarationListParser::ParseOnlyDeclarations(
    std::string_view str, SecurityStats* securityStats,
    std::vector<std::string_view>* authoredSpellings) {
  DeclarationListParserImpl parser(str, securityStats);
  return parser.parseDeclarations(str, authoredSpellings);
}

std::vector<Declaration> DeclarationListParser::ParseRuleDeclarations(
    std::span<ComponentValue> components, SecurityStats* securityStats) {
  std::vector<Declaration> result;
  if (components.size() > kMaximumComponentValues) {
    RecordDeclarationStats(securityStats, 0, kMaximumComponentValues);
    RejectDeclarationList(securityStats);
    return result;
  }
  if (components.empty()) {
    return result;
  }

  SubTokenizer<ComponentValue> tokenizer(components);
  details::DeclarationComponentValueTokenizer declarationTokenizer(tokenizer);

  while (!declarationTokenizer.isEOF()) {
    if (result.size() >= DeclarationListParser::kMaximumDeclarations) {
      RejectDeclarationList(securityStats);
      break;
    }
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

  RecordDeclarationStats(securityStats, result.size(), components.size());
  return result;
}

}  // namespace donner::css::parser
