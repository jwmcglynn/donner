#include "donner/svg/parser/AttributeParser.h"

#include <cctype>
#include <entt/entt.hpp>
#include <string_view>

#include "donner/base/MathUtils.h"
#include "donner/base/ParseError.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/css/parser/ValueParser.h"
#include "donner/svg/AllSVGElements.h"  // IWYU pragma: keep
#include "donner/svg/SVGClipPathElement.h"
#include "donner/svg/SVGFilterElement.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/filter/FilterUnits.h"
#include "donner/svg/core/MaskUnits.h"
#include "donner/svg/parser/AngleParser.h"
#include "donner/svg/parser/ListParser.h"
#include "donner/svg/parser/Number2dParser.h"
#include "donner/svg/parser/PointsListParser.h"  // IWYU pragma: keep, used by PointsListParser
#include "donner/svg/parser/PreserveAspectRatioParser.h"
#include "donner/svg/parser/ViewBoxParser.h"
#include "donner/svg/parser/details/SVGParserContext.h"

namespace donner::svg::parser {

using xml::XMLQualifiedNameRef;

namespace {

template <typename T>
concept HasPathLength =
    requires(T element, std::optional<double> value) { element.setPathLength(value); };

bool IsAlwaysGenericAttribute(const XMLQualifiedNameRef& name) {
  return name == XMLQualifiedNameRef("id") || name == XMLQualifiedNameRef("class") ||
         name == XMLQualifiedNameRef("style");
}

std::optional<double> ParseNumberNoSuffix(std::string_view str) {
  const auto maybeResult = donner::parser::NumberParser::Parse(str);
  if (maybeResult.hasResult()) {
    if (maybeResult.result().consumedChars != str.size()) {
      // We had extra characters, treat as invalid.
      return std::nullopt;
    }

    return maybeResult.result().number;
  } else {
    return std::nullopt;
  }
}

std::optional<Lengthd> ParseLengthAttribute(SVGParserContext& context, std::string_view value) {
  using donner::parser::LengthParser;

  LengthParser::Options options;
  options.unitOptional = true;

  auto maybeLengthResult = LengthParser::Parse(value, options);
  if (maybeLengthResult.hasError()) {
    context.addSubparserWarning(std::move(maybeLengthResult.error()),
                                context.parserOriginFrom(value));
    return std::nullopt;
  }

  if (maybeLengthResult.result().consumedChars != value.size()) {
    ParseError err;
    err.reason = "Unexpected data at end of attribute";
    err.location = FileOffset::Offset(maybeLengthResult.result().consumedChars);
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    return std::nullopt;
  }

  return maybeLengthResult.result().length;
}

std::optional<float> ParseStopOffset(SVGParserContext& context, std::string_view value) {
  using donner::parser::LengthParser;

  // Since we want to both parse a number or percent, use a LengthParser and then filter the allowed
  // suffixes.
  LengthParser::Options options;
  options.unitOptional = true;
  options.limitUnitToPercentage = true;

  auto maybeLengthResult = LengthParser::Parse(value, options);
  if (maybeLengthResult.hasError()) {
    context.addSubparserWarning(std::move(maybeLengthResult.error()),
                                context.parserOriginFrom(value));
    return std::nullopt;
  }

  if (maybeLengthResult.result().consumedChars != value.size()) {
    ParseError err;
    err.reason = "Unexpected data at end of attribute";
    err.location = FileOffset::Offset(maybeLengthResult.result().consumedChars);
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    return std::nullopt;
  }

  // Convert to the [0, 1] range.
  const Lengthd length = maybeLengthResult.result().length;
  if (length.unit == Lengthd::Unit::Percent) {
    return Clamp(NarrowToFloat(length.value / 100.0), 0.0f, 1.0f);
  } else {
    return Clamp(NarrowToFloat(length.value), 0.0f, 1.0f);
  }
}

/**
 * Parses `x`, `y`, `width`, and `height` values for elements that have them. Returns true if the
 * attribute was found, so that the caller may use that information to skip other attribute parsing.
 *
 * @tparam T Element type, should should have setter methods for `setX`, `setY`, `setWidth`, and
 * `setHeight`.
 * @param context The XML parser context.
 * @param element The element to set the values on.
 * @param name The name of the attribute.
 * @param value The value of the attribute.
 * @return True if the attribute was `x`, `y`, `width`, or `height`.
 */
template <typename T>
bool ParseXYWidthHeight(SVGParserContext& context, T element, const XMLQualifiedNameRef& name,
                        std::string_view value) {
  if (name == XMLQualifiedNameRef("x")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setX(length.value());
    }
  } else if (name == XMLQualifiedNameRef("y")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setY(length.value());
    }
  } else if (name == XMLQualifiedNameRef("width")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setWidth(length.value());
    }
  } else if (name == XMLQualifiedNameRef("height")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setHeight(length.value());
    }
  } else {
    return false;
  }

  return true;
}

/**
 * Parse the `in` attribute value into a \ref FilterInput.
 *
 * Standard keywords: SourceGraphic, SourceAlpha, FillPaint, StrokePaint.
 * Any other non-empty string is treated as a named result reference.
 *
 * @param value Attribute value.
 * @return The parsed FilterInput.
 */
components::FilterInput ParseFilterInput(std::string_view value) {
  using components::FilterInput;
  using components::FilterStandardInput;

  if (value == "SourceGraphic") {
    return FilterInput{FilterStandardInput::SourceGraphic};
  }
  if (value == "SourceAlpha") {
    return FilterInput{FilterStandardInput::SourceAlpha};
  }
  if (value == "FillPaint") {
    return FilterInput{FilterStandardInput::FillPaint};
  }
  if (value == "StrokePaint") {
    return FilterInput{FilterStandardInput::StrokePaint};
  }
  return FilterInput{FilterInput::Named{RcString(value)}};
}

/**
 * Parses the `in` and `result` attributes common to all filter primitives.
 * Returns true if the attribute was handled.
 *
 * @tparam T Element type deriving from SVGFilterPrimitiveStandardAttributes.
 * @param element The element to set the values on.
 * @param name The attribute name.
 * @param value The attribute value.
 * @return True if the attribute was `in` or `result`.
 */
template <typename T>
bool ParseFilterPrimitiveAttributes(T element, const XMLQualifiedNameRef& name,
                                    std::string_view value) {
  if (name == XMLQualifiedNameRef("in")) {
    element.entityHandle().template get<components::FilterPrimitiveComponent>().in =
        ParseFilterInput(value);
    return true;
  }
  if (name == XMLQualifiedNameRef("in2")) {
    element.entityHandle().template get<components::FilterPrimitiveComponent>().in2 =
        ParseFilterInput(value);
    return true;
  }
  if (name == XMLQualifiedNameRef("result")) {
    element.setResult(value);
    return true;
  }
  return false;
}

/**
 * Parses `viewBox` and `preserveAspectRatio` values for elements that have them. Returns true if
 * the attribute was found, so that the caller may use that information to skip other attribute
 * parsing.
 *
 * @tparam T Element type, should should have setter methods for `setViewBox` and
 * `setPreserveAspectRatio`.
 * @param context The XML parser context.
 * @param element The element to set the values on.
 * @param name The name of the attribute.
 * @param value The value of the attribute.
 * @return True if the attribute was `viewBox` or `preserveAspectRatio`.
 */
template <typename T>
bool ParseViewBoxPreserveAspectRatio(SVGParserContext& context, T element,
                                     const XMLQualifiedNameRef& name, std::string_view value) {
  if (name == XMLQualifiedNameRef("viewBox")) {
    auto maybeViewBox = ViewBoxParser::Parse(value);
    if (maybeViewBox.hasError()) {
      context.addSubparserWarning(std::move(maybeViewBox.error()), context.parserOriginFrom(value));
    } else {
      element.setViewBox(maybeViewBox.result());
    }
  } else if (name == XMLQualifiedNameRef("preserveAspectRatio")) {
    auto maybeAspectRatio = PreserveAspectRatioParser::Parse(value);
    if (maybeAspectRatio.hasError()) {
      context.addSubparserWarning(std::move(maybeAspectRatio.error()),
                                  context.parserOriginFrom(value));
    } else {
      element.setPreserveAspectRatio(maybeAspectRatio.result());
    }
  } else {
    return false;
  }

  return true;
}

std::optional<double> ParseAngleAttribute(SVGParserContext& context, std::string_view value) {
  // Use the ValueParser to parse the string into ComponentValues
  auto componentValues = css::parser::ValueParser::Parse(value);

  if (componentValues.empty()) {
    ParseError err;
    err.reason = "Invalid angle value '" + std::string(value) + "'";
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    return std::nullopt;
  }

  // Use the first ComponentValue to parse the angle
  const css::ComponentValue& componentValue = componentValues.front();

  // Use ParseAngle with AllowBareZero to accept bare numbers as degrees
  auto parseResult =
      svg::parser::ParseAngle(componentValue, AngleParseOptions::AllowNumbersInDegrees);

  if (parseResult.hasError()) {
    context.addSubparserWarning(std::move(parseResult.error()), context.parserOriginFrom(value));
    return std::nullopt;
  }

  // Check if there are extra tokens after the angle
  if (componentValues.size() > 1) {
    ParseError err;
    err.reason = "Unexpected data after angle value in '" + std::string(value) + "'";
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
  }

  return parseResult.result();
}

void ParsePresentationAttribute(SVGParserContext& context, SVGElement& element,
                                const XMLQualifiedNameRef& name, std::string_view value) {
  // TODO: Move this logic into SVGElement::setAttribute.

  // TODO: Detect the SVG namespace here and only parse elements in that namespace.
  if (name.namespacePrefix.empty()) {
    // For now, we only parse attributes that are not in a namespace.
    auto result = element.trySetPresentationAttribute(name.name, value);
    if (result.hasError()) {
      context.addSubparserWarning(std::move(result.error()), context.parserOriginFrom(value));
    } else if (!result.result()) {
      if (context.options().disableUserAttributes) {
        ParseError err;
        err.reason = "Unknown attribute '" + name.toString() + "' (disableUserAttributes: true)";
        if (auto maybeLocation = context.getAttributeLocation(element, name)) {
          err.location = maybeLocation->start;
        }
        context.addWarning(std::move(err));
        element.removeAttribute(name);
        return;
      }
    }
  }

  element.setAttribute(name, value);
}

void ParseUnconditionalCommonAttribute(SVGParserContext& context, SVGElement& element,
                                       const XMLQualifiedNameRef& name, std::string_view value) {
  // TODO: Support namespaces on presentation attributes.
  // For now, only parse attributes that are not in a namespace as presentation attributes.
  if (IsAlwaysGenericAttribute(name)) {
    element.setAttribute(name, value);
  } else {
    ParsePresentationAttribute(context, element, name, value);
  }
}

template <typename T>
std::optional<ParseError> ParseCommonAttribute(SVGParserContext& context, T& element,
                                               const XMLQualifiedNameRef& name,
                                               std::string_view value) {
  if constexpr (HasPathLength<T>) {
    if (name == XMLQualifiedNameRef("pathLength")) {
      // Parse the attribute as a number, and if it resolves set the length.
      if (auto maybeNumber = ParseNumberNoSuffix(value)) {
        element.setPathLength(maybeNumber.value());
      } else {
        ParseError err;
        err.reason = "Invalid pathLength value '" + std::string(value) + "'";
        context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
      }

      return std::nullopt;
    }
  }

  parser::ParseUnconditionalCommonAttribute(context, element, name, value);
  return std::nullopt;
}

std::optional<ParseError> ParseGradientCommonAttribute(SVGParserContext& context,
                                                       SVGGradientElement& element,
                                                       const XMLQualifiedNameRef& name,
                                                       std::string_view value) {
  if (name == XMLQualifiedNameRef("gradientUnits")) {
    if (value == "userSpaceOnUse") {
      element.setGradientUnits(GradientUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setGradientUnits(GradientUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid gradientUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("spreadMethod")) {
    if (value == "pad") {
      element.setSpreadMethod(GradientSpreadMethod::Pad);
    } else if (value == "reflect") {
      element.setSpreadMethod(GradientSpreadMethod::Reflect);
    } else if (value == "repeat") {
      element.setSpreadMethod(GradientSpreadMethod::Repeat);
    } else {
      ParseError err;
      err.reason = "Invalid spreadMethod value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(RcString(value));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <typename T>
std::optional<ParseError> ParseAttribute(SVGParserContext& context, T element,
                                         const XMLQualifiedNameRef& name, std::string_view value) {
  return ParseCommonAttribute(context, element, name, value);
}

template <>
std::optional<ParseError> ParseAttribute<SVGClipPathElement>(SVGParserContext& context,
                                                             SVGClipPathElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  if (name == XMLQualifiedNameRef("clipPathUnits")) {
    if (value == "userSpaceOnUse") {
      element.setClipPathUnits(ClipPathUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setClipPathUnits(ClipPathUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid clipPathUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGMaskElement>(SVGParserContext& context,
                                                         SVGMaskElement element,
                                                         const XMLQualifiedNameRef& name,
                                                         std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("maskUnits")) {
    if (value == "userSpaceOnUse") {
      element.setMaskUnits(MaskUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setMaskUnits(MaskUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid maskUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("maskContentUnits")) {
    if (value == "userSpaceOnUse") {
      element.setMaskContentUnits(MaskContentUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setMaskContentUnits(MaskContentUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid maskContentUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFilterElement>(SVGParserContext& context,
                                                           SVGFilterElement element,
                                                           const XMLQualifiedNameRef& name,
                                                           std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("filterUnits")) {
    if (value == "userSpaceOnUse") {
      element.setFilterUnits(FilterUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setFilterUnits(FilterUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid filterUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("primitiveUnits")) {
    if (value == "userSpaceOnUse") {
      element.setPrimitiveUnits(PrimitiveUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setPrimitiveUnits(PrimitiveUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid primitiveUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("color-interpolation-filters")) {
    auto& comp = element.entityHandle().get<components::FilterComponent>();
    if (value == "sRGB") {
      comp.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
    } else if (value == "linearRGB") {
      comp.colorInterpolationFilters = ColorInterpolationFilters::LinearRGB;
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEGaussianBlurElement>(SVGParserContext& context,
                                                                   SVGFEGaussianBlurElement element,
                                                                   const XMLQualifiedNameRef& name,
                                                                   std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    // Handled by filter primitive standard attributes.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("stdDeviation")) {
    const auto maybeNumber2d = Number2dParser::Parse(value);
    if (maybeNumber2d.hasResult()) {
      const Number2dParser::Result number2d = maybeNumber2d.result();
      // TODO: Does this handle whitespace at the end of the string?
      if (number2d.consumedChars == value.size()) {
        element.setStdDeviation(number2d.numberX, number2d.numberY);
      } else {
        ParseError err;
        err.reason = "Unexpected additional data in stdDeviation, '" + std::string(value) + "'";
        context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
      }
    } else {
      ParseError err;
      err.reason = "Invalid stdDeviation value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEBlendElement>(SVGParserContext& context,
                                                             SVGFEBlendElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("mode")) {
    auto& comp = element.entityHandle().get<components::FEBlendComponent>();
    if (value == "normal") {
      comp.mode = components::FEBlendComponent::Mode::Normal;
    } else if (value == "multiply") {
      comp.mode = components::FEBlendComponent::Mode::Multiply;
    } else if (value == "screen") {
      comp.mode = components::FEBlendComponent::Mode::Screen;
    } else if (value == "darken") {
      comp.mode = components::FEBlendComponent::Mode::Darken;
    } else if (value == "lighten") {
      comp.mode = components::FEBlendComponent::Mode::Lighten;
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEComponentTransferElement>(
    SVGParserContext& context, SVGFEComponentTransferElement element,
    const XMLQualifiedNameRef& name, std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

namespace {

/// Parse the shared attributes for feFuncR/G/B/A elements.
void ParseFuncAttributes(components::FEFuncComponent& comp, const XMLQualifiedNameRef& name,
                         std::string_view value) {
  if (name == XMLQualifiedNameRef("type")) {
    if (value == "identity") {
      comp.type = components::FEFuncComponent::FuncType::Identity;
    } else if (value == "table") {
      comp.type = components::FEFuncComponent::FuncType::Table;
    } else if (value == "discrete") {
      comp.type = components::FEFuncComponent::FuncType::Discrete;
    } else if (value == "linear") {
      comp.type = components::FEFuncComponent::FuncType::Linear;
    } else if (value == "gamma") {
      comp.type = components::FEFuncComponent::FuncType::Gamma;
    }
  } else if (name == XMLQualifiedNameRef("tableValues")) {
    comp.tableValues.clear();
    std::string_view remaining = value;
    while (!remaining.empty()) {
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',' ||
                                     remaining.front() == '\t' || remaining.front() == '\n' ||
                                     remaining.front() == '\r')) {
        remaining.remove_prefix(1);
      }
      if (remaining.empty()) {
        break;
      }
      const auto maybeNumber = donner::parser::NumberParser::Parse(remaining);
      if (maybeNumber.hasResult()) {
        comp.tableValues.push_back(maybeNumber.result().number);
        remaining.remove_prefix(maybeNumber.result().consumedChars);
      } else {
        break;
      }
    }
  } else if (name == XMLQualifiedNameRef("slope")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      comp.slope = *n;
    }
  } else if (name == XMLQualifiedNameRef("intercept")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      comp.intercept = *n;
    }
  } else if (name == XMLQualifiedNameRef("amplitude")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      comp.amplitude = *n;
    }
  } else if (name == XMLQualifiedNameRef("exponent")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      comp.exponent = *n;
    }
  } else if (name == XMLQualifiedNameRef("offset")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      comp.offset = *n;
    }
  }
}

}  // namespace

template <>
std::optional<ParseError> ParseAttribute<SVGFEFuncRElement>(SVGParserContext& context,
                                                             SVGFEFuncRElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  ParseFuncAttributes(comp, name, value);
  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEFuncGElement>(SVGParserContext& context,
                                                             SVGFEFuncGElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  ParseFuncAttributes(comp, name, value);
  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEFuncBElement>(SVGParserContext& context,
                                                             SVGFEFuncBElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  ParseFuncAttributes(comp, name, value);
  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEFuncAElement>(SVGParserContext& context,
                                                             SVGFEFuncAElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  ParseFuncAttributes(comp, name, value);
  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEColorMatrixElement>(
    SVGParserContext& context, SVGFEColorMatrixElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("type")) {
    auto& comp = element.entityHandle().get<components::FEColorMatrixComponent>();
    if (value == "matrix") {
      comp.type = components::FEColorMatrixComponent::Type::Matrix;
    } else if (value == "saturate") {
      comp.type = components::FEColorMatrixComponent::Type::Saturate;
    } else if (value == "hueRotate") {
      comp.type = components::FEColorMatrixComponent::Type::HueRotate;
    } else if (value == "luminanceToAlpha") {
      comp.type = components::FEColorMatrixComponent::Type::LuminanceToAlpha;
    }
  } else if (name == XMLQualifiedNameRef("values")) {
    auto& comp = element.entityHandle().get<components::FEColorMatrixComponent>();
    comp.values.clear();
    // Parse space/comma-separated list of numbers.
    std::string_view remaining = value;
    while (!remaining.empty()) {
      // Skip whitespace and commas.
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',' ||
                                     remaining.front() == '\t' || remaining.front() == '\n' ||
                                     remaining.front() == '\r')) {
        remaining.remove_prefix(1);
      }
      if (remaining.empty()) {
        break;
      }
      const auto maybeNumber = donner::parser::NumberParser::Parse(remaining);
      if (maybeNumber.hasResult()) {
        comp.values.push_back(maybeNumber.result().number);
        remaining.remove_prefix(maybeNumber.result().consumedChars);
      } else {
        break;
      }
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFECompositeElement>(SVGParserContext& context,
                                                                 SVGFECompositeElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("operator")) {
    auto& comp = element.entityHandle().get<components::FECompositeComponent>();
    if (value == "over") {
      comp.op = components::FECompositeComponent::Operator::Over;
    } else if (value == "in") {
      comp.op = components::FECompositeComponent::Operator::In;
    } else if (value == "out") {
      comp.op = components::FECompositeComponent::Operator::Out;
    } else if (value == "atop") {
      comp.op = components::FECompositeComponent::Operator::Atop;
    } else if (value == "xor") {
      comp.op = components::FECompositeComponent::Operator::Xor;
    } else if (value == "lighter") {
      comp.op = components::FECompositeComponent::Operator::Lighter;
    } else if (value == "arithmetic") {
      comp.op = components::FECompositeComponent::Operator::Arithmetic;
    }
  } else if (name == XMLQualifiedNameRef("k1")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      element.entityHandle().get<components::FECompositeComponent>().k1 = *n;
    }
  } else if (name == XMLQualifiedNameRef("k2")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      element.entityHandle().get<components::FECompositeComponent>().k2 = *n;
    }
  } else if (name == XMLQualifiedNameRef("k3")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      element.entityHandle().get<components::FECompositeComponent>().k3 = *n;
    }
  } else if (name == XMLQualifiedNameRef("k4")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      element.entityHandle().get<components::FECompositeComponent>().k4 = *n;
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEDropShadowElement>(
    SVGParserContext& context, SVGFEDropShadowElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  }

  auto& comp = element.entityHandle().get<components::FEDropShadowComponent>();
  if (name == XMLQualifiedNameRef("dx")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      comp.dx = *n;
    }
  } else if (name == XMLQualifiedNameRef("dy")) {
    if (auto n = ParseNumberNoSuffix(value)) {
      comp.dy = *n;
    }
  } else if (name == XMLQualifiedNameRef("stdDeviation")) {
    // Parse one or two numbers: "sigmaX" or "sigmaX sigmaY".
    const auto firstNumber = donner::parser::NumberParser::Parse(value);
    if (firstNumber.hasResult()) {
      comp.stdDeviationX = firstNumber.result().number;
      comp.stdDeviationY = firstNumber.result().number;
      std::string_view remaining = value.substr(firstNumber.result().consumedChars);
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',')) {
        remaining.remove_prefix(1);
      }
      if (!remaining.empty()) {
        const auto secondNumber = donner::parser::NumberParser::Parse(remaining);
        if (secondNumber.hasResult()) {
          comp.stdDeviationY = secondNumber.result().number;
        }
      }
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEFloodElement>(SVGParserContext& context,
                                                             SVGFEFloodElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else {
    // flood-color and flood-opacity are presentation attributes handled by the CSS property system.
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEMorphologyElement>(
    SVGParserContext& context, SVGFEMorphologyElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("operator")) {
    auto& comp = element.entityHandle().get<components::FEMorphologyComponent>();
    if (value == "erode") {
      comp.op = components::FEMorphologyComponent::Operator::Erode;
    } else if (value == "dilate") {
      comp.op = components::FEMorphologyComponent::Operator::Dilate;
    }
  } else if (name == XMLQualifiedNameRef("radius")) {
    auto& comp = element.entityHandle().get<components::FEMorphologyComponent>();
    // Parse one or two numbers: "r" or "rX rY". Extra values invalidate the attribute.
    const auto firstNumber = donner::parser::NumberParser::Parse(value);
    if (firstNumber.hasResult()) {
      double rx = firstNumber.result().number;
      double ry = rx;
      bool valid = true;
      std::string_view remaining = value.substr(firstNumber.result().consumedChars);
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',')) {
        remaining.remove_prefix(1);
      }
      if (!remaining.empty()) {
        const auto secondNumber = donner::parser::NumberParser::Parse(remaining);
        if (secondNumber.hasResult()) {
          ry = secondNumber.result().number;
          // Check for trailing data (too many values).
          remaining = remaining.substr(secondNumber.result().consumedChars);
          while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',')) {
            remaining.remove_prefix(1);
          }
          if (!remaining.empty()) {
            valid = false;
          }
        }
      }
      if (valid) {
        comp.radiusX = rx;
        comp.radiusY = ry;
      }
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEConvolveMatrixElement>(
    SVGParserContext& context, SVGFEConvolveMatrixElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("order")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    const auto firstNumber = donner::parser::NumberParser::Parse(value);
    if (firstNumber.hasResult()) {
      int orderX = static_cast<int>(firstNumber.result().number);
      int orderY = orderX;
      std::string_view remaining = value.substr(firstNumber.result().consumedChars);
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',')) {
        remaining.remove_prefix(1);
      }
      if (!remaining.empty()) {
        const auto secondNumber = donner::parser::NumberParser::Parse(remaining);
        if (secondNumber.hasResult()) {
          orderY = static_cast<int>(secondNumber.result().number);
        }
      }
      comp.orderX = orderX;
      comp.orderY = orderY;
    }
  } else if (name == XMLQualifiedNameRef("kernelMatrix")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    comp.kernelMatrix.clear();
    std::string_view remaining = value;
    while (!remaining.empty()) {
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',' ||
                                    remaining.front() == '\n' || remaining.front() == '\r' ||
                                    remaining.front() == '\t')) {
        remaining.remove_prefix(1);
      }
      if (remaining.empty()) {
        break;
      }
      const auto maybeNumber = donner::parser::NumberParser::Parse(remaining);
      if (maybeNumber.hasResult()) {
        comp.kernelMatrix.push_back(maybeNumber.result().number);
        remaining = remaining.substr(maybeNumber.result().consumedChars);
      } else {
        break;
      }
    }
  } else if (name == XMLQualifiedNameRef("divisor")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.divisor = maybeNumber.result().number;
    }
  } else if (name == XMLQualifiedNameRef("bias")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.bias = maybeNumber.result().number;
    }
  } else if (name == XMLQualifiedNameRef("targetX")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.targetX = static_cast<int>(maybeNumber.result().number);
    }
  } else if (name == XMLQualifiedNameRef("targetY")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.targetY = static_cast<int>(maybeNumber.result().number);
    }
  } else if (name == XMLQualifiedNameRef("edgeMode")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    if (value == "duplicate") {
      comp.edgeMode = components::FEConvolveMatrixComponent::EdgeMode::Duplicate;
    } else if (value == "wrap") {
      comp.edgeMode = components::FEConvolveMatrixComponent::EdgeMode::Wrap;
    } else if (value == "none") {
      comp.edgeMode = components::FEConvolveMatrixComponent::EdgeMode::None;
    }
  } else if (name == XMLQualifiedNameRef("preserveAlpha")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    comp.preserveAlpha = (value == "true");
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFETurbulenceElement>(SVGParserContext& context,
                                                                  SVGFETurbulenceElement element,
                                                                  const XMLQualifiedNameRef& name,
                                                                  std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("baseFrequency")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    const auto firstNumber = donner::parser::NumberParser::Parse(value);
    if (firstNumber.hasResult()) {
      comp.baseFrequencyX = firstNumber.result().number;
      comp.baseFrequencyY = comp.baseFrequencyX;
      std::string_view remaining = value.substr(firstNumber.result().consumedChars);
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',')) {
        remaining.remove_prefix(1);
      }
      if (!remaining.empty()) {
        const auto secondNumber = donner::parser::NumberParser::Parse(remaining);
        if (secondNumber.hasResult()) {
          comp.baseFrequencyY = secondNumber.result().number;
        }
      }
    }
  } else if (name == XMLQualifiedNameRef("numOctaves")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      comp.numOctaves = static_cast<int>(*maybeNumber);
    }
  } else if (name == XMLQualifiedNameRef("seed")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      comp.seed = *maybeNumber;
    }
  } else if (name == XMLQualifiedNameRef("type")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    if (value == "fractalNoise") {
      comp.type = components::FETurbulenceComponent::Type::FractalNoise;
    } else if (value == "turbulence") {
      comp.type = components::FETurbulenceComponent::Type::Turbulence;
    }
    // Invalid values: keep default (turbulence).
  } else if (name == XMLQualifiedNameRef("stitchTiles")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    comp.stitchTiles = (value == "stitch");
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFETileElement>(SVGParserContext& context,
                                                            SVGFETileElement element,
                                                            const XMLQualifiedNameRef& name,
                                                            std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEOffsetElement>(SVGParserContext& context,
                                                              SVGFEOffsetElement element,
                                                              const XMLQualifiedNameRef& name,
                                                              std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("dx")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setOffset(*maybeNumber, element.dy());
    }
  } else if (name == XMLQualifiedNameRef("dy")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setOffset(element.dx(), *maybeNumber);
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEMergeElement>(SVGParserContext& context,
                                                             SVGFEMergeElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEMergeNodeElement>(
    SVGParserContext& context, SVGFEMergeNodeElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (name == XMLQualifiedNameRef("in")) {
    element.entityHandle().get<components::FEMergeNodeComponent>().in = ParseFilterInput(value);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGImageElement>(SVGParserContext& context,
                                                          SVGImageElement element,
                                                          const XMLQualifiedNameRef& name,
                                                          std::string_view value) {
  if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(value);
  } else if (name == XMLQualifiedNameRef("preserveAspectRatio")) {
    auto maybeAspectRatio = PreserveAspectRatioParser::Parse(value);
    if (maybeAspectRatio.hasError()) {
      context.addSubparserWarning(std::move(maybeAspectRatio.error()),
                                  context.parserOriginFrom(value));
    } else {
      element.setPreserveAspectRatio(maybeAspectRatio.result());
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGLineElement>(SVGParserContext& context,
                                                         SVGLineElement element,
                                                         const XMLQualifiedNameRef& name,
                                                         std::string_view value) {
  if (name == XMLQualifiedNameRef("x1")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setX1(length.value());
    }
  } else if (name == XMLQualifiedNameRef("y1")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setY1(length.value());
    }
  } else if (name == XMLQualifiedNameRef("x2")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setX2(length.value());
    }
  } else if (name == XMLQualifiedNameRef("y2")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setY2(length.value());
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGLinearGradientElement>(SVGParserContext& context,
                                                                   SVGLinearGradientElement element,
                                                                   const XMLQualifiedNameRef& name,
                                                                   std::string_view value) {
  if (name == XMLQualifiedNameRef("x1")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setX1(length.value());
    }
  } else if (name == XMLQualifiedNameRef("y1")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setY1(length.value());
    }
  } else if (name == XMLQualifiedNameRef("x2")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setX2(length.value());
    }
  } else if (name == XMLQualifiedNameRef("y2")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setY2(length.value());
    }
  } else {
    return parser::ParseGradientCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGPatternElement>(SVGParserContext& context,
                                                            SVGPatternElement element,
                                                            const XMLQualifiedNameRef& name,
                                                            std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (ParseViewBoxPreserveAspectRatio(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("patternUnits")) {
    if (value == "userSpaceOnUse") {
      element.setPatternUnits(PatternUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setPatternUnits(PatternUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid patternUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("patternContentUnits")) {
    if (value == "userSpaceOnUse") {
      element.setPatternContentUnits(PatternContentUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setPatternContentUnits(PatternContentUnits::ObjectBoundingBox);
    } else {
      ParseError err;
      err.reason = "Invalid patternUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(RcStringOrRef(RcString(value)));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGPolygonElement>(SVGParserContext& context,
                                                            SVGPolygonElement element,
                                                            const XMLQualifiedNameRef& name,
                                                            std::string_view value) {
  if (name == XMLQualifiedNameRef("points")) {
    auto pointsResult = PointsListParser::Parse(value);

    // Note that errors here are non-fatal, since valid points are also returned.
    if (pointsResult.hasError()) {
      context.addSubparserWarning(std::move(pointsResult.error()), context.parserOriginFrom(value));
    }

    if (pointsResult.hasResult()) {
      element.setPoints(std::move(pointsResult.result()));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGPolylineElement>(SVGParserContext& context,
                                                             SVGPolylineElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  if (name == XMLQualifiedNameRef("points")) {
    auto pointsResult = PointsListParser::Parse(value);

    // Note that errors here are non-fatal, since valid points are also returned.
    if (pointsResult.hasError()) {
      context.addSubparserWarning(std::move(pointsResult.error()), context.parserOriginFrom(value));
    }

    if (pointsResult.hasResult()) {
      element.setPoints(std::move(pointsResult.result()));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGRadialGradientElement>(SVGParserContext& context,
                                                                   SVGRadialGradientElement element,
                                                                   const XMLQualifiedNameRef& name,
                                                                   std::string_view value) {
  if (name == XMLQualifiedNameRef("cx")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setCx(length.value());
    }
  } else if (name == XMLQualifiedNameRef("cy")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setCy(length.value());
    }
  } else if (name == XMLQualifiedNameRef("r")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setR(length.value());
    }
  } else if (name == XMLQualifiedNameRef("fx")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setFx(length.value());
    }
  } else if (name == XMLQualifiedNameRef("fy")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setFy(length.value());
    }
  } else if (name == XMLQualifiedNameRef("fr")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setFr(length.value());
    }
  } else {
    return parser::ParseGradientCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGSVGElement>(SVGParserContext& context,
                                                        SVGSVGElement element,
                                                        const XMLQualifiedNameRef& name,
                                                        std::string_view value) {
  if (ParseViewBoxPreserveAspectRatio(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name.namespacePrefix == "xmlns" || name == XMLQualifiedNameRef("xmlns")) {
    // This was already parsed by @ref ParseXmlNsAttribute.
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGStopElement>(SVGParserContext& context,
                                                         SVGStopElement element,
                                                         const XMLQualifiedNameRef& name,
                                                         std::string_view value) {
  if (name == XMLQualifiedNameRef("offset")) {
    if (auto maybeOffset = ParseStopOffset(context, value)) {
      element.setOffset(maybeOffset.value());
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGStyleElement>(SVGParserContext& context,
                                                          SVGStyleElement element,
                                                          const XMLQualifiedNameRef& name,
                                                          std::string_view value) {
  if (name == XMLQualifiedNameRef("type")) {
    if (value.empty() ||
        StringUtils::Equals<StringComparison::IgnoreCase>(value, std::string_view("text/css"))) {
      // The value is valid.
    } else {
      ParseError err;
      err.reason = "Invalid <style> element type '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }

    element.setType(RcString(value));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGUseElement>(SVGParserContext& context,
                                                        SVGUseElement element,
                                                        const XMLQualifiedNameRef& name,
                                                        std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(RcString(value));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGMarkerElement>(SVGParserContext& context,
                                                           SVGMarkerElement element,
                                                           const XMLQualifiedNameRef& name,
                                                           std::string_view value) {
  if (ParseViewBoxPreserveAspectRatio(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("markerWidth")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setMarkerWidth(maybeNumber.value());
    } else {
      ParseError err;
      err.reason = "Invalid markerWidth value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("markerHeight")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setMarkerHeight(maybeNumber.value());
    } else {
      ParseError err;
      err.reason = "Invalid markerHeight value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("refX")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setRefX(maybeNumber.value());
    } else {
      ParseError err;
      err.reason = "Invalid refX value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("refY")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setRefY(maybeNumber.value());
    } else {
      ParseError err;
      err.reason = "Invalid refY value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("orient")) {
    if (value == "auto") {
      element.setOrient(MarkerOrient::Auto());
    } else if (value == "auto-start-reverse") {
      element.setOrient(MarkerOrient::AutoStartReverse());
    } else {
      auto maybeAngleRadians = ParseAngleAttribute(context, value);
      if (maybeAngleRadians) {
        element.setOrient(MarkerOrient::AngleRadians(maybeAngleRadians.value()));
      } else {
        // Error already reported in ParseAngleAttribute
      }
    }
  } else if (name == XMLQualifiedNameRef("markerUnits")) {
    if (value == "strokeWidth") {
      element.setMarkerUnits(MarkerUnits::StrokeWidth);
    } else if (value == "userSpaceOnUse") {
      element.setMarkerUnits(MarkerUnits::UserSpaceOnUse);
    } else {
      ParseError err;
      err.reason = "Invalid markerUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGSymbolElement>(SVGParserContext& context,
                                                           SVGSymbolElement element,
                                                           const XMLQualifiedNameRef& name,
                                                           std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (ParseViewBoxPreserveAspectRatio(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("refX")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setRefX(maybeNumber.value());
    } else {
      ParseError err;
      err.reason = "Invalid refX value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("refY")) {
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      element.setRefY(maybeNumber.value());
    } else {
      ParseError err;
      err.reason = "Invalid refY value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

/**
 * Parses attributes for \ref xml_text elements.
 *
 * @tparam SVGTextElement The type of the element to parse.
 * @param context The parser context.
 * @param element The element to parse.
 * @param name The name of the attribute to parse.
 * @param value The value of the attribute to parse.
 */
template <>
std::optional<ParseError> ParseAttribute<SVGTextElement>(SVGParserContext& context,
                                                         SVGTextElement element,
                                                         const XMLQualifiedNameRef& name,
                                                         std::string_view value) {
  if (name == XMLQualifiedNameRef("textLength")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setTextLength(length.value());
    }
  } else if (name == XMLQualifiedNameRef("lengthAdjust")) {
    if (value == "spacing") {
      element.setLengthAdjust(LengthAdjust::Spacing);
    } else if (value == "spacingAndGlyphs") {
      element.setLengthAdjust(LengthAdjust::SpacingAndGlyphs);
    } else {
      ParseError err;
      err.reason = "Invalid lengthAdjust value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("x")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(length.value());
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setXList(std::move(list));
  } else if (name == XMLQualifiedNameRef("y")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(length.value());
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setYList(std::move(list));
  } else if (name == XMLQualifiedNameRef("dx")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(*length);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setDxList(std::move(list));
  } else if (name == XMLQualifiedNameRef("dy")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(*length);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setDyList(std::move(list));
  } else if (name == XMLQualifiedNameRef("rotate")) {
    SmallVector<double, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto angleRad = ParseAngleAttribute(context, token)) {
            list.push_back(*angleRad * MathConstants<double>::kRadToDeg);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setRotateList(std::move(list));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

/**
 * Parses attributes for \ref xml_tspan elements.
 *
 * @tparam SVGTSpanElement The type of the element to parse.
 * @param context The parser context.
 * @param element The element to parse.
 * @param name The name of the attribute to parse.
 * @param value The value of the attribute to parse.
 */
template <>
std::optional<ParseError> ParseAttribute<SVGTSpanElement>(SVGParserContext& context,
                                                          SVGTSpanElement element,
                                                          const XMLQualifiedNameRef& name,
                                                          std::string_view value) {
  if (name == XMLQualifiedNameRef("x")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(*length);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setXList(std::move(list));
  } else if (name == XMLQualifiedNameRef("y")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(*length);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setYList(std::move(list));
  } else if (name == XMLQualifiedNameRef("dx")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(*length);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setDxList(std::move(list));
  } else if (name == XMLQualifiedNameRef("dy")) {
    SmallVector<Lengthd, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto length = ParseLengthAttribute(context, token)) {
            list.push_back(*length);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setDyList(std::move(list));
  } else if (name == XMLQualifiedNameRef("rotate")) {
    SmallVector<double, 1> list;
    if (auto error = parser::ListParser::Parse(value, [&](std::string_view token) {
          if (auto angleRad = ParseAngleAttribute(context, token)) {
            list.push_back(*angleRad * MathConstants<double>::kRadToDeg);
          }
        })) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    }
    element.setRotateList(std::move(list));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <size_t I = 0, typename... Types>
std::optional<ParseError> ParseAttributesForElement(SVGParserContext& context, SVGElement& element,
                                                    const XMLQualifiedNameRef& name,
                                                    std::string_view value,
                                                    entt::type_list<Types...>) {
  if constexpr (I != sizeof...(Types)) {
    using ElementType = typename std::tuple_element<I, std::tuple<Types...>>::type;

    if (element.type() == ElementType::Type) {
      ElementType elementDerived = element.cast<ElementType>();
      return ParseAttribute(context, elementDerived, name, value);
    }

    return ParseAttributesForElement<I + 1>(context, element, name, value,
                                            entt::type_list<Types...>());
  } else {
    return ParseAttribute(context, element, name, value);
  }
}

}  // namespace

std::optional<ParseError> AttributeParser::ParseAndSetAttribute(SVGParserContext& context,
                                                                SVGElement& element,
                                                                const XMLQualifiedNameRef& name,
                                                                std::string_view value) noexcept {
  return ParseAttributesForElement(context, element, name, value, AllSVGElements());
}

}  // namespace donner::svg::parser
