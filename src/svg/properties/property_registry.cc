#include "src/svg/properties/property_registry.h"

#include <frozen/string.h>
#include <frozen/unordered_map.h>
#include <frozen/unordered_set.h>

#include "src/base/parser/length_parser.h"
#include "src/css/css.h"
#include "src/css/parser/color_parser.h"
#include "src/css/parser/value_parser.h"
#include "src/svg/parser/length_percentage_parser.h"
#include "src/svg/properties/property_parsing.h"

namespace donner::svg {

namespace {

// Presentation attributes have a specificity of 0.
static constexpr css::Specificity kSpecificityPresentationAttribute =
    css::Specificity::FromABC(0, 0, 0);

parser::ParseResult<double> ParseNumber(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* number = component.tryGetToken<css::Token::Number>()) {
      return number->value;
    }
  }

  parser::ParseError err;
  err.reason = "Invalid number";
  err.location =
      !components.empty() ? components.front().sourceOffset() : parser::FileOffset::Offset(0);
  return err;
}

parser::ParseResult<Display> ParseDisplay(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("inline")) {
        return Display::Inline;
      } else if (value.equalsLowercase("block")) {
        return Display::Block;
      } else if (value.equalsLowercase("list-item")) {
        return Display::ListItem;
      } else if (value.equalsLowercase("inline-block")) {
        return Display::InlineBlock;
      } else if (value.equalsLowercase("table")) {
        return Display::Table;
      } else if (value.equalsLowercase("inline-table")) {
        return Display::InlineTable;
      } else if (value.equalsLowercase("table-row-group")) {
        return Display::TableRowGroup;
      } else if (value.equalsLowercase("table-header-group")) {
        return Display::TableHeaderGroup;
      } else if (value.equalsLowercase("table-footer-group")) {
        return Display::TableFooterGroup;
      } else if (value.equalsLowercase("table-row")) {
        return Display::TableRow;
      } else if (value.equalsLowercase("table-column-group")) {
        return Display::TableColumnGroup;
      } else if (value.equalsLowercase("table-column")) {
        return Display::TableColumn;
      } else if (value.equalsLowercase("table-cell")) {
        return Display::TableCell;
      } else if (value.equalsLowercase("table-caption")) {
        return Display::TableCaption;
      } else if (value.equalsLowercase("none")) {
        return Display::None;
      }
    }
  }

  parser::ParseError err;
  err.reason = "Invalid display value";
  err.location =
      !components.empty() ? components.front().sourceOffset() : parser::FileOffset::Offset(0);
  return err;
}

parser::ParseResult<Visibility> ParseVisibility(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("visible")) {
        return Visibility::Visible;
      } else if (value.equalsLowercase("hidden")) {
        return Visibility::Hidden;
      } else if (value.equalsLowercase("collapse")) {
        return Visibility::Collapse;
      }
    }
  }

  parser::ParseError err;
  err.reason = "Invalid display value";
  err.location =
      !components.empty() ? components.front().sourceOffset() : parser::FileOffset::Offset(0);
  return err;
}

parser::ParseResult<PaintServer> ParsePaintServer(std::span<const css::ComponentValue> components) {
  if (components.empty()) {
    parser::ParseError err;
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
          parser::ParseError err;
          err.reason = "Unexpected tokens after paint server value";
          err.location = token.offset();
          err.location.offset.value() += name.size();
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
              // TODO: Is there a difference between omitted and "none"?
              return PaintServer(PaintServer::ElementReference(url.value, std::nullopt));
            }
          }
        }

        // If we couldn't identify a fallback yet, try parsing it as a color.
        auto colorResult = css::parser::ColorParser::Parse(components.subspan(i));
        if (colorResult.hasResult()) {
          return PaintServer(
              PaintServer::ElementReference(url.value, std::move(colorResult.result())));
        } else {
          // Invalid paint.
          parser::ParseError err;
          err.reason = "Invalid paint server url, failed to parse fallback";
          err.location = components[i].sourceOffset();
          return err;
        }
      }

      // If we finished looping over additional tokens and didn't return, there is no fallback, only
      // whitespace.
      return PaintServer(PaintServer::ElementReference(url.value, std::nullopt));
    }
  }

  // If we couldn't parse yet, try parsing as a color.
  auto colorResult = css::parser::ColorParser::Parse(components);
  if (colorResult.hasResult()) {
    return PaintServer(PaintServer::Solid(std::move(colorResult.result())));
  }

  parser::ParseError err;
  err.reason = "Invalid paint server";
  err.location = firstComponent.sourceOffset();
  return err;
}

parser::ParseResult<FillRule> ParseFillRule(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("nonzero")) {
        return FillRule::NonZero;
      } else if (value.equalsLowercase("evenodd")) {
        return FillRule::EvenOdd;
      }
    }
  }

  parser::ParseError err;
  err.reason = "Invalid fill rule";
  err.location =
      !components.empty() ? components.front().sourceOffset() : parser::FileOffset::Offset(0);
  return err;
}

parser::ParseResult<StrokeLinecap> ParseStrokeLinecap(
    std::span<const css::ComponentValue> components) {
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

  parser::ParseError err;
  err.reason = "Invalid linecap";
  err.location =
      !components.empty() ? components.front().sourceOffset() : parser::FileOffset::Offset(0);
  return err;
}

parser::ParseResult<StrokeLinejoin> ParseStrokeLinejoin(
    std::span<const css::ComponentValue> components) {
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

  parser::ParseError err;
  err.reason = "Invalid linejoin";
  err.location =
      !components.empty() ? components.front().sourceOffset() : parser::FileOffset::Offset(0);
  return err;
}

parser::ParseResult<std::vector<Lengthd>> ParseStrokeDasharray(
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
        parser::ParseError err;
        err.reason = "Unexpected token in dasharray";
        err.location = !components.empty() ? components.front().sourceOffset()
                                           : parser::FileOffset::EndOfString();
        return err;
      }
    }

    const css::ComponentValue& component = components.front();
    if (const auto* dimension = component.tryGetToken<css::Token::Dimension>()) {
      if (!dimension->suffixUnit) {
        parser::ParseError err;
        err.reason = "Invalid unit on length";
        err.location = component.sourceOffset();
        return err;
      } else {
        result.emplace_back(dimension->value, dimension->suffixUnit.value());
      }
    } else if (const auto* percentage = component.tryGetToken<css::Token::Percentage>()) {
      result.emplace_back(percentage->value, Lengthd::Unit::Percent);
    } else if (const auto* number = component.tryGetToken<css::Token::Number>()) {
      result.emplace_back(number->value, Lengthd::Unit::None);
    } else {
      parser::ParseError err;
      err.reason = "Unexpected token in dasharray";
      err.location = component.sourceOffset();
      return err;
    }

    components = components.subspan(1);
  }

  return result;
}

parser::ParseResult<FilterEffect> ParseFilter(std::span<const css::ComponentValue> components) {
  // TODO: Handle parsing a list of filter effects
  // https://www.w3.org/TR/filter-effects/#FilterProperty
  if (components.empty()) {
    parser::ParseError err;
    err.reason = "Invalid filter value";
    return err;
  }

  const css::ComponentValue& firstComponent = components.front();
  if (firstComponent.is<css::Token>()) {
    const auto& token = firstComponent.get<css::Token>();
    if (token.is<css::Token::Ident>()) {
      const RcString& name = token.get<css::Token::Ident>().value;

      if (name.equalsLowercase("none")) {
        return FilterEffect(FilterEffect::None());
      }
    } else if (token.is<css::Token::Url>()) {
      const auto& url = token.get<css::Token::Url>();

      return FilterEffect(FilterEffect::ElementReference(url.value));
    }
  } else if (firstComponent.is<css::Function>()) {
    const auto& function = firstComponent.get<css::Function>();
    if (function.name.equalsLowercase("blur")) {
      // Parse optional length value as the stdDeviation.
      if (function.values.empty()) {
        return FilterEffect(FilterEffect::Blur(Lengthd(0.0, Lengthd::Unit::Px)));
      } else if (function.values.size() == 1) {
        const auto& arg = function.values.front();
        if (const auto* dimension = arg.tryGetToken<css::Token::Dimension>()) {
          if (!dimension->suffixUnit || dimension->suffixUnit == Lengthd::Unit::Percent) {
            parser::ParseError err;
            err.reason = "Invalid unit on length";
            err.location = arg.sourceOffset();
            return err;
          } else {
            const Lengthd stdDeviation(dimension->value, dimension->suffixUnit.value());
            return FilterEffect(FilterEffect::Blur(stdDeviation, stdDeviation));
          }
        } else {
          parser::ParseError err;
          err.reason = "Invalid blur value";
          err.location = arg.sourceOffset();
          return err;
        }
      }
    }
  }

  parser::ParseError err;
  err.reason = "Invalid filter value";
  err.location = firstComponent.sourceOffset();
  return err;
}

// List of valid presentation attributes from
// https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
static constexpr frozen::unordered_set<frozen::string, 15> kValidPresentationAttributes = {
    "cx",   "cy",        "height",     "width",        "x",     "y", "r", "rx", "ry", "d",
    "fill", "transform", "stop-color", "stop-opacity", "filter"
    // The properties which may apply to any element in the SVG namespace are omitted.
};

static constexpr frozen::unordered_map<frozen::string, PropertyParseFn, 16> kProperties = {
    {"color",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       auto maybeError = Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return css::parser::ColorParser::Parse(params.components());
           },
           &registry.color);
       if (maybeError.has_value()) {
         return maybeError;
       }

       // From https://www.w3.org/TR/css-color-3/#currentcolor:
       // If the ‘currentColor’ keyword is set on the ‘color’ property itself, it is treated as
       // ‘color: inherit’.
       if (registry.color.hasValue() && registry.color.getRequired().isCurrentColor()) {
         registry.color.set(PropertyState::Inherit, registry.color.specificity);
       }

       return maybeError;
     }},  //
    {"display",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseDisplay(params.components());
           },
           &registry.display);
     }},  //
    {"opacity",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return parser::ParseAlphaValue(params.components());
           },
           &registry.opacity);
     }},  //
    {"visibility",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseVisibility(params.components());
           },
           &registry.visibility);
     }},  //
    {"fill",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParsePaintServer(params.components());
           },
           &registry.fill);
     }},  //
    {"fill-rule",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseFillRule(params.components());
           },
           &registry.fillRule);
     }},  //
    {"fill-opacity",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return parser::ParseAlphaValue(params.components());
           },
           &registry.fillOpacity);
     }},  //
    {"stroke",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParsePaintServer(params.components());
           },
           &registry.stroke);
     }},  //
    {"stroke-opacity",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return parser::ParseAlphaValue(params.components());
           },
           &registry.strokeOpacity);
     }},  //
    {"stroke-width",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
           },
           &registry.strokeWidth);
     }},  //
    {"stroke-linecap",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseStrokeLinecap(params.components());
           },
           &registry.strokeLinecap);
     }},  //
    {"stroke-linejoin",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseStrokeLinejoin(params.components());
           },
           &registry.strokeLinejoin);
     }},  //
    {"stroke-miterlimit",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseNumber(params.components());
           },
           &registry.strokeMiterlimit);
     }},  //
    {"stroke-dasharray",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseStrokeDasharray(params.components());
           },
           &registry.strokeDasharray);
     }},  //
    {"stroke-dashoffset",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return parser::ParseLengthPercentage(params.components(), params.allowUserUnits());
           },
           &registry.strokeDashoffset);
     }},  //
    {"filter",
     [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
       return Parse(
           params,
           [](const parser::PropertyParseFnParams& params) {
             return ParseFilter(params.components());
           },
           &registry.filter);
     }},  //
};

}  // namespace

PropertyRegistry::PropertyRegistry() = default;

PropertyRegistry::~PropertyRegistry() = default;

PropertyRegistry::PropertyRegistry(const PropertyRegistry&) = default;
PropertyRegistry::PropertyRegistry(PropertyRegistry&&) noexcept = default;
PropertyRegistry& PropertyRegistry::operator=(const PropertyRegistry&) = default;
PropertyRegistry& PropertyRegistry::operator=(PropertyRegistry&&) noexcept = default;

[[nodiscard]] PropertyRegistry PropertyRegistry::inheritFrom(const PropertyRegistry& parent,
                                                             PropertyInheritOptions options) const {
  PropertyRegistry result;
  result.unparsedProperties = unparsedProperties;  // Unparsed properties are not inherited.

  auto resultProperties = result.allPropertiesMutable();
  const auto parentProperties = parent.allProperties();
  const auto selfProperties = allProperties();

  forEachProperty<0, numProperties()>([&resultProperties, &parentProperties, &selfProperties,
                                       options](auto i) {
    auto res =
        std::get<i.value>(selfProperties).inheritFrom(std::get<i.value>(parentProperties), options);

    std::get<i.value>(resultProperties) = res;
  });

  return result;
}

void PropertyRegistry::resolveUnits(const Boxd& viewbox, const FontMetrics& fontMetrics) {
  std::apply([&viewbox, &fontMetrics](
                 auto&&... property) { (property.resolveUnits(viewbox, fontMetrics), ...); },
             std::tuple(allPropertiesMutable()));
}

std::optional<parser::ParseError> PropertyRegistry::parseProperty(
    const css::Declaration& declaration, css::Specificity specificity) {
  const frozen::string frozenName(declaration.name);
  const auto it = kProperties.find(frozenName);
  if (it != kProperties.end()) {
    return it->second(*this, CreateParseFnParams(declaration, specificity,
                                                 parser::PropertyParseBehavior::AllowUserUnits));
  }

  // Only store unparsed properties if they are valid presentation attribute names.
  if (kValidPresentationAttributes.count(frozenName) != 0) {
    unparsedProperties.emplace(
        std::make_pair(declaration.name, parser::UnparsedProperty{declaration, specificity}));
  } else {
    parser::ParseError err;
    err.reason = "Unknown property '" + declaration.name + "'";
    err.location = declaration.sourceOffset;
    return err;
  }

  return std::nullopt;
}

void PropertyRegistry::parseStyle(std::string_view str) {
  const std::vector<css::Declaration> declarations = css::CSS::ParseStyleAttribute(str);
  for (const auto& declaration : declarations) {
    std::ignore = parseProperty(declaration, css::Specificity::StyleAttribute());
  }
}

parser::ParseResult<bool> PropertyRegistry::parsePresentationAttribute(
    std::string_view name, std::string_view value, std::optional<ElementType> type,
    EntityHandle handle) {
  /* TODO: The SVG2 spec says the name may be similar to the attribute, not necessarily the same.
   * There may need to be a second mapping.
   */
  /* For attributes, fields may be unitless, in which case they are specified in "user units",
   * see https://www.w3.org/TR/SVG2/coords.html#TermUserUnits. For this case, the spec says to
   * adjust the grammar to modify things like <length> to [<length> | <number>], see
   * https://www.w3.org/TR/SVG2/types.html#syntax.
   *
   * In practice, we propagate an "AllowUserUnits" flag. "User units" are specified as being
   * equivalent to pixels.
   */
  assert((!type.has_value() || (type.has_value() && handle != EntityHandle())) &&
         "If a type is specified, entity handle must be set");

  parser::PropertyParseFnParams params;
  params.valueOrComponents = value;
  params.specificity = kSpecificityPresentationAttribute;
  params.parseBehavior = parser::PropertyParseBehavior::AllowUserUnits;

  const auto it = kProperties.find(frozen::string(name));
  if (it != kProperties.end()) {
    auto maybeError = it->second(*this, params);
    if (maybeError.has_value()) {
      return std::move(maybeError.value());
    }

    return true;
  }

  return ParseSpecialAttributes(params, name, type, handle);
}

std::ostream& operator<<(std::ostream& os, const PropertyRegistry& registry) {
  os << "PropertyRegistry {\n";

  const auto resultProperties = const_cast<PropertyRegistry&>(registry).allProperties();
  PropertyRegistry::forEachProperty<0, PropertyRegistry::numProperties()>(
      [&os, &resultProperties](auto i) {
        const auto& property = std::get<i.value>(resultProperties);
        if (property.hasValue()) {
          os << "  " << property << std::endl;
        }
      });

  return os << "}\n";
}

}  // namespace donner::svg
