#include "donner/svg/properties/PropertyRegistry.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "donner/base/CompileTimeMap.h"
#include "donner/base/SmallVector.h"
#include "donner/css/CSS.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/TransformOrigin.h"
#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg {

namespace {

template <typename T>
bool TrySkipToken(std::span<const css::ComponentValue>& components) {
  if (!components.empty() && components.front().isToken<T>()) {
    components = components.subspan(1);
    return true;
  }
  return false;
}

/// Returns true if whitespace tokens were skipped.
bool SkipWhitespace(std::span<const css::ComponentValue>& components) {
  bool foundWhitespace = false;

  while (!components.empty() && components.front().isToken<css::Token::Whitespace>()) {
    components = components.subspan(1);
    foundWhitespace = true;
  }

  return foundWhitespace;
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
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

ParseResult<Display> ParseDisplay(std::span<const css::ComponentValue> components) {
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

  ParseError err;
  err.reason = "Invalid display value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

ParseResult<Visibility> ParseVisibility(std::span<const css::ComponentValue> components) {
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

  ParseError err;
  err.reason = "Invalid display value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

ParseResult<Overflow> ParseOverflow(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("visible")) {
        return Overflow::Visible;
      } else if (value.equalsLowercase("hidden")) {
        return Overflow::Hidden;
      } else if (value.equalsLowercase("scroll")) {
        return Overflow::Scroll;
      } else if (value.equalsLowercase("auto")) {
        return Overflow::Auto;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid overflow value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
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
              // TODO(jwmcglynn): Is there a difference between omitted and "none"?
              return PaintServer(PaintServer::ElementReference(url.value, std::nullopt));
            }
          }
        }

        // If we couldn't identify a fallback yet, try parsing it as a color.
        auto colorResult = css::parser::ColorParser::Parse(components.subspan(i));
        if (colorResult.hasResult()) {
          return PaintServer(PaintServer::ElementReference(url.value, colorResult.result()));
        } else {
          // Invalid paint.
          ParseError err;
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
    return PaintServer(PaintServer::Solid(colorResult.result()));
  }

  ParseError err;
  err.reason = "Invalid paint server";
  err.location = firstComponent.sourceOffset();
  return err;
}

ParseResult<FillRule> ParseFillRule(std::span<const css::ComponentValue> components) {
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

  ParseError err;
  err.reason = "Invalid fill rule";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

ParseResult<ClipRule> ParseClipRule(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("nonzero")) {
        return ClipRule::NonZero;
      } else if (value.equalsLowercase("evenodd")) {
        return ClipRule::EvenOdd;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid clip-rule value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
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
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
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
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

ParseResult<StrokeDasharray> ParseStrokeDasharray(std::span<const css::ComponentValue> components) {
  // https://www.w3.org/TR/css-values-4/#mult-comma
  StrokeDasharray result;

  while (!components.empty()) {
    if (!result.empty()) {
      if (TrySkipToken<css::Token::Whitespace>(components) ||
          TrySkipToken<css::Token::Comma>(components) || components.empty()) {
        TrySkipToken<css::Token::Whitespace>(components);
      } else {
        ParseError err;
        err.reason = "Unexpected tokens after dasharray value";
        err.location =
            !components.empty() ? components.front().sourceOffset() : FileOffset::EndOfString();
        return err;
      }
    }

    if (components.empty()) {
      break;
    }

    const css::ComponentValue& component = components.front();
    if (const auto* dimension = component.tryGetToken<css::Token::Dimension>()) {
      if (!dimension->suffixUnit) {
        ParseError err;
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
      ParseError err;
      err.reason = "Unexpected token in dasharray";
      err.location = component.sourceOffset();
      return err;
    }

    components = components.subspan(1);
  }

  return result;
}

ParseResult<TransformOrigin> ParseTransformOrigin(std::span<const css::ComponentValue> components) {
  SkipWhitespace(components);

  auto parseCoord = [](const css::ComponentValue& component, bool isY) -> ParseResult<Lengthd> {
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;
      if (!isY) {
        if (value.equalsLowercase("left")) {
          return Lengthd(0, Lengthd::Unit::Percent);
        } else if (value.equalsLowercase("right")) {
          return Lengthd(100, Lengthd::Unit::Percent);
        } else if (value.equalsLowercase("center")) {
          return Lengthd(50, Lengthd::Unit::Percent);
        }
      } else {
        if (value.equalsLowercase("top")) {
          return Lengthd(0, Lengthd::Unit::Percent);
        } else if (value.equalsLowercase("bottom")) {
          return Lengthd(100, Lengthd::Unit::Percent);
        } else if (value.equalsLowercase("center")) {
          return Lengthd(50, Lengthd::Unit::Percent);
        }
      }
    }

    return parser::ParseLengthPercentage(component, true);
  };

  TransformOrigin result{Lengthd(50, Lengthd::Unit::Percent), Lengthd(50, Lengthd::Unit::Percent)};

  if (!components.empty()) {
    auto first = parseCoord(components.front(), false);
    if (first.hasError()) {
      return std::move(first.error());
    }
    result.x = first.result();
    components = components.subspan(1);

    if (!SkipWhitespace(components)) {
      ParseError err;
      err.reason = "Unexpected token in transform-origin";
      err.location =
          components.empty() ? FileOffset::EndOfString() : components.front().sourceOffset();

      return err;
    }

    if (!components.empty()) {
      auto second = parseCoord(components.front(), true);
      if (second.hasError()) {
        return std::move(second.error());
      }
      result.y = second.result();
      components = components.subspan(1);
    }

    SkipWhitespace(components);

    if (!components.empty()) {
      ParseError err;
      err.reason = "Unexpected token in transform-origin";
      err.location = components.front().sourceOffset();
      return err;
    }
  }

  return result;
}

ParseResult<Reference> ParseReference(std::string_view tag,
                                      std::span<const css::ComponentValue> components) {
  if (components.empty()) {
    ParseError err;
    err.reason = std::string("Empty ") + std::string(tag) + " value";
    return err;
  }

  const css::ComponentValue& firstComponent = components.front();
  if (firstComponent.is<css::Token>()) {
    const auto& token = firstComponent.get<css::Token>();
    if (token.is<css::Token::Url>()) {
      const auto& url = token.get<css::Token::Url>();
      return Reference{url.value};
    }
  }

  ParseError err;
  err.reason = "Invalid url reference";
  err.location = firstComponent.sourceOffset();
  return err;
}

ParseResult<FilterEffect> ParseFilter(std::span<const css::ComponentValue> components) {
  // TODO(https://github.com/jwmcglynn/donner/issues/151): Handle parsing a list of filter effects
  // https://www.w3.org/TR/filter-effects/#FilterProperty
  if (components.empty()) {
    ParseError err;
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
        return FilterEffect(FilterEffect::Blur{Lengthd(0.0, Lengthd::Unit::Px)});
      } else if (function.values.size() == 1) {
        const auto& arg = function.values.front();
        if (const auto* dimension = arg.tryGetToken<css::Token::Dimension>()) {
          if (!dimension->suffixUnit || dimension->suffixUnit == Lengthd::Unit::Percent) {
            ParseError err;
            err.reason = "Invalid unit on length";
            err.location = arg.sourceOffset();
            return err;
          } else {
            const Lengthd stdDeviation(dimension->value, dimension->suffixUnit.value());
            return FilterEffect(FilterEffect::Blur{stdDeviation, stdDeviation});
          }
        } else {
          ParseError err;
          err.reason = "Invalid blur value";
          err.location = arg.sourceOffset();
          return err;
        }
      }
    }
  }

  ParseError err;
  err.reason = "Invalid filter value";
  err.location = firstComponent.sourceOffset();
  return err;
}

ParseResult<PointerEvents> ParsePointerEvents(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("none")) {
        return PointerEvents::None;
      } else if (value.equalsLowercase("bounding-box")) {
        return PointerEvents::BoundingBox;
      } else if (value.equalsLowercase("visibleFill")) {
        return PointerEvents::VisibleFill;
      } else if (value.equalsLowercase("visiblePainted")) {
        return PointerEvents::VisiblePainted;
      } else if (value.equalsLowercase("visibleStroke")) {
        return PointerEvents::VisibleStroke;
      } else if (value.equalsLowercase("visible")) {
        return PointerEvents::Visible;
      } else if (value.equalsLowercase("painted")) {
        return PointerEvents::Painted;
      } else if (value.equalsLowercase("fill")) {
        return PointerEvents::Fill;
      } else if (value.equalsLowercase("stroke")) {
        return PointerEvents::Stroke;
      } else if (value.equalsLowercase("all")) {
        return PointerEvents::All;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid pointer-events";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

// List of valid presentation attributes from
// https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
constexpr std::array<std::pair<std::string_view, bool>, 70> kValidPresentationAttributeEntries{{
    {"cx", true},
    {"cy", true},
    {"height", true},
    {"width", true},
    {"x", true},
    {"y", true},
    {"r", true},
    {"rx", true},
    {"ry", true},
    {"d", true},
    {"fill", true},
    {"transform", true},
    {"alignment-baseline", true},
    {"baseline-shift", true},
    {"clip-path", true},
    {"clip-rule", true},
    {"color", true},
    {"color-interpolation", true},
    {"color-interpolation-filters", true},
    {"color-rendering", true},
    {"cursor", true},
    {"direction", true},
    {"display", true},
    {"dominant-baseline", true},
    {"fill-opacity", true},
    {"fill-rule", true},
    {"filter", true},
    {"flood-color", true},
    {"flood-opacity", true},
    {"font-family", true},
    {"font-size", true},
    {"font-size-adjust", true},
    {"font-stretch", true},
    {"font-style", true},
    {"font-variant", true},
    {"font-weight", true},
    {"glyph-orientation-horizontal", true},
    {"glyph-orientation-vertical", true},
    {"image-rendering", true},
    {"letter-spacing", true},
    {"lighting-color", true},
    {"marker-end", true},
    {"marker-mid", true},
    {"marker-start", true},
    {"mask", true},
    {"opacity", true},
    {"overflow", true},
    {"paint-order", true},
    {"pointer-events", true},
    {"shape-rendering", true},
    {"stop-color", true},
    {"stop-opacity", true},
    {"stroke", true},
    {"stroke-dasharray", true},
    {"stroke-dashoffset", true},
    {"stroke-linecap", true},
    {"stroke-linejoin", true},
    {"stroke-miterlimit", true},
    {"stroke-opacity", true},
    {"stroke-width", true},
    {"text-anchor", true},
    {"text-decoration", true},
    {"text-overflow", true},
    {"text-rendering", true},
    {"unicode-bidi", true},
    {"vector-effect", true},
    {"visibility", true},
    {"white-space", true},
    {"word-spacing", true},
    {"writing-mode", true},
}};

constexpr auto kValidPresentationAttributesResult =
    makeCompileTimeMap(kValidPresentationAttributeEntries);
static_assert(kValidPresentationAttributesResult.status == CompileTimeMapStatus::kOk);
constexpr auto kValidPresentationAttributes = kValidPresentationAttributesResult.map;

using PropertyParseFn = std::optional<ParseError> (*)(PropertyRegistry& registry,
                                                      const parser::PropertyParseFnParams& params);

using MapResult = CompileTimeMapResult<std::string_view, PropertyParseFn, 28>;

constexpr MapResult
    kPropertiesResult =
        makeCompileTimeMap<std::string_view, PropertyParseFn, 28>(
            {{
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
                   // If the 'currentColor' keyword is set on the 'color' property itself, it is
                   // treated as `color: inherit`.
                   if (registry.color.hasValue() && registry.color.getRequired().isCurrentColor()) {
                     registry.color.set(PropertyState::Inherit, registry.color.specificity);
                   }

                   return maybeError;
                 }},  //
                {"font-family",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params)
                           -> ParseResult<SmallVector<RcString, 1>> {
                         auto components = params.components();
                         SmallVector<RcString, 1> families;
                         size_t i = 0;
                         while (i < components.size()) {
                           // Skip commas and whitespace.
                           if (components[i].isToken<css::Token::Comma>() ||
                               components[i].isToken<css::Token::Whitespace>()) {
                             ++i;
                             continue;
                           }
                           // Collect one family item until the next comma.
                           size_t start = i;
                           while (i < components.size() &&
                                  !components[i].isToken<css::Token::Comma>()) {
                             ++i;
                           }
                           auto item = std::span<const css::ComponentValue>(
                               components.data() + start, i - start);
                           // Quoted family name.
                           if (item.size() == 1 && item[0].isToken<css::Token::String>()) {
                             families.emplace_back(
                                 item[0].get<css::Token>().get<css::Token::String>().value);
                           } else if (item.size() == 1 && item[0].is<css::Function>()) {
                             const auto& func = item[0].get<css::Function>();
                             if (func.name.equalsLowercase("generic")) {
                               std::string name;
                               bool first = true;
                               for (const auto& cv : func.values) {
                                 if (auto ident = cv.tryGetToken<css::Token::Ident>()) {
                                   if (!first) {
                                     name.push_back(' ');
                                   }
                                   first = false;
                                   name.append(ident->value);
                                 } else {
                                   ParseError err;
                                   err.reason = "Invalid generic-family";
                                   err.location = cv.sourceOffset();
                                   return err;
                                 }
                               }
                               families.emplace_back(RcString(name));
                             } else {
                               ParseError err;
                               err.reason = "Invalid font-family function";
                               err.location = item.front().sourceOffset();
                               return err;
                             }
                           } else {
                             // Unquoted family name (sequence of idents).
                             std::string name;
                             bool first = true;
                             for (const auto& cv : item) {
                               if (auto ident = cv.tryGetToken<css::Token::Ident>()) {
                                 if (!first) {
                                   name.push_back(' ');
                                 }
                                 first = false;
                                 name.append(ident->value);
                               } else {
                                 ParseError err;
                                 err.reason = "Invalid font-family";
                                 err.location = cv.sourceOffset();
                                 return err;
                               }
                             }
                             families.emplace_back(RcString(name));
                           }
                         }
                         return families;
                       },
                       &registry.fontFamily);
                 }},  //
                {"font-size",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return parser::ParseLengthPercentage(params.components(),
                                                              params.allowUserUnits());
                       },
                       &registry.fontSize);
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
                {"overflow",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseOverflow(params.components());
                       },
                       &registry.overflow);
                 }},  //
                {"transform-origin",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseTransformOrigin(params.components());
                       },
                       &registry.transformOrigin);
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
                         return parser::ParseLengthPercentage(params.components(),
                                                              params.allowUserUnits());
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
                         return parser::ParseLengthPercentage(params.components(),
                                                              params.allowUserUnits());
                       },
                       &registry.strokeDashoffset);
                 }},  //
                {"clip-path",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseReference("clip-path", params.components());
                       },
                       &registry.clipPath);
                 }},  //
                {"clip-rule",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseClipRule(params.components());
                       },
                       &registry.clipRule);
                 }},  //
                {"mask",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseReference("mask", params.components());
                       },
                       &registry.mask);
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
                {"pointer-events",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParsePointerEvents(params.components());
                       },
                       &registry.pointerEvents);
                 }},  //
                {"marker-start",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseReference("marker-start", params.components());
                       },
                       &registry.markerStart);
                 }},  //
                {"marker-mid",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseReference("marker-mid", params.components());
                       },
                       &registry.markerMid);
                 }},  //
                {"marker-end",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseReference("marker-end", params.components());
                       },
                       &registry.markerEnd);
                 }},  //
                {"marker",
                 [](PropertyRegistry& registry,
                    const parser::PropertyParseFnParams& params) -> std::optional<ParseError> {
                   // First, parse the shorthand value as a Reference
                   const auto parseResult = ParseReference("marker", params.components());
                   if (!parseResult.hasResult()) {
                     return parseResult.error();
                   }

                   const Reference& markerValue = parseResult.result();

                   // Now, set marker-start, marker-mid, and marker-end using the Parse function
                   std::optional<ParseError> error;

                   // Set marker-start
                   error = Parse(
                       params,
                       [markerValue](const parser::PropertyParseFnParams& /*unused*/)
                           -> ParseResult<Reference> { return markerValue; },
                       &registry.markerStart);
                   assert(!error && "Unexpected error parsing marker shorthand property");

                   // Set marker-mid
                   error = Parse(
                       params,
                       [markerValue](const parser::PropertyParseFnParams& /*unused*/)
                           -> ParseResult<Reference> { return markerValue; },
                       &registry.markerMid);
                   assert(!error && "Unexpected error parsing marker shorthand property");

                   // Set marker-end
                   error = Parse(
                       params,
                       [markerValue](const parser::PropertyParseFnParams& /*unused*/)
                           -> ParseResult<Reference> { return markerValue; },
                       &registry.markerEnd);
                   assert(!error && "Unexpected error parsing marker shorthand property");

                   return std::nullopt;  // Parsing succeeded
                 }},                     //
            }});

static_assert(kPropertiesResult.status == CompileTimeMapStatus::kOk);
constexpr auto kProperties = kPropertiesResult.map;

}  // namespace

PropertyRegistry::PropertyRegistry() = default;

PropertyRegistry::~PropertyRegistry() = default;

PropertyRegistry::PropertyRegistry(const PropertyRegistry&) = default;
PropertyRegistry::PropertyRegistry(PropertyRegistry&&) noexcept = default;
PropertyRegistry& PropertyRegistry::operator=(const PropertyRegistry&) = default;
PropertyRegistry& PropertyRegistry::operator=(PropertyRegistry&&) noexcept = default;

size_t PropertyRegistry::numPropertiesSet() const {
  const auto selfProperties = allProperties();

  size_t result = 0;
  forEachProperty<0, numProperties()>([&selfProperties, &result](auto i) {
    if (std::get<i.value>(selfProperties).hasValue()) {
      ++result;
    }
  });

  return result;
}

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

std::optional<ParseError> PropertyRegistry::parseProperty(const css::Declaration& declaration,
                                                          css::Specificity specificity) {
  const std::string_view name(declaration.name);
  const PropertyParseFn* parseFn = kProperties.find(name);
  if (parseFn != nullptr) {
    return (*parseFn)(*this,
                      parser::PropertyParseFnParams::Create(
                          declaration, specificity, parser::PropertyParseBehavior::AllowUserUnits));
  }

  // Only store unparsed properties if they are valid presentation attribute names.
  if (kValidPresentationAttributes.contains(name)) {
    unparsedProperties.emplace(
        std::make_pair(declaration.name, parser::UnparsedProperty{declaration, specificity}));
  } else {
    ParseError err;
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

ParseResult<bool> PropertyRegistry::parsePresentationAttribute(std::string_view name,
                                                               std::string_view value,
                                                               std::optional<ElementType> type,
                                                               EntityHandle handle) {
  /* TODO(jwmcglynn): The SVG2 spec says the name may be similar to the attribute, not necessarily
   * the same. There may need to be a second mapping.
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

  if (!kValidPresentationAttributes.contains(name)) {
    return false;
  }

  parser::PropertyParseFnParams params = parser::PropertyParseFnParams::CreateForAttribute(value);

  const PropertyParseFn* parseFn = kProperties.find(name);
  if (parseFn != nullptr) {
    auto maybeError = (*parseFn)(*this, params);
    if (maybeError.has_value()) {
      return std::move(maybeError.value());
    }

    return true;
  }

  return ParseSpecialAttributes(params, name, type, handle);
}

std::ostream& operator<<(std::ostream& os, const PropertyRegistry& registry) {
  os << "PropertyRegistry {\n";

  // const_cast avoids unnecessary code duplication for `allProperties()` for a debug codepath.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
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
