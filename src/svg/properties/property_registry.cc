#include "src/svg/properties/property_registry.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "src/css/parser/color_parser.h"
#include "src/css/parser/declaration_list_parser.h"
#include "src/css/parser/value_parser.h"

namespace donner::svg {

namespace {

static constexpr uint32_t kSpecificityImportant = ~static_cast<uint32_t>(1);
// The style attribute can only be overridden by !important.
static constexpr uint32_t kSpecificityStyleAttribute = kSpecificityImportant - 1;
// Presentation attributes have a specificity of 0.
static constexpr uint32_t kSpecificityPresentationAttribute = 0;

std::span<const css::ComponentValue> trimWhitespace(
    std::span<const css::ComponentValue> components) {
  while (!components.empty() && components.front().isToken<css::Token::Whitespace>()) {
    components = components.subspan(1);
  }

  while (!components.empty() && components.back().isToken<css::Token::Whitespace>()) {
    components = components.subspan(0, components.size() - 1);
  }

  return components;
}

template <typename T, typename ParseCallbackFn>
std::optional<ParseError> parse(const PropertyParseFnParams& params, ParseCallbackFn callbackFn,
                                PropertyRegistry::Property<T>* destination) {
  // If the property is set to a built-in keyword, such as "inherit", the property has already been
  // parsed so we can just set based on the value of explicitState.
  if (params.explicitState != PropertyState::NotSet) {
    destination->set(params.explicitState, params.specificity);
    return std::nullopt;
  }

  ParseResult<T> result = callbackFn(params);
  if (result.hasError()) {
    // If there is a parse error, the CSS specification requires user agents to ignore the
    // declaration, and not modify the existing value.
    // See https://www.w3.org/TR/CSS2/syndata.html#ignore.
    return std::move(result.error());
  }

  destination->set(std::move(result.result()), params.specificity);
  return std::nullopt;
}

ParseResult<PaintServer> ParsePaintServer(std::span<const css::ComponentValue> components) {
  if (components.empty()) {
    ParseError err;
    err.reason = "Invalid paint server value";
    return err;
  }

  const css::ComponentValue& firstComponent = components.front();
  if (firstComponent.is<css::Token>()) {
    const auto& token = firstComponent.get<css::Token>();
    if (token.is<css::Token::Ident>()) {
      const RcString& name = token.get<css::Token::Ident>().value;

      std::optional<PaintServer> result;

      if (name.equalsLowercase("none")) {
        result = PaintServer(PaintServer::None());
      } else if (name.equalsLowercase("context-fill")) {
        result = PaintServer(PaintServer::ContextFill());
      } else if (name.equalsLowercase("context-stroke")) {
        result = PaintServer(PaintServer::ContextStroke());
      }

      if (result) {
        if (components.size() > 1) {
          ParseError err;
          err.reason = "Unexpected tokens after paint server value";
          err.offset = token.offset() + name.size();
          return err;
        }

        return std::move(result.value());
      }
    } else if (token.is<css::Token::Url>()) {
      const auto& url = token.get<css::Token::Url>();

      // Extract the fallback if it is provided.
      for (size_t i = 1; i < components.size(); ++i) {
        const auto& component = components[i];
        if (component.is<css::Token>()) {
          const auto& token = component.get<css::Token>();
          if (token.is<css::Token::Whitespace>()) {
            continue;
          } else if (token.is<css::Token::Ident>()) {
            const RcString& value = token.get<css::Token::Ident>().value;
            if (value.equalsLowercase("none")) {
              return PaintServer(PaintServer::Reference(url.value, std::nullopt));
            }
          }
        }

        // If we couldn't identify a fallback yet, try parsing it as a color.
        auto colorResult = css::ColorParser::Parse(components.subspan(i));
        if (colorResult.hasResult()) {
          return PaintServer(PaintServer::Reference(url.value, std::move(colorResult.result())));
        } else {
          // Invalid paint.
          ParseError err;
          err.reason = "Invalid paint server url, failed to parse fallback";
          err.offset = components[i].sourceOffset();
          return err;
        }
      }

      // If we finished looping over additional tokens and didn't return, there is no fallback, only
      // whitespace.
      return PaintServer(PaintServer::Reference(url.value, std::nullopt));
    }

    // If we couldn't parse yet, try parsing as a color.
    auto colorResult = css::ColorParser::Parse(components);
    if (colorResult.hasResult()) {
      return PaintServer(PaintServer::Solid(std::move(colorResult.result())));
    }
  }

  ParseError err;
  err.reason = "Invalid paint server";
  err.offset = firstComponent.sourceOffset();
  return err;
}

static constexpr frozen::unordered_map<frozen::string, PropertyParseFn, 3> kProperties = {
    {"color",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return parse(
           params,
           [](const PropertyParseFnParams& params) {
             return css::ColorParser::Parse(params.components);
           },
           &registry.color);
     }},  //
    {"fill",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return parse(
           params,
           [](const PropertyParseFnParams& params) { return ParsePaintServer(params.components); },
           &registry.fill);
     }},  //
    {"stroke",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return parse(
           params,
           [](const PropertyParseFnParams& params) { return ParsePaintServer(params.components); },
           &registry.stroke);
     }}  //
};

}  // namespace

std::optional<ParseError> PropertyRegistry::parseProperty(const css::Declaration& declaration,
                                                          uint32_t specificity) {
  PropertyParseFnParams params;
  // Note that we only need to trim the trailing whitespace here, but trimWhitespace actually trims
  // both.
  params.components = trimWhitespace(declaration.values);

  // Detect CSS-wide keywords, see https://www.w3.org/TR/css-cascade-3/#defaulting-keywords.
  if (params.components.size() == 1 && params.components.front().isToken<css::Token::Ident>()) {
    const RcString& ident =
        params.components.front().get<css::Token>().get<css::Token::Ident>().value;
    if (ident.equalsLowercase("initial")) {
      params.explicitState = PropertyState::ExplicitInitial;
    } else if (ident.equalsLowercase("inherit")) {
      params.explicitState = PropertyState::Inherit;
    } else if (ident.equalsLowercase("unset")) {
      params.explicitState = PropertyState::ExplicitUnset;
    }
  }

  params.specificity = declaration.important ? kSpecificityImportant : specificity;

  const auto it = kProperties.find(frozen::string(declaration.name));
  if (it != kProperties.end()) {
    return it->second(*this, params);
  }

  ParseError err;
  err.reason = "Unknown property '" + declaration.name + "'";
  err.offset = declaration.sourceOffset;
  return err;
}

void PropertyRegistry::parseStyle(std::string_view str) {
  const std::vector<css::Declaration> declarations =
      css::DeclarationListParser::ParseOnlyDeclarations(str);
  for (const auto& declaration : declarations) {
    std::ignore = parseProperty(declaration, kSpecificityStyleAttribute);
  }
}

void PropertyRegistry::parsePresentationAttribute(std::string_view name, std::string_view value) {
  /* TODO: The SVG2 spec says the name may be similar to the attribute, not necessarily the same.
   * There may need to be a second mapping.
   */
  /* TODO: For attributes, fields may be unitless, in which case they are specified in "user units",
   * see https://www.w3.org/TR/SVG2/coords.html#TermUserUnits. For this case, the spec says to
   * adjust the grammar to modify things like <length> to [<length> | <number>], see
   * https://www.w3.org/TR/SVG2/types.html#syntax.
   *
   * In practice, we should propagate an "allowUserUnits" flag. "User units" are specified as being
   * equivalent to pixels.
   */
  const auto it = kProperties.find(frozen::string(name));
  if (it != kProperties.end()) {
    std::vector<css::ComponentValue> components = css::ValueParser::Parse(value);

    PropertyParseFnParams params;
    // Trim both leading and trailing whitespace.
    params.components = trimWhitespace(components);
    params.specificity = kSpecificityPresentationAttribute;

    std::ignore = it->second(*this, params);
  }
}

}  // namespace donner::svg
