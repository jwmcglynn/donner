#include "src/svg/properties/property_registry.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>
#include <frozen/unordered_set.h>

#include "src/base/parser/length_parser.h"
#include "src/css/parser/color_parser.h"
#include "src/css/parser/declaration_list_parser.h"
#include "src/css/parser/value_parser.h"
#include "src/svg/properties/presentation_attribute_parsing.h"
#include "src/svg/properties/property_parsing.h"

namespace donner::svg {

namespace {

// Presentation attributes have a specificity of 0.
static constexpr css::Specificity kSpecificityPresentationAttribute =
    css::Specificity::FromABC(0, 0, 0);

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

ParseResult<double> ParseNumber(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* number = component.tryGetToken<css::Token::Number>()) {
      return number->value;
    }
  }

  ParseError err;
  err.reason = "Invalid number";
  err.offset = !components.empty() ? components.front().sourceOffset() : 0;
  return err;
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
  }

  // If we couldn't parse yet, try parsing as a color.
  auto colorResult = css::ColorParser::Parse(components);
  if (colorResult.hasResult()) {
    return PaintServer(PaintServer::Solid(std::move(colorResult.result())));
  }

  ParseError err;
  err.reason = "Invalid paint server";
  err.offset = firstComponent.sourceOffset();
  return err;
}

ParseResult<double> ParseAlphaValue(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* number = component.tryGetToken<css::Token::Number>()) {
      return Clamp(number->value, 0.0, 1.0);
    } else if (const auto* percentage = component.tryGetToken<css::Token::Percentage>()) {
      return Clamp(percentage->value / 100.0, 0.0, 1.0);
    }
  }

  ParseError err;
  err.reason = "Invalid alpha value";
  err.offset = !components.empty() ? components.front().sourceOffset() : 0;
  return err;
}

ParseResult<StrokeLinecap> ParseStrokeLinecap(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("butt")) {
        return StrokeLinecap::Butt;
      } else if (value.equalsLowercase("round")) {
        return StrokeLinecap::Round;
      } else if (value.equalsLowercase("square")) {
        return StrokeLinecap::Square;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid linecap";
  err.offset = !components.empty() ? components.front().sourceOffset() : 0;
  return err;
}

ParseResult<StrokeLinejoin> ParseStrokeLinejoin(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("miter")) {
        return StrokeLinejoin::Miter;
      } else if (value.equalsLowercase("miter-clip")) {
        return StrokeLinejoin::MiterClip;
      } else if (value.equalsLowercase("round")) {
        return StrokeLinejoin::Round;
      } else if (value.equalsLowercase("bevel")) {
        return StrokeLinejoin::Bevel;
      } else if (value.equalsLowercase("arcs")) {
        return StrokeLinejoin::Arcs;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid linejoin";
  err.offset = !components.empty() ? components.front().sourceOffset() : 0;
  return err;
}

ParseResult<std::vector<Lengthd>> ParseStrokeDasharray(
    std::span<const css::ComponentValue> components) {
  // https://www.w3.org/TR/css-values-4/#mult-comma
  std::vector<Lengthd> result;

  auto trySkipToken = [&components]<typename T>() -> bool {
    if (!components.empty() && components.front().isToken<T>()) {
      components = components.subspan(1);
      return true;
    }
    return false;
  };

  while (!components.empty()) {
    if (!result.empty()) {
      if (trySkipToken.template operator()<css::Token::Whitespace>() ||
          trySkipToken.template operator()<css::Token::Comma>() || components.empty()) {
        trySkipToken.template operator()<css::Token::Whitespace>();
      } else {
        ParseError err;
        err.reason = "Unexpected token in dasharray";
        err.offset =
            !components.empty() ? components.front().sourceOffset() : ParseError::kEndOfString;
        return err;
      }
    }

    const css::ComponentValue& component = components.front();
    if (const auto* dimension = component.tryGetToken<css::Token::Dimension>()) {
      if (!dimension->suffixUnit) {
        ParseError err;
        err.reason = "Invalid unit on length";
        err.offset = component.sourceOffset();
        return err;
      } else {
        result.emplace_back(dimension->value, dimension->suffixUnit.value());
      }
    } else if (const auto* percentage = component.tryGetToken<css::Token::Percentage>()) {
      result.emplace_back(percentage->value, Lengthd::Unit::Percent);
    } else if (const auto* number = component.tryGetToken<css::Token::Number>()) {
      result.emplace_back(number->value, Lengthd::Unit::None);
    } else {
      ParseError err;
      err.reason = "Unexpected token in dasharray";
      err.offset = component.sourceOffset();
      return err;
    }

    components = components.subspan(1);
  }

  return result;
}

// List of valid presentation attributes from
// https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
static constexpr frozen::unordered_set<frozen::string, 12> kValidPresentationAttributes = {
    "cx", "cy", "height", "width", "x", "y", "r", "rx", "ry", "d", "fill", "transform",
    // The properties which may apply to any element in the SVG namespace are omitted.
};

static constexpr frozen::unordered_map<frozen::string, PropertyParseFn, 10> kProperties = {
    {"color",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) {
             return css::ColorParser::Parse(params.components);
           },
           &registry.color);
     }},  //
    {"fill",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) { return ParsePaintServer(params.components); },
           &registry.fill);
     }},  //
    {"stroke",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) { return ParsePaintServer(params.components); },
           &registry.stroke);
     }},  //
    {"stroke-opacity",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) { return ParseAlphaValue(params.components); },
           &registry.strokeOpacity);
     }},  //
    {"stroke-width",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) {
             return ParseLengthPercentage(params.components, params.allowUserUnits);
           },
           &registry.strokeWidth);
     }},  //
    {"stroke-linecap",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) {
             return ParseStrokeLinecap(params.components);
           },
           &registry.strokeLinecap);
     }},  //
    {"stroke-linejoin",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) {
             return ParseStrokeLinejoin(params.components);
           },
           &registry.strokeLinejoin);
     }},  //
    {"stroke-miterlimit",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) { return ParseNumber(params.components); },
           &registry.strokeMiterlimit);
     }},  //
    {"stroke-dasharray",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) {
             return ParseStrokeDasharray(params.components);
           },
           &registry.strokeDasharray);
     }},  //
    {"stroke-dashoffset",
     [](PropertyRegistry& registry, const PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const PropertyParseFnParams& params) {
             return ParseLengthPercentage(params.components, params.allowUserUnits);
           },
           &registry.strokeDashoffset);
     }},  //
};

}  // namespace

PropertyParseFnParams CreateParseFnParams(const css::Declaration& declaration,
                                          css::Specificity specificity) {
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

  params.specificity = declaration.important ? css::Specificity::Important() : specificity;

  return params;
}

std::optional<ParseError> PropertyRegistry::parseProperty(const css::Declaration& declaration,
                                                          css::Specificity specificity) {
  const frozen::string frozenName(declaration.name);
  const auto it = kProperties.find(frozenName);
  if (it != kProperties.end()) {
    return it->second(*this, CreateParseFnParams(declaration, specificity));
  }

  // Only store unparsed properties if they are valid presentation attribute names.
  if (kValidPresentationAttributes.count(frozenName) != 0) {
    unparsedProperties.emplace(
        std::make_pair(declaration.name, UnparsedProperty{declaration, specificity}));
  } else {
    ParseError err;
    err.reason = "Unknown property '" + declaration.name + "'";
    err.offset = declaration.sourceOffset;
    return err;
  }

  return std::nullopt;
}

void PropertyRegistry::parseStyle(std::string_view str) {
  const std::vector<css::Declaration> declarations =
      css::DeclarationListParser::ParseOnlyDeclarations(str);
  for (const auto& declaration : declarations) {
    std::ignore = parseProperty(declaration, css::Specificity::StyleAttribute());
  }
}

namespace {

template <typename ReturnType, typename FnT>
ReturnType toConstexpr(ElementType type, FnT fn) {
  switch (type) {
    case ElementType::Circle: return fn(std::integral_constant<ElementType, ElementType::Circle>());
    case ElementType::Defs: return fn(std::integral_constant<ElementType, ElementType::Defs>());
    case ElementType::Path: return fn(std::integral_constant<ElementType, ElementType::Path>());
    case ElementType::Rect: return fn(std::integral_constant<ElementType, ElementType::Rect>());
    case ElementType::Style: return fn(std::integral_constant<ElementType, ElementType::Style>());
    case ElementType::SVG: return fn(std::integral_constant<ElementType, ElementType::SVG>());
    case ElementType::Unknown:
      return fn(std::integral_constant<ElementType, ElementType::Unknown>());
    case ElementType::Use: return fn(std::integral_constant<ElementType, ElementType::Use>());
  };
}

}  // namespace

bool PropertyRegistry::parsePresentationAttribute(std::string_view name, std::string_view value,
                                                  std::optional<ElementType> type,
                                                  EntityHandle handle) {
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
  assert((!type.has_value() || (type.has_value() && handle != EntityHandle())) &&
         "If a type is specified, entity handle must be set");

  const auto it = kProperties.find(frozen::string(name));
  if (it != kProperties.end()) {
    std::vector<css::ComponentValue> components = css::ValueParser::Parse(value);

    PropertyParseFnParams params;
    // Trim both leading and trailing whitespace.
    params.components = trimWhitespace(components);
    params.specificity = kSpecificityPresentationAttribute;
    params.allowUserUnits = true;

    auto maybeError = it->second(*this, params);
    if (maybeError.has_value()) {
      std::cerr << "Error parsing " << name << " property: " << maybeError.value() << std::endl;
    }

    return true;
  }

  if (!type.has_value()) {
    // Stop processing if there is not an element type.
    return false;
  }

  std::vector<css::ComponentValue> components = css::ValueParser::Parse(value);

  PropertyParseFnParams params;
  // Trim both leading and trailing whitespace.
  params.components = trimWhitespace(components);
  params.specificity = kSpecificityPresentationAttribute;
  params.allowUserUnits = true;

  ParseResult<bool> result = toConstexpr<ParseResult<bool>>(type.value(), [&](auto elementType) {
    return ParsePresentationAttribute<elementType()>(handle, name, params);
  });

  if (result.hasError()) {
    std::cerr << "Error parsing " << name << " property: " << result.error() << std::endl;
    return false;
  } else {
    return result.result();
  }
}

std::ostream& operator<<(std::ostream& os, const PropertyRegistry& registry) {
  os << "PropertyRegistry {" << std::endl;

  const auto resultProperties = const_cast<PropertyRegistry&>(registry).allProperties();
  PropertyRegistry::forEachProperty<0, PropertyRegistry::numProperties()>(
      [&os, &resultProperties](auto i) {
        const auto& property = std::get<i.value>(resultProperties);
        if (property.hasValue()) {
          os << "  " << property << std::endl;
        }
      });

  return os << "}" << std::endl;
}

}  // namespace donner::svg
