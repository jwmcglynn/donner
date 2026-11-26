#include "donner/css/VariableSubstitution.h"

#include <algorithm>
#include <utility>

#include "donner/base/SmallVector.h"
#include "donner/css/VariableReference.h"

namespace donner::css {
namespace {

using Values = std::span<const ComponentValue>;

void SkipWhitespace(Values& values) {
  while (!values.empty() && values.front().isToken<Token::Whitespace>()) {
    values = values.subspan(1);
  }
}

}  // namespace

namespace details {

std::optional<VariableReference> ParseVariableReference(const Function& function) {
  Values args(function.values);
  SkipWhitespace(args);
  if (args.empty()) {
    return std::nullopt;
  }
  const auto* ident = args.front().tryGetToken<Token::Ident>();
  if (!ident || !std::string_view(ident->value).starts_with("--") || ident->value.size() == 2) {
    return std::nullopt;
  }
  args = args.subspan(1);
  SkipWhitespace(args);
  VariableReference result{std::string_view(ident->value), std::nullopt};
  if (!args.empty()) {
    if (!args.front().isToken<Token::Comma>()) {
      return std::nullopt;
    }
    result.fallback = args.subspan(1);
    for (const auto& value : *result.fallback) {
      const auto* delim = value.tryGetToken<Token::Delim>();
      if (value.isToken<Token::Semicolon>() || (delim && delim->value == '!')) {
        return std::nullopt;
      }
    }
  }
  return result;
}

}  // namespace details

namespace {

VariableReferences Inspect(Values input, const std::function<void(std::string_view)>* visit) {
  VariableReferences status = VariableReferences::None;
  SmallVector<Values, 4> levels;
  levels.emplace_back(input);
  while (!levels.empty()) {
    auto& rest = levels.back();
    if (rest.empty()) {
      levels.pop_back();
      continue;
    }
    const auto& value = rest.front();
    rest = rest.subspan(1);
    if (const auto* function = std::get_if<Function>(&value.value)) {
      if (function->name.equalsLowercase("var")) {
        const auto ref = details::ParseVariableReference(*function);
        if (!ref) {
          return VariableReferences::Invalid;
        }
        status = VariableReferences::Valid;
        if (visit) {
          (*visit)(ref->name);
        }
      }
      levels.emplace_back(function->values);
    } else if (const auto* block = std::get_if<SimpleBlock>(&value.value)) {
      levels.emplace_back(block->values);
    }
  }
  return status;
}

// Count payload bytes before copying a token. Keep whitespace charges proportional to its
// owned spelling even though CSS text serialization normalizes it to a single space.
std::size_t TokenBytes(const Token& token) {
  return token.visit([](const auto& t) -> std::size_t {
    using T = std::remove_cvref_t<decltype(t)>;
    if constexpr (std::is_same_v<T, Token::Ident> || std::is_same_v<T, Token::Whitespace>) {
      return t.value.size();
    } else if constexpr (std::is_same_v<T, Token::Function> || std::is_same_v<T, Token::Hash>) {
      return t.name.size() + 1;
    } else if constexpr (std::is_same_v<T, Token::AtKeyword>) {
      return t.value.size() + 1;
    } else if constexpr (std::is_same_v<T, Token::String>) {
      return t.value.size() + 2;
    } else if constexpr (std::is_same_v<T, Token::Url>) {
      return t.value.size() + 5;
    } else if constexpr (std::is_same_v<T, Token::Number>) {
      return t.valueString.size();
    } else if constexpr (std::is_same_v<T, Token::Percentage>) {
      return t.valueString.size() + 1;
    } else if constexpr (std::is_same_v<T, Token::Dimension>) {
      return t.valueString.size() + t.suffixString.size();
    } else if constexpr (std::is_same_v<T, Token::CDO>) {
      return 4;
    } else if constexpr (std::is_same_v<T, Token::CDC>) {
      return 3;
    } else {
      return 1;
    }
  });
}

struct Level {
  Values rest;
  std::vector<ComponentValue>* output;
  bool computed;
  std::size_t depth;
};

class Expansion {
public:
  Expansion(const VariableLookup& lookup, VariableSubstitutionLimits limits,
            const std::function<bool(std::size_t, std::size_t)>& admit)
      : lookup_(lookup), remaining_(limits), admit_(admit) {}

  VariableSubstitutionResult run(Values input) {
    std::vector<ComponentValue> output;
    levels_.emplace_back(Level{input, &output, false, 0});
    while (!levels_.empty()) {
      Level& level = levels_.back();
      if (level.rest.empty()) {
        levels_.pop_back();
        continue;
      }
      const ComponentValue& value = level.rest.front();
      level.rest = level.rest.subspan(1);
      const auto status = append(value, level);
      if (status != VariableSubstitutionStatus::Resolved) {
        return {status, {}};
      }
    }
    return {VariableSubstitutionStatus::Resolved, std::move(output)};
  }

private:
  bool charge(std::size_t bytes) {
    if (!remaining_.components || bytes > remaining_.bytes || (admit_ && !admit_(1, bytes))) {
      return false;
    }
    --remaining_.components;
    remaining_.bytes -= bytes;
    return true;
  }

  VariableSubstitutionStatus reference(const Function& function, const Level& level) {
    const auto ref = details::ParseVariableReference(function);
    if (!ref || level.computed) {
      return VariableSubstitutionStatus::Invalid;
    }
    if (auto value = lookup_(ref->name)) {
      levels_.emplace_back(Level{*value, level.output, true, level.depth});
    } else if (ref->fallback) {
      levels_.emplace_back(Level{*ref->fallback, level.output, false, level.depth});
    } else {
      return VariableSubstitutionStatus::Invalid;
    }
    return VariableSubstitutionStatus::Resolved;
  }

  VariableSubstitutionStatus append(const ComponentValue& value, const Level& level) {
    if (const auto* token = std::get_if<Token>(&value.value)) {
      if (token->isParseError()) {
        return VariableSubstitutionStatus::Invalid;
      }
      if (!charge(TokenBytes(*token))) {
        return VariableSubstitutionStatus::LimitExceeded;
      }
      level.output->push_back(value);
      return VariableSubstitutionStatus::Resolved;
    }
    const auto* function = std::get_if<Function>(&value.value);
    if (function && function->name.equalsLowercase("var")) {
      return reference(*function, level);
    }
    // A substituted subtree can become deeper than either source. Keep the result safe for
    // existing recursive consumers and destructors of parsed component values.
    if (level.depth >= 64 || !charge(function ? function->name.size() + 2 : 2)) {
      return VariableSubstitutionStatus::LimitExceeded;
    }
    if (function) {
      auto& copied = level.output->emplace_back(Function(function->name, function->sourceOffset));
      levels_.emplace_back(
          Level{function->values, &copied.get<Function>().values, level.computed, level.depth + 1});
    } else {
      const auto& block = value.get<SimpleBlock>();
      auto& copied =
          level.output->emplace_back(SimpleBlock(block.associatedToken, block.sourceOffset));
      levels_.emplace_back(
          Level{block.values, &copied.get<SimpleBlock>().values, level.computed, level.depth + 1});
    }
    return VariableSubstitutionStatus::Resolved;
  }

  const VariableLookup& lookup_;
  VariableSubstitutionLimits remaining_;
  const std::function<bool(std::size_t, std::size_t)>& admit_;
  SmallVector<Level, 4> levels_;
};

}  // namespace

VariableReferences InspectVariableReferences(Values values) {
  return Inspect(values, nullptr);
}

bool VisitVariableReferences(Values values, const std::function<void(std::string_view)>& visit) {
  return Inspect(values, &visit) != VariableReferences::Invalid;
}

VariableSubstitutionResult SubstituteVariables(
    Values values, const VariableLookup& lookup, VariableSubstitutionLimits limits,
    const std::function<bool(std::size_t, std::size_t)>& admit) {
  if (InspectVariableReferences(values) == VariableReferences::Invalid) {
    return {VariableSubstitutionStatus::Invalid, {}};
  }
  return Expansion(lookup, limits, admit).run(values);
}

}  // namespace donner::css
