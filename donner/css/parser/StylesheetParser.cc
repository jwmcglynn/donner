#include "donner/css/parser/StylesheetParser.h"

#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/SelectorParser.h"

namespace donner::css::parser {

Stylesheet StylesheetParser::Parse(std::string_view str) {
  std::vector<Rule> rules = RuleParser::ParseStylesheet(str);

  std::vector<SelectorRule> selectorRules;
  for (auto&& rule : rules) {
    // If the rule is a QualifiedRule, then we need to parse the selector and add it to our list.
    if (QualifiedRule* qualifiedRule = std::get_if<QualifiedRule>(&rule.value)) {
      auto selectorResult = SelectorParser::ParseComponents(qualifiedRule->prelude);
      // Ignore errors.
      if (selectorResult.hasError()) {
        continue;
      }

      std::vector<Declaration> declarations =
          DeclarationListParser::ParseRuleDeclarations(qualifiedRule->block.values);

      SelectorRule selectorRule;
      selectorRule.selector = std::move(selectorResult.result());
      selectorRule.declarations = std::move(declarations);
      selectorRules.emplace_back(std::move(selectorRule));
    }
  }

  return Stylesheet(std::move(selectorRules));
}

}  // namespace donner::css::parser
