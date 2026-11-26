#include "donner/editor/CssSourceReference.h"

#include <algorithm>
#include <cstddef>

#include "donner/base/StringUtils.h"
#include "donner/css/parser/details/ComponentValueParser.h"

namespace donner::editor {
namespace {

using css::ComponentValue;
using css::Token;

class ReferenceVisitor {
public:
  ReferenceVisitor(std::string_view source,
                   const std::function<bool(const CssSourceReference&)>& visit,
                   const std::function<std::optional<bool>(std::string_view)>& hasValue,
                   const CssSourceReferenceHooks& hooks)
      : source_(source), visit_(visit), hasValue_(hasValue), hooks_(hooks) {}

  bool walk(std::span<const ComponentValue> values, std::size_t depth = 0) {
    if (depth > css::parser::details::ComponentValueParsingContext::kMaximumNestingDepth) {
      return false;
    }
    for (const auto& value : values) {
      if (!charge() || !visitValue(value, depth)) {
        return false;
      }
    }
    return true;
  }

private:
  bool charge() {
    if (!remaining_ || (hooks_.consume && !hooks_.consume())) {
      return false;
    }
    --remaining_;
    return true;
  }

  bool skipWhitespace(std::span<const ComponentValue>& values) {
    while (!values.empty()) {
      if (!charge()) {
        return false;
      }
      if (!values.front().isToken<Token::Whitespace>()) {
        break;
      }
      values = values.subspan(1);
    }
    return true;
  }

  bool emit(CssSourceReference::Kind kind, std::string_view name, std::size_t start,
            std::size_t length) const {
    if (name.empty() || start > source_.size() || length == 0 || length > source_.size() - start) {
      return true;
    }
    return visit_({kind, name, {FileOffset::Offset(start), FileOffset::Offset(start + length)}});
  }

  bool fragment(std::string_view url, std::size_t start, std::size_t length) const {
    url = StringUtils::TrimWhitespace(url);
    if (!url.starts_with('#') || !length || start > source_.size() ||
        length > source_.size() - start) {
      return true;
    }
    const auto raw = source_.substr(start, length);
    const auto spelling = StringUtils::TrimWhitespace(raw);
    return emit(CssSourceReference::Kind::FragmentUrl, url.substr(1),
                start + static_cast<std::size_t>(spelling.data() - raw.data()), spelling.size());
  }

  bool quotedUrl(std::span<const ComponentValue> values) {
    if (!skipWhitespace(values)) {
      return false;
    }
    if (values.empty()) {
      return true;
    }
    auto rest = values.subspan(1);
    if (!skipWhitespace(rest)) {
      return false;
    }
    if (!rest.empty()) {
      return true;
    }
    const auto* token = std::get_if<Token>(&values.front().value);
    const auto* string = token ? token->tryGet<Token::String>() : nullptr;
    if (!string || token->sourceByteLength() < 2 || *token->offset().offset >= source_.size()) {
      return true;
    }
    return fragment(string->value, *token->offset().offset + 1, token->sourceByteLength() - 2);
  }

  bool variable(std::span<const ComponentValue> args, std::size_t depth) {
    if (!skipWhitespace(args)) {
      return false;
    }
    if (args.empty() || !args.front().isToken<Token::Ident>()) {
      return true;
    }
    const auto& token = std::get<Token>(args.front().value);
    const std::string_view name = token.get<Token::Ident>().value;
    auto rest = args.subspan(1);
    if (!skipWhitespace(rest)) {
      return false;
    }
    if (name.size() <= 2 || !name.starts_with("--") ||
        (!rest.empty() && !rest.front().isToken<Token::Comma>())) {
      return true;
    }
    if (!emit(CssSourceReference::Kind::CustomProperty, name, *token.offset().offset,
              token.sourceByteLength())) {
      return false;
    }
    if (rest.empty() || (hasValue_ && hasValue_(name).value_or(true))) {
      return true;
    }
    return fallback(name, rest.subspan(1), depth + 1);
  }

  bool fallback(std::string_view name, std::span<const ComponentValue> values, std::size_t depth) {
    if (hooks_.enterFallback) {
      hooks_.enterFallback(name);
    }
    const bool complete = walk(values, depth);
    if (hooks_.leaveFallback) {
      hooks_.leaveFallback();
    }
    return complete;
  }

  bool visitValue(const ComponentValue& value, std::size_t depth) {
    if (const auto* block = std::get_if<css::SimpleBlock>(&value.value)) {
      return walk(block->values, depth + 1);
    }
    if (const auto* function = std::get_if<css::Function>(&value.value)) {
      if (function->name.equalsLowercase("var")) {
        return variable(function->values, depth);
      }
      if (function->name.equalsLowercase("url")) {
        return quotedUrl(function->values);
      }
      return walk(function->values, depth + 1);
    }
    const auto& token = std::get<Token>(value.value);
    const auto* url = token.tryGet<Token::Url>();
    if (!url || !url->payloadLength || *token.offset().offset > source_.size() ||
        url->payloadOffset > source_.size() - *token.offset().offset) {
      return true;
    }
    return fragment(url->value, *token.offset().offset + url->payloadOffset, url->payloadLength);
  }

  std::string_view source_;
  const std::function<bool(const CssSourceReference&)>& visit_;
  const std::function<std::optional<bool>(std::string_view)>& hasValue_;
  const CssSourceReferenceHooks& hooks_;
  std::size_t remaining_ =
      css::parser::details::ComponentValueParsingBudget::kMaximumComponentValues;
};
}  // namespace

bool VisitCssSourceReferences(std::span<const css::ComponentValue> values,
                              std::string_view cssSource,
                              const std::function<bool(const CssSourceReference&)>& visit,
                              const std::function<std::optional<bool>(std::string_view)>& hasValue,
                              const CssSourceReferenceHooks& hooks) {
  return ReferenceVisitor(cssSource, visit, hasValue, hooks).walk(values);
}

}  // namespace donner::editor
