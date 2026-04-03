#include "donner/svg/properties/PropertyRegistry.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "donner/base/CompileTimeMap.h"
#include "donner/base/MathUtils.h"
#include "donner/base/SmallVector.h"
#include "donner/css/CSS.h"
#include "donner/css/parser/ColorParser.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/TransformOrigin.h"
#include "donner/svg/parser/CssTransformParser.h"
#include "donner/svg/parser/LengthPercentageParser.h"
#include "donner/svg/parser/TransformParser.h"
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

ParseResult<TextAnchor> ParseTextAnchor(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("start")) {
        return TextAnchor::Start;
      } else if (value.equalsLowercase("middle")) {
        return TextAnchor::Middle;
      } else if (value.equalsLowercase("end")) {
        return TextAnchor::End;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid text-anchor value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

ParseResult<TextDecoration> ParseTextDecoration(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("none")) {
        return TextDecoration::None;
      } else if (value.equalsLowercase("underline")) {
        return TextDecoration::Underline;
      } else if (value.equalsLowercase("overline")) {
        return TextDecoration::Overline;
      } else if (value.equalsLowercase("line-through")) {
        return TextDecoration::LineThrough;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid text-decoration value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

ParseResult<DominantBaseline> ParseDominantBaseline(
    std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("auto")) {
        return DominantBaseline::Auto;
      } else if (value.equalsLowercase("text-bottom")) {
        return DominantBaseline::TextBottom;
      } else if (value.equalsLowercase("alphabetic")) {
        return DominantBaseline::Alphabetic;
      } else if (value.equalsLowercase("ideographic")) {
        return DominantBaseline::Ideographic;
      } else if (value.equalsLowercase("middle")) {
        return DominantBaseline::Middle;
      } else if (value.equalsLowercase("central")) {
        return DominantBaseline::Central;
      } else if (value.equalsLowercase("mathematical")) {
        return DominantBaseline::Mathematical;
      } else if (value.equalsLowercase("hanging")) {
        return DominantBaseline::Hanging;
      } else if (value.equalsLowercase("text-top")) {
        return DominantBaseline::TextTop;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid dominant-baseline value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

/// Parse CSS isolation property: auto | isolate.
ParseResult<Isolation> ParseIsolation(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    if (const auto* ident = components.front().tryGetToken<css::Token::Ident>()) {
      if (ident->value.equalsLowercase("auto")) return Isolation::Auto;
      if (ident->value.equalsLowercase("isolate")) return Isolation::Isolate;
    }
  }
  ParseError err;
  err.reason = "Invalid isolation value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

/// Parse mix-blend-mode CSS keyword values.
ParseResult<MixBlendMode> ParseMixBlendMode(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    if (const auto* ident = components.front().tryGetToken<css::Token::Ident>()) {
      const RcString& v = ident->value;
      if (v.equalsLowercase("normal")) return MixBlendMode::Normal;
      if (v.equalsLowercase("multiply")) return MixBlendMode::Multiply;
      if (v.equalsLowercase("screen")) return MixBlendMode::Screen;
      if (v.equalsLowercase("overlay")) return MixBlendMode::Overlay;
      if (v.equalsLowercase("darken")) return MixBlendMode::Darken;
      if (v.equalsLowercase("lighten")) return MixBlendMode::Lighten;
      if (v.equalsLowercase("color-dodge")) return MixBlendMode::ColorDodge;
      if (v.equalsLowercase("color-burn")) return MixBlendMode::ColorBurn;
      if (v.equalsLowercase("hard-light")) return MixBlendMode::HardLight;
      if (v.equalsLowercase("soft-light")) return MixBlendMode::SoftLight;
      if (v.equalsLowercase("difference")) return MixBlendMode::Difference;
      if (v.equalsLowercase("exclusion")) return MixBlendMode::Exclusion;
      if (v.equalsLowercase("hue")) return MixBlendMode::Hue;
      if (v.equalsLowercase("saturation")) return MixBlendMode::Saturation;
      if (v.equalsLowercase("color")) return MixBlendMode::Color;
      if (v.equalsLowercase("luminosity")) return MixBlendMode::Luminosity;
    }
  }
  ParseError err;
  err.reason = "Invalid mix-blend-mode value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

/// Parse writing-mode: SVG1 values (lr-tb, lr, rl-tb, rl, tb-rl, tb, tb-lr) and
/// CSS3 values (horizontal-tb, vertical-rl, vertical-lr).
ParseResult<WritingMode> ParseWritingMode(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    if (const auto* ident = components.front().tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;
      // CSS3 values.
      if (value.equalsLowercase("horizontal-tb")) {
        return WritingMode::HorizontalTb;
      }
      if (value.equalsLowercase("vertical-rl")) {
        return WritingMode::VerticalRl;
      }
      if (value.equalsLowercase("vertical-lr")) {
        return WritingMode::VerticalLr;
      }
      // SVG1 values.
      if (value.equalsLowercase("lr-tb") || value.equalsLowercase("lr") ||
          value.equalsLowercase("rl-tb") || value.equalsLowercase("rl")) {
        return WritingMode::HorizontalTb;
      }
      if (value.equalsLowercase("tb-rl") || value.equalsLowercase("tb")) {
        return WritingMode::VerticalRl;
      }
      if (value.equalsLowercase("tb-lr")) {
        return WritingMode::VerticalLr;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid writing-mode value";
  err.location = !components.empty() ? components.front().sourceOffset() : FileOffset::Offset(0);
  return err;
}

/// Parse "baseline | sub | super | <length> | <percentage>" for baseline-shift.
/// "sub" and "super" are converted to em-relative values matching typical browser rendering.
/// Percentages are relative to font-size per SVG spec, converted to em units.
/// Positive values shift the baseline up (per CSS convention).
ParseResult<Lengthd> ParseBaselineShift(std::span<const css::ComponentValue> components,
                                        bool allowUserUnits) {
  if (components.size() == 1) {
    if (const auto* ident = components.front().tryGetToken<css::Token::Ident>()) {
      if (ident->value.equalsLowercase("baseline")) {
        return Lengthd(0, Lengthd::Unit::None);
      }
      if (ident->value.equalsLowercase("sub")) {
        // Subscript: shift down (~33% of font-size). Negative = down per CSS convention.
        return Lengthd(-0.33, Lengthd::Unit::Em);
      }
      if (ident->value.equalsLowercase("super")) {
        // Superscript: shift up (~40% of font-size). Positive = up per CSS convention.
        return Lengthd(0.4, Lengthd::Unit::Em);
      }
    }
    // Check for percentage: convert to em (percentage of font-size per SVG spec).
    if (const auto* pct = components.front().tryGetToken<css::Token::Percentage>()) {
      return Lengthd(pct->value / 100.0, Lengthd::Unit::Em);
    }
  }
  return parser::ParseLengthPercentage(components, allowUserUnits);
}

/// Parse "normal | <length>" for letter-spacing and word-spacing.
/// "normal" is treated as Lengthd(0).
ParseResult<Lengthd> ParseSpacingValue(std::span<const css::ComponentValue> components,
                                       bool allowUserUnits) {
  if (components.size() == 1) {
    if (const auto* ident = components.front().tryGetToken<css::Token::Ident>()) {
      if (ident->value.equalsLowercase("normal")) {
        return Lengthd(0, Lengthd::Unit::None);
      }
    }
  }
  return parser::ParseLengthPercentage(components, allowUserUnits);
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

ParseResult<ColorInterpolationFilters> ParseColorInterpolationFilters(
    std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("srgb")) {
        return ColorInterpolationFilters::SRGB;
      } else if (value.equalsLowercase("linearrgb")) {
        return ColorInterpolationFilters::LinearRGB;
      } else if (value.equalsLowercase("auto")) {
        return ColorInterpolationFilters::LinearRGB;  // SVG spec: auto = linearRGB
      }
    }
  }

  ParseError err;
  err.reason = "Invalid color-interpolation-filters value";
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

/// Parse a CSS angle value from a Dimension token, converting to degrees.
std::optional<double> ParseAngleDegrees(const css::Token::Dimension& dimension) {
  const RcString& suffix = dimension.suffixString;
  if (suffix.equalsLowercase("deg")) {
    return dimension.value;
  } else if (suffix.equalsLowercase("grad")) {
    return dimension.value / 400.0 * 360.0;
  } else if (suffix.equalsLowercase("rad")) {
    return dimension.value * MathConstants<double>::kRadToDeg;
  } else if (suffix.equalsLowercase("turn")) {
    return dimension.value * 360.0;
  }
  return std::nullopt;
}

/// Parse a number-or-percentage argument from a filter function's values.
/// Returns the amount as a decimal (e.g. 50% → 0.5, 2 → 2.0).
/// If values are empty, returns the default value.
ParseResult<double> ParseNumberPercentage(std::span<const css::ComponentValue> values,
                                          double defaultValue) {
  if (values.empty()) {
    return defaultValue;
  }

  std::span<const css::ComponentValue> remaining = values;
  SkipWhitespace(remaining);

  if (remaining.empty()) {
    return defaultValue;
  }

  const auto& arg = remaining.front();
  if (const auto* number = arg.tryGetToken<css::Token::Number>()) {
    return number->value;
  } else if (const auto* percentage = arg.tryGetToken<css::Token::Percentage>()) {
    return percentage->value / 100.0;
  }

  ParseError err;
  err.reason = "Expected number or percentage";
  err.location = arg.sourceOffset();
  return err;
}

/// Parse a single CSS filter function and return the corresponding FilterEffect.
ParseResult<FilterEffect> ParseFilterFunction(const css::Function& function) {
  if (function.name.equalsLowercase("blur")) {
    if (function.values.empty()) {
      return FilterEffect(
          FilterEffect::Blur{Lengthd(0.0, Lengthd::Unit::Px), Lengthd(0.0, Lengthd::Unit::Px)});
    }
    std::span<const css::ComponentValue> remaining = function.values;
    SkipWhitespace(remaining);
    if (remaining.empty()) {
      return FilterEffect(
          FilterEffect::Blur{Lengthd(0.0, Lengthd::Unit::Px), Lengthd(0.0, Lengthd::Unit::Px)});
    }
    const auto& arg = remaining.front();
    if (const auto* dimension = arg.tryGetToken<css::Token::Dimension>()) {
      if (!dimension->suffixUnit || dimension->suffixUnit == Lengthd::Unit::Percent) {
        ParseError err;
        err.reason = "Invalid unit on blur length";
        err.location = arg.sourceOffset();
        return err;
      }
      const Lengthd stdDeviation(dimension->value, dimension->suffixUnit.value());
      return FilterEffect(FilterEffect::Blur{stdDeviation, stdDeviation});
    }
    // Accept bare 0 as 0px.
    if (const auto* number = arg.tryGetToken<css::Token::Number>()) {
      if (number->value == 0.0) {
        return FilterEffect(
            FilterEffect::Blur{Lengthd(0.0, Lengthd::Unit::Px), Lengthd(0.0, Lengthd::Unit::Px)});
      }
    }
    ParseError err;
    err.reason = "Invalid blur value";
    err.location = arg.sourceOffset();
    return err;
  }

  if (function.name.equalsLowercase("hue-rotate")) {
    if (function.values.empty()) {
      return FilterEffect(FilterEffect::HueRotate{0.0});
    }
    std::span<const css::ComponentValue> remaining = function.values;
    SkipWhitespace(remaining);
    if (remaining.empty()) {
      return FilterEffect(FilterEffect::HueRotate{0.0});
    }
    const auto& arg = remaining.front();
    if (const auto* dimension = arg.tryGetToken<css::Token::Dimension>()) {
      if (auto degrees = ParseAngleDegrees(*dimension)) {
        return FilterEffect(FilterEffect::HueRotate{*degrees});
      }
      ParseError err;
      err.reason = "Invalid angle unit for hue-rotate";
      err.location = arg.sourceOffset();
      return err;
    }
    // Accept bare 0 as 0deg.
    if (const auto* number = arg.tryGetToken<css::Token::Number>()) {
      if (number->value == 0.0) {
        return FilterEffect(FilterEffect::HueRotate{0.0});
      }
    }
    ParseError err;
    err.reason = "Invalid hue-rotate value";
    err.location = arg.sourceOffset();
    return err;
  }

  if (function.name.equalsLowercase("brightness")) {
    auto result = ParseNumberPercentage(function.values, 1.0);
    if (result.hasError()) {
      return std::move(result.error());
    }
    // CSS Filter Effects Level 1: brightness() does not allow negative values.
    if (result.result() < 0.0) {
      ParseError err;
      err.reason = "Negative value not allowed for brightness()";
      err.location = function.sourceOffset;
      return err;
    }
    return FilterEffect(FilterEffect::Brightness{result.result()});
  }

  if (function.name.equalsLowercase("contrast")) {
    auto result = ParseNumberPercentage(function.values, 1.0);
    if (result.hasError()) {
      return std::move(result.error());
    }
    // CSS Filter Effects Level 1: contrast() does not allow negative values.
    if (result.result() < 0.0) {
      ParseError err;
      err.reason = "Negative value not allowed for contrast()";
      err.location = function.sourceOffset;
      return err;
    }
    return FilterEffect(FilterEffect::Contrast{result.result()});
  }

  if (function.name.equalsLowercase("grayscale")) {
    auto result = ParseNumberPercentage(function.values, 1.0);
    if (result.hasError()) {
      return std::move(result.error());
    }
    return FilterEffect(FilterEffect::Grayscale{std::clamp(result.result(), 0.0, 1.0)});
  }

  if (function.name.equalsLowercase("invert")) {
    auto result = ParseNumberPercentage(function.values, 1.0);
    if (result.hasError()) {
      return std::move(result.error());
    }
    return FilterEffect(FilterEffect::Invert{std::clamp(result.result(), 0.0, 1.0)});
  }

  if (function.name.equalsLowercase("opacity")) {
    auto result = ParseNumberPercentage(function.values, 1.0);
    if (result.hasError()) {
      return std::move(result.error());
    }
    return FilterEffect(FilterEffect::FilterOpacity{std::clamp(result.result(), 0.0, 1.0)});
  }

  if (function.name.equalsLowercase("saturate")) {
    auto result = ParseNumberPercentage(function.values, 1.0);
    if (result.hasError()) {
      return std::move(result.error());
    }
    // CSS Filter Effects Level 1: saturate() does not allow negative values.
    if (result.result() < 0.0) {
      ParseError err;
      err.reason = "Negative value not allowed for saturate()";
      err.location = function.sourceOffset;
      return err;
    }
    return FilterEffect(FilterEffect::Saturate{result.result()});
  }

  if (function.name.equalsLowercase("sepia")) {
    auto result = ParseNumberPercentage(function.values, 1.0);
    if (result.hasError()) {
      return std::move(result.error());
    }
    return FilterEffect(FilterEffect::Sepia{std::clamp(result.result(), 0.0, 1.0)});
  }

  if (function.name.equalsLowercase("drop-shadow")) {
    // Syntax: drop-shadow(<color>? <offset-x> <offset-y> <blur-radius>? <color>?)
    std::span<const css::ComponentValue> remaining = function.values;
    SkipWhitespace(remaining);

    css::Color color{css::Color::CurrentColor{}};
    bool foundColor = false;

    // Try to parse leading color (named color, hex, or color function).
    // ColorParser::Parse requires exactly one ComponentValue, so pass only the first token.
    if (!remaining.empty()) {
      std::span<const css::ComponentValue> singleToken = remaining.subspan(0, 1);
      auto colorResult = css::parser::ColorParser::Parse(singleToken);
      if (!colorResult.hasError()) {
        color = colorResult.result();
        foundColor = true;
        remaining = remaining.subspan(1);
        SkipWhitespace(remaining);
      }
    }

    // Helper to parse a length value (dimension or unitless number).
    auto parseLengthArg = [](std::span<const css::ComponentValue>& rem) -> std::optional<Lengthd> {
      if (rem.empty()) {
        return std::nullopt;
      }
      const auto& arg = rem.front();
      if (const auto* dim = arg.tryGetToken<css::Token::Dimension>()) {
        if (dim->suffixUnit && *dim->suffixUnit != Lengthd::Unit::Percent) {
          rem = rem.subspan(1);
          return Lengthd(dim->value, *dim->suffixUnit);
        }
      } else if (const auto* num = arg.tryGetToken<css::Token::Number>()) {
        rem = rem.subspan(1);
        return Lengthd(num->value, Lengthd::Unit::None);
      }
      return std::nullopt;
    };

    // Parse offset-x (required).
    auto offsetX = parseLengthArg(remaining);
    if (!offsetX) {
      ParseError err;
      err.reason = "Expected offset-x for drop-shadow";
      if (!remaining.empty()) {
        err.location = remaining.front().sourceOffset();
      }
      return err;
    }
    SkipWhitespace(remaining);

    // Parse offset-y (required).
    auto offsetY = parseLengthArg(remaining);
    if (!offsetY) {
      ParseError err;
      err.reason = "Expected offset-y for drop-shadow";
      if (!remaining.empty()) {
        err.location = remaining.front().sourceOffset();
      }
      return err;
    }
    SkipWhitespace(remaining);

    // Parse optional blur-radius.
    Lengthd stdDeviation(0.0, Lengthd::Unit::Px);
    if (auto blur = parseLengthArg(remaining)) {
      stdDeviation = *blur;
      SkipWhitespace(remaining);
    }

    // Try to parse trailing color if not found yet.
    if (!foundColor && !remaining.empty()) {
      std::span<const css::ComponentValue> singleToken = remaining.subspan(0, 1);
      auto colorResult = css::parser::ColorParser::Parse(singleToken);
      if (!colorResult.hasError()) {
        color = colorResult.result();
        remaining = remaining.subspan(1);
        SkipWhitespace(remaining);
      }
    }

    // Reject if there are extra tokens (per spec, invalid functions are ignored).
    if (!remaining.empty()) {
      ParseError err;
      err.reason = "Unexpected extra values in drop-shadow()";
      err.location = remaining.front().sourceOffset();
      return err;
    }

    return FilterEffect(FilterEffect::DropShadow{*offsetX, *offsetY, stdDeviation, color});
  }

  ParseError err;
  err.reason = "Unknown filter function '" + std::string(function.name) + "'";
  err.location = function.sourceOffset;
  return err;
}

ParseResult<std::vector<FilterEffect>> ParseFilter(
    std::span<const css::ComponentValue> components) {
  if (components.empty()) {
    ParseError err;
    err.reason = "Invalid filter value";
    return err;
  }

  std::span<const css::ComponentValue> remaining = components;
  SkipWhitespace(remaining);

  // Check for "none" keyword.
  if (remaining.size() == 1 || (remaining.size() >= 1 && remaining.front().is<css::Token>())) {
    const auto& first = remaining.front();
    if (const auto* ident = first.tryGetToken<css::Token::Ident>()) {
      if (ident->value.equalsLowercase("none")) {
        return std::vector<FilterEffect>();
      }
    }
  }

  // Parse list of filter functions and/or url() references.
  std::vector<FilterEffect> effects;
  while (!remaining.empty()) {
    SkipWhitespace(remaining);
    if (remaining.empty()) {
      break;
    }

    const css::ComponentValue& component = remaining.front();

    if (component.is<css::Token>()) {
      const auto& token = component.get<css::Token>();
      if (token.is<css::Token::Url>()) {
        const auto& url = token.get<css::Token::Url>();
        effects.emplace_back(FilterEffect::ElementReference(url.value));
        remaining = remaining.subspan(1);
        continue;
      }
      // url("...") with quotes is parsed as a function token.
    }

    if (component.is<css::Function>()) {
      const auto& function = component.get<css::Function>();
      // Handle url("...") with quoted argument.
      if (function.name.equalsLowercase("url")) {
        if (!function.values.empty()) {
          std::span<const css::ComponentValue> funcValues = function.values;
          SkipWhitespace(funcValues);
          if (!funcValues.empty()) {
            if (const auto* str = funcValues.front().tryGetToken<css::Token::String>()) {
              effects.emplace_back(FilterEffect::ElementReference(str->value));
              remaining = remaining.subspan(1);
              continue;
            }
          }
        }
      }

      auto result = ParseFilterFunction(function);
      if (result.hasError()) {
        return std::move(result.error());
      }
      effects.push_back(std::move(result.result()));
      remaining = remaining.subspan(1);
      continue;
    }

    // Skip comma separators if present.
    if (component.isToken<css::Token::Comma>()) {
      remaining = remaining.subspan(1);
      continue;
    }

    ParseError err;
    err.reason = "Invalid filter value";
    err.location = component.sourceOffset();
    return err;
  }

  if (effects.empty()) {
    ParseError err;
    err.reason = "Invalid filter value";
    return err;
  }

  return effects;
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

ParseResult<CursorType> ParseCursor(std::span<const css::ComponentValue> components) {
  if (components.size() == 1) {
    const css::ComponentValue& component = components.front();
    if (const auto* ident = component.tryGetToken<css::Token::Ident>()) {
      const RcString& value = ident->value;

      if (value.equalsLowercase("auto")) {
        return CursorType::Auto;
      } else if (value.equalsLowercase("default")) {
        return CursorType::Default;
      } else if (value.equalsLowercase("none")) {
        return CursorType::None;
      } else if (value.equalsLowercase("pointer")) {
        return CursorType::Pointer;
      } else if (value.equalsLowercase("crosshair")) {
        return CursorType::Crosshair;
      } else if (value.equalsLowercase("move")) {
        return CursorType::Move;
      } else if (value.equalsLowercase("text")) {
        return CursorType::Text;
      } else if (value.equalsLowercase("wait")) {
        return CursorType::Wait;
      } else if (value.equalsLowercase("help")) {
        return CursorType::Help;
      } else if (value.equalsLowercase("not-allowed")) {
        return CursorType::NotAllowed;
      } else if (value.equalsLowercase("grab")) {
        return CursorType::Grab;
      } else if (value.equalsLowercase("grabbing")) {
        return CursorType::Grabbing;
      } else if (value.equalsLowercase("n-resize")) {
        return CursorType::NResize;
      } else if (value.equalsLowercase("e-resize")) {
        return CursorType::EResize;
      } else if (value.equalsLowercase("s-resize")) {
        return CursorType::SResize;
      } else if (value.equalsLowercase("w-resize")) {
        return CursorType::WResize;
      } else if (value.equalsLowercase("ne-resize")) {
        return CursorType::NEResize;
      } else if (value.equalsLowercase("nw-resize")) {
        return CursorType::NWResize;
      } else if (value.equalsLowercase("se-resize")) {
        return CursorType::SEResize;
      } else if (value.equalsLowercase("sw-resize")) {
        return CursorType::SWResize;
      } else if (value.equalsLowercase("col-resize")) {
        return CursorType::ColResize;
      } else if (value.equalsLowercase("row-resize")) {
        return CursorType::RowResize;
      } else if (value.equalsLowercase("zoom-in")) {
        return CursorType::ZoomIn;
      } else if (value.equalsLowercase("zoom-out")) {
        return CursorType::ZoomOut;
      }
    }
  }

  ParseError err;
  err.reason = "Invalid cursor value";
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

constexpr auto kValidPresentationAttributes =
    makeCompileTimeMap(kValidPresentationAttributeEntries);

using PropertyParseFn = std::optional<ParseError> (*)(PropertyRegistry& registry,
                                                      const parser::PropertyParseFnParams& params);

constexpr auto kProperties =
    makeCompileTimeMap(
        std::to_array<std::pair<std::string_view, PropertyParseFn>>(
            {
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
                               if (cv.isToken<css::Token::Whitespace>()) {
                                 continue;  // Skip whitespace between idents in unquoted names.
                               }
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
                       [](const parser::PropertyParseFnParams& params) -> ParseResult<Lengthd> {
                         const auto& components = params.components();
                         // Handle font-size keywords (CSS Fonts Level 4).
                         if (components.size() == 1) {
                           if (const auto* ident =
                                   components.front().tryGetToken<css::Token::Ident>()) {
                             // Relative keywords: resolve as percentage of parent.
                             if (ident->value.equalsLowercase("larger")) {
                               return Lengthd(120, Lengthd::Unit::Percent);
                             }
                             if (ident->value.equalsLowercase("smaller")) {
                               return Lengthd(100.0 / 1.2, Lengthd::Unit::Percent);
                             }
                             // Absolute-size keywords (CSS Fonts Level 4 §2.5.1).
                             // Scaling factors relative to medium (UA default font size).
                             // https://www.w3.org/TR/css-fonts-4/#absolute-size-mapping
                             // Note that this differs from CSS2, which itself differs from CSS1 -
                             // so our font sizes are slightly different than other renderers which
                             // anchor to CSS2 (e.g. resvg).
                             constexpr double kMediumFontSize = 12.0;
                             if (ident->value.equalsLowercase("xx-small"))
                               return Lengthd(kMediumFontSize * 3.0 / 5.0, Lengthd::Unit::Px);
                             if (ident->value.equalsLowercase("x-small"))
                               return Lengthd(kMediumFontSize * 3.0 / 4.0, Lengthd::Unit::Px);
                             if (ident->value.equalsLowercase("small"))
                               return Lengthd(kMediumFontSize * 8.0 / 9.0, Lengthd::Unit::Px);
                             if (ident->value.equalsLowercase("medium"))
                               return Lengthd(kMediumFontSize, Lengthd::Unit::Px);
                             if (ident->value.equalsLowercase("large"))
                               return Lengthd(kMediumFontSize * 6.0 / 5.0, Lengthd::Unit::Px);
                             if (ident->value.equalsLowercase("x-large"))
                               return Lengthd(kMediumFontSize * 3.0 / 2.0, Lengthd::Unit::Px);
                             if (ident->value.equalsLowercase("xx-large"))
                               return Lengthd(kMediumFontSize * 2.0, Lengthd::Unit::Px);
                           }
                         }
                         return parser::ParseLengthPercentage(components, params.allowUserUnits());
                       },
                       &registry.fontSize);
                 }},  //
                {"font-weight",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) -> ParseResult<int> {
                         const auto& components = params.components();
                         if (components.size() == 1) {
                           const auto& comp = components.front();
                           if (const auto* ident = comp.tryGetToken<css::Token::Ident>()) {
                             if (ident->value.equalsLowercase("normal")) return 400;
                             if (ident->value.equalsLowercase("bold")) return 700;
                             // Relative keywords: stored as sentinels, resolved during cascade
                             // by resolveFontWeight() when the inherited value is available.
                             if (ident->value.equalsLowercase("bolder"))
                               return PropertyRegistry::kFontWeightBolder;
                             if (ident->value.equalsLowercase("lighter"))
                               return PropertyRegistry::kFontWeightLighter;
                           } else if (const auto* num = comp.tryGetToken<css::Token::Number>()) {
                             if (num->value >= 1 && num->value <= 1000) {
                               return static_cast<int>(num->value);
                             }
                           }
                         }
                         ParseError err;
                         err.reason = "Invalid font-weight value";
                         return err;
                       },
                       &registry.fontWeight);
                 }},  //
                {"font-style",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) -> ParseResult<FontStyle> {
                         const auto& components = params.components();
                         if (components.size() == 1) {
                           if (const auto* ident =
                                   components.front().tryGetToken<css::Token::Ident>()) {
                             if (ident->value.equalsLowercase("normal")) return FontStyle::Normal;
                             if (ident->value.equalsLowercase("italic")) return FontStyle::Italic;
                             if (ident->value.equalsLowercase("oblique")) return FontStyle::Oblique;
                           }
                         }
                         ParseError err;
                         err.reason = "Invalid font-style value";
                         return err;
                       },
                       &registry.fontStyle);
                 }},  //
                {"font-stretch",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) -> ParseResult<int> {
                         const auto& components = params.components();
                         if (components.size() == 1) {
                           if (const auto* ident =
                                   components.front().tryGetToken<css::Token::Ident>()) {
                             if (ident->value.equalsLowercase("normal"))
                               return static_cast<int>(FontStretch::Normal);
                             if (ident->value.equalsLowercase("ultra-condensed"))
                               return static_cast<int>(FontStretch::UltraCondensed);
                             if (ident->value.equalsLowercase("extra-condensed"))
                               return static_cast<int>(FontStretch::ExtraCondensed);
                             if (ident->value.equalsLowercase("condensed"))
                               return static_cast<int>(FontStretch::Condensed);
                             if (ident->value.equalsLowercase("semi-condensed"))
                               return static_cast<int>(FontStretch::SemiCondensed);
                             if (ident->value.equalsLowercase("semi-expanded"))
                               return static_cast<int>(FontStretch::SemiExpanded);
                             if (ident->value.equalsLowercase("expanded"))
                               return static_cast<int>(FontStretch::Expanded);
                             if (ident->value.equalsLowercase("extra-expanded"))
                               return static_cast<int>(FontStretch::ExtraExpanded);
                             if (ident->value.equalsLowercase("ultra-expanded"))
                               return static_cast<int>(FontStretch::UltraExpanded);
                             // SVG 1.1 relative keywords, stored as sentinels.
                             if (ident->value.equalsLowercase("narrower"))
                               return PropertyRegistry::kFontStretchNarrower;
                             if (ident->value.equalsLowercase("wider"))
                               return PropertyRegistry::kFontStretchWider;
                           }
                         }
                         ParseError err;
                         err.reason = "Invalid font-stretch value";
                         return err;
                       },
                       &registry.fontStretch);
                 }},  //
                {"font-variant",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) -> ParseResult<FontVariant> {
                         const auto& components = params.components();
                         if (components.size() == 1) {
                           if (const auto* ident =
                                   components.front().tryGetToken<css::Token::Ident>()) {
                             if (ident->value.equalsLowercase("normal")) return FontVariant::Normal;
                             if (ident->value.equalsLowercase("small-caps"))
                               return FontVariant::SmallCaps;
                           }
                         }
                         ParseError err;
                         err.reason = "Invalid font-variant value";
                         return err;
                       },
                       &registry.fontVariant);
                 }},  //
                {"text-anchor",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseTextAnchor(params.components());
                       },
                       &registry.textAnchor);
                 }},  //
                {"text-decoration",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseTextDecoration(params.components());
                       },
                       &registry.textDecoration);
                 }},  //
                {"dominant-baseline",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseDominantBaseline(params.components());
                       },
                       &registry.dominantBaseline);
                 }},  //
                {"letter-spacing",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseSpacingValue(params.components(), params.allowUserUnits());
                       },
                       &registry.letterSpacing);
                 }},  //
                {"word-spacing",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseSpacingValue(params.components(), params.allowUserUnits());
                       },
                       &registry.wordSpacing);
                 }},  //
                {"baseline-shift",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseBaselineShift(params.components(), params.allowUserUnits());
                       },
                       &registry.baselineShift);
                 }},  //
                {"alignment-baseline",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseDominantBaseline(params.components());
                       },
                       &registry.alignmentBaseline);
                 }},  //
                {"isolation",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseIsolation(params.components());
                       },
                       &registry.isolation);
                 }},  //
                {"mix-blend-mode",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseMixBlendMode(params.components());
                       },
                       &registry.mixBlendMode);
                 }},  //
                {"writing-mode",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseWritingMode(params.components());
                       },
                       &registry.writingMode);
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
                {"color-interpolation-filters",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseColorInterpolationFilters(params.components());
                       },
                       &registry.colorInterpolationFilters);
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
                {"cursor",
                 [](PropertyRegistry& registry, const parser::PropertyParseFnParams& params) {
                   return Parse(
                       params,
                       [](const parser::PropertyParseFnParams& params) {
                         return ParseCursor(params.components());
                       },
                       &registry.cursor);
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
            }));

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

  // Handle 'transform' as a special case — it's stored in TransformComponent, not PropertyRegistry.
  if (handle != EntityHandle() &&
      StringUtils::EqualsLowercase(name, std::string_view("transform"))) {
    auto& transform = handle.get_or_emplace<components::TransformComponent>();
    auto maybeError = parser::Parse(
        params,
        [](const parser::PropertyParseFnParams& params) {
          if (const std::string_view* str =
                  std::get_if<std::string_view>(&params.valueOrComponents)) {
            return parser::TransformParser::Parse(*str).map<CssTransform>(
                [](const Transformd& transform) { return CssTransform(transform); });
          } else {
            return parser::CssTransformParser::Parse(params.components());
          }
        },
        &transform.transform);
    if (maybeError) {
      return std::move(maybeError.value());
    }

    return true;
  }

  return false;
}

void PropertyRegistry::resolveFontSize(double parentFontSizePx) {
  const Lengthd fs = fontSize.getRequired();
  double resolvedPx;
  switch (fs.unit) {
    case Lengthd::Unit::Percent:
      // font-size: N% means N% of parent's computed font-size (NOT the viewBox).
      resolvedPx = fs.value / 100.0 * parentFontSizePx;
      break;
    case Lengthd::Unit::Em: resolvedPx = fs.value * parentFontSizePx; break;
    case Lengthd::Unit::Ex:
      // TODO(jwm): Measure the actual font's x-height instead of using the 0.5 fallback.
      resolvedPx = fs.value * 0.5 * parentFontSizePx;
      break;
    case Lengthd::Unit::Rem:
      // TODO(jwm): Thread the root element's computed font-size.
      resolvedPx = fs.value * 12.0;
      break;
    default:
      // Absolute units (px, pt, cm, etc.) — resolve with empty context.
      resolvedPx = fs.toPixels(Boxd(), FontMetrics(), Lengthd::Extent::Mixed);
      break;
  }
  fontSize.value = Lengthd(resolvedPx, Lengthd::Unit::Px);
}

void PropertyRegistry::resolveFontWeight(int parentFontWeight) {
  const int fw = fontWeight.getRequired();
  if (fw == kFontWeightBolder) {
    // CSS Fonts Level 4 §2.5: bolder relative to inherited weight.
    int resolved;
    if (parentFontWeight < 350) {
      resolved = 400;
    } else if (parentFontWeight <= 550) {
      resolved = 700;
    } else {
      resolved = 900;
    }
    fontWeight.value = resolved;
  } else if (fw == kFontWeightLighter) {
    // CSS Fonts Level 4 §2.5: lighter relative to inherited weight.
    int resolved;
    if (parentFontWeight <= 550) {
      resolved = 100;
    } else if (parentFontWeight <= 750) {
      resolved = 400;
    } else {
      resolved = 700;
    }
    fontWeight.value = resolved;
  }
}

void PropertyRegistry::resolveFontStretch(int parentFontStretch) {
  const int fs = fontStretch.getRequired();
  if (fs == kFontStretchNarrower) {
    // Move one step narrower, clamped to UltraCondensed.
    int resolved = parentFontStretch - 1;
    if (resolved < static_cast<int>(FontStretch::UltraCondensed)) {
      resolved = static_cast<int>(FontStretch::UltraCondensed);
    }
    fontStretch.value = resolved;
  } else if (fs == kFontStretchWider) {
    // Move one step wider, clamped to UltraExpanded.
    int resolved = parentFontStretch + 1;
    if (resolved > static_cast<int>(FontStretch::UltraExpanded)) {
      resolved = static_cast<int>(FontStretch::UltraExpanded);
    }
    fontStretch.value = resolved;
  }
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
