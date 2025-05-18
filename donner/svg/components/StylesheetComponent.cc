#include "donner/svg/components/StylesheetComponent.h"  // IWYU pragma: keep

#include "donner/css/Stylesheet.h"
#include "donner/css/parser/DeclarationListParser.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/css/parser/StylesheetParser.h"
#include "donner/css/parser/ValueParser.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/text/FontContext.h"
#include "donner/svg/properties/PresentationAttributeParsing.h"  // IWYU pragma: keep, for ParsePresentationAttribute
#include "donner/svg/resources/FontLoader.h"
#include "include/core/SkData.h"

namespace donner::svg::components {

namespace {

std::optional<std::string> TryGetSingleUrl(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* url = component.tryGetToken<css::Token::Url>()) {
      return std::string(url->value.str());
    }
    if (component.is<css::Function>()) {
      const css::Function& func = component.get<css::Function>();
      if (func.name.equalsLowercase("url") && func.values.size() == 1) {
        if (const auto* strTok = func.values[0].tryGetToken<css::Token::String>()) {
          return std::string(strTok->value.str());
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace

void StylesheetComponent::parseStylesheet(const RcStringOrRef& str,
                                          components::FontContext* fontContext) {
  stylesheet = donner::css::parser::StylesheetParser::Parse(str);

  if (!fontContext) {
    return;
  }

  auto rules = donner::css::parser::RuleParser::ParseStylesheet(str);
  Registry& registry = fontContext->registry();
  auto* resMgr = registry.ctx().try_get<components::ResourceManagerContext>();
  ResourceLoaderInterface* loader = resMgr ? resMgr->resourceLoader() : nullptr;

  for (const auto& rule : rules) {
    if (const css::AtRule* at = std::get_if<css::AtRule>(&rule.value)) {
      if (at->name.equalsLowercase("font-face") && at->block) {
        auto decls = css::parser::DeclarationListParser::ParseRuleDeclarations(at->block->values);
        RcString family;
        std::optional<std::string> src;
        for (const auto& decl : decls) {
          if (decl.name.equalsLowercase("font-family")) {
            if (auto ident = parser::TryGetSingleIdent(decl.values)) {
              family = *ident;
            }
          } else if (decl.name.equalsLowercase("src")) {
            src = TryGetSingleUrl(decl.values);
          }
        }
        if (!family.empty() && src && loader) {
          FontLoader fontLoader(*loader);
          auto dataOrErr = fontLoader.fromUri(*src);
          if (std::holds_alternative<std::vector<uint8_t>>(dataOrErr)) {
            const auto& data = std::get<std::vector<uint8_t>>(dataOrErr);
            fontContext->addFont(family, SkData::MakeWithCopy(data.data(), data.size()));
          }
        }
      }
    }
  }
}

}  // namespace donner::svg::components

namespace donner::svg::parser {

template <>
ParseResult<bool> ParsePresentationAttribute<ElementType::Style>(
    EntityHandle handle, std::string_view name, const PropertyParseFnParams& params) {
  // The style element has no presentation attributes.
  return false;
}

}  // namespace donner::svg::parser
