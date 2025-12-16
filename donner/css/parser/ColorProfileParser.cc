#include "donner/css/parser/ColorProfileParser.h"

#include <algorithm>

#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"

namespace donner::css::parser {

namespace {

std::span<const ComponentValue> trimWhitespace(std::span<const ComponentValue> values) {
  while (!values.empty() && values.front().isToken<Token::Whitespace>()) {
    values = values.subspan(1);
  }
  while (!values.empty() && values.back().isToken<Token::Whitespace>()) {
    values = values.subspan(0, values.size() - 1);
  }
  return values;
}

std::optional<std::string> profileIdent(std::span<const ComponentValue> values) {
  values = trimWhitespace(values);
  if (values.size() != 1 || !values.front().is<Token>()) {
    return std::nullopt;
  }

  const Token& token = values.front().get<Token>();
  if (!token.is<Token::Ident>()) {
    return std::nullopt;
  }

  return token.get<Token::Ident>().value.str();
}

std::optional<std::string> parseSrcProfile(std::span<const ComponentValue> values) {
  values = trimWhitespace(values);
  if (values.empty()) {
    return std::nullopt;
  }

  if (values.front().is<Token>()) {
    const Token& token = values.front().get<Token>();
    if (token.is<Token::Ident>()) {
      return token.get<Token::Ident>().value.str();
    }
    return std::nullopt;
  }

  if (values.front().is<css::Function>()) {
    const css::Function& function = values.front().get<css::Function>();
    if (!function.name.equalsLowercase("color")) {
      return std::nullopt;
    }

    auto params = trimWhitespace(function.values);
    if (!params.empty() && params.front().isToken<Token::Ident>()) {
      const Token& paramToken = params.front().get<Token>();
      return paramToken.get<Token::Ident>().value.str();
    }
  }

  return std::nullopt;
}

std::optional<std::string> parseProfileName(const AtRule& rule) {
  if (rule.prelude.empty()) {
    return std::nullopt;
  }
  return profileIdent(rule.prelude);
}

ColorProfileRegistry parseRules(std::span<const Rule> rules) {
  ColorProfileRegistry registry;

  for (const Rule& rule : rules) {
    const auto* atRule = std::get_if<AtRule>(&rule.value);
    if (!atRule || !atRule->name.equalsLowercase("color-profile")) {
      continue;
    }

    auto profileName = parseProfileName(*atRule);
    if (!profileName || !atRule->block) {
      continue;
    }

    std::vector<ComponentValue> blockValues(atRule->block->values.begin(),
                                            atRule->block->values.end());
    std::vector<Declaration> declarations =
        DeclarationListParser::ParseRuleDeclarations(blockValues);

    for (const Declaration& decl : declarations) {
      std::string declName = decl.name.str();
      std::transform(declName.begin(), declName.end(), declName.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (declName != "src") {
        continue;
      }

      auto srcName = parseSrcProfile(decl.values);
      if (!srcName) {
        continue;
      }

      if (auto spaceId = ColorSpaceIdFromString(*srcName)) {
        registry.registerProfile(*profileName, *spaceId);
      }
      break;
    }
  }

  return registry;
}

}  // namespace

ColorProfileRegistry ColorProfileParser::Parse(std::span<const Rule> rules) {
  return parseRules(rules);
}

ColorProfileRegistry ColorProfileParser::ParseStylesheet(std::string_view stylesheet) {
  std::vector<Rule> rules = RuleParser::ParseStylesheet(stylesheet);
  return parseRules(rules);
}

}  // namespace donner::css::parser
