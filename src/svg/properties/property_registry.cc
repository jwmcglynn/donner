#include "src/svg/properties/property_registry.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>

#include "src/css/parser/color_parser.h"
#include "src/css/parser/declaration_list_parser.h"

namespace donner::svg {

namespace {

std::span<const css::ComponentValue> trimTrailingWhitespace(
    std::span<const css::ComponentValue> components) {
  while (!components.empty() && components.back().isToken<css::Token::Whitespace>()) {
    components = components.subspan(0, components.size() - 1);
  }

  return components;
}

template <typename T>
std::optional<ParseError> unwrap(ParseResult<T>&& result, std::optional<T>* destination) {
  if (result.hasError()) {
    *destination = std::nullopt;
    return std::move(result.error());
  }

  *destination = std::move(result.result());
  return std::nullopt;
}

ParseResult<PaintServer> ParsePaintServer(std::span<const css::ComponentValue> components) {
  components = trimTrailingWhitespace(components);

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

static constexpr frozen::unordered_map<frozen::string, ParseFunction, 2> kProperties = {
    {"color",
     [](PropertyRegistry& registry, const css::Declaration& declaration) {
       return unwrap(css::ColorParser::Parse(declaration.values), &registry.color);
     }},  //
    {"fill",
     [](PropertyRegistry& registry, const css::Declaration& declaration) {
       return unwrap(ParsePaintServer(declaration.values), &registry.fill);
     }}  //
};

}  // namespace

std::optional<ParseError> PropertyRegistry::parseProperty(const css::Declaration& declaration) {
  // TODO: Add support for "inherit" values.
  const auto it = kProperties.find(frozen::string(declaration.name));
  if (it != kProperties.end()) {
    return it->second(*this, declaration);
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
    [[maybe_unused]] auto error = parseProperty(declaration);
    // Ignore errors.
  }
}

}  // namespace donner::svg
