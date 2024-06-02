#include "src/svg/xml/attribute_parser.h"

#include <entt/entt.hpp>
#include <string_view>

#include "src/base/parser/length_parser.h"
#include "src/base/parser/number_parser.h"
#include "src/base/parser/parse_error.h"
#include "src/svg/all_svg_elements.h"  // IWYU pragma: keep
#include "src/svg/components/filter/filter_units.h"
#include "src/svg/parser/number2d_parser.h"
#include "src/svg/parser/points_list_parser.h"  // IWYU pragma: keep, used by PointsListParser
#include "src/svg/parser/preserve_aspect_ratio_parser.h"
#include "src/svg/parser/viewbox_parser.h"
#include "src/svg/svg_filter_element.h"
#include "src/svg/xml/details/xml_parser_context.h"
#include "src/svg/xml/xml_qualified_name.h"

namespace donner::svg {

namespace {

template <typename T>
concept HasPathLength =
    requires(T element, std::optional<double> value) { element.setPathLength(value); };

bool IsAlwaysGenericAttribute(const XMLQualifiedNameRef& name) {
  return name == XMLQualifiedNameRef("id") || name == XMLQualifiedNameRef("class") ||
         name == XMLQualifiedNameRef("style");
}

static std::optional<double> ParseNumberNoSuffix(std::string_view str) {
  const auto maybeResult = NumberParser::Parse(str);
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

static std::optional<Lengthd> ParseLengthAttribute(XMLParserContext& context,
                                                   std::string_view value) {
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
    err.offset = maybeLengthResult.result().consumedChars;
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    return std::nullopt;
  }

  return maybeLengthResult.result().length;
}

static std::optional<float> ParseStopOffset(XMLParserContext& context, std::string_view value) {
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
    err.offset = maybeLengthResult.result().consumedChars;
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

static void ParsePresentationAttribute(XMLParserContext& context, SVGElement& element,
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
        context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
        return;
      }
    }
  }

  element.setAttribute(name, value);
}

static void ParseUnconditionalCommonAttribute(XMLParserContext& context, SVGElement& element,
                                              const XMLQualifiedNameRef& name,
                                              std::string_view value) {
  // TODO: Support namespaces on presentation attributes.
  // For now, only parse attributes that are not in a namespace as presentation attributes.
  if (IsAlwaysGenericAttribute(name)) {
    element.setAttribute(name, value);
  } else {
    ParsePresentationAttribute(context, element, name, value);
  }
}

template <typename T>
std::optional<ParseError> ParseCommonAttribute(XMLParserContext& context, T& element,
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

  ParseUnconditionalCommonAttribute(context, element, name, value);
  return std::nullopt;
}

std::optional<ParseError> ParseGradientCommonAttribute(XMLParserContext& context,
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
std::optional<ParseError> ParseAttribute(XMLParserContext& context, T element,
                                         const XMLQualifiedNameRef& name, std::string_view value) {
  return ParseCommonAttribute(context, element, name, value);
}

template <>
std::optional<ParseError> ParseAttribute<SVGFilterElement>(XMLParserContext& context,
                                                           SVGFilterElement element,
                                                           const XMLQualifiedNameRef& name,
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
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGFEGaussianBlurElement>(XMLParserContext& context,
                                                                   SVGFEGaussianBlurElement element,
                                                                   const XMLQualifiedNameRef& name,
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
std::optional<ParseError> ParseAttribute<SVGLineElement>(XMLParserContext& context,
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
std::optional<ParseError> ParseAttribute<SVGLinearGradientElement>(XMLParserContext& context,
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
    return ParseGradientCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGPatternElement>(XMLParserContext& context,
                                                            SVGPatternElement element,
                                                            const XMLQualifiedNameRef& name,
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
  } else if (name == XMLQualifiedNameRef("viewBox")) {
    auto maybeViewbox = ViewboxParser::Parse(value);
    if (maybeViewbox.hasError()) {
      context.addSubparserWarning(std::move(maybeViewbox.error()), context.parserOriginFrom(value));
    } else {
      element.setViewbox(maybeViewbox.result());
    }
  } else if (name == XMLQualifiedNameRef("preserveAspectRatio")) {
    auto maybeAspectRatio = PreserveAspectRatioParser::Parse(value);
    if (maybeAspectRatio.hasError()) {
      context.addSubparserWarning(std::move(maybeAspectRatio.error()),
                                  context.parserOriginFrom(value));
    } else {
      element.setPreserveAspectRatio(maybeAspectRatio.result());
    }
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
    element.setHref(RcString(value));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGPolygonElement>(XMLParserContext& context,
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
std::optional<ParseError> ParseAttribute<SVGPolylineElement>(XMLParserContext& context,
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
std::optional<ParseError> ParseAttribute<SVGRadialGradientElement>(XMLParserContext& context,
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
    return ParseGradientCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGSVGElement>(XMLParserContext& context,
                                                        SVGSVGElement element,
                                                        const XMLQualifiedNameRef& name,
                                                        std::string_view value) {
  if (name == XMLQualifiedNameRef("viewBox")) {
    auto maybeViewbox = ViewboxParser::Parse(value);
    if (maybeViewbox.hasError()) {
      context.addSubparserWarning(std::move(maybeViewbox.error()), context.parserOriginFrom(value));
    } else {
      element.setViewbox(maybeViewbox.result());
    }
  } else if (name == XMLQualifiedNameRef("preserveAspectRatio")) {
    auto maybeAspectRatio = PreserveAspectRatioParser::Parse(value);
    if (maybeAspectRatio.hasError()) {
      context.addSubparserWarning(std::move(maybeAspectRatio.error()),
                                  context.parserOriginFrom(value));
    } else {
      element.setPreserveAspectRatio(maybeAspectRatio.result());
    }
  } else if (name.namespacePrefix == "xmlns" || name == XMLQualifiedNameRef("xmlns")) {
    // This was already parsed by @ref ParseXmlNsAttribute.
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseError> ParseAttribute<SVGStopElement>(XMLParserContext& context,
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
std::optional<ParseError> ParseAttribute<SVGStyleElement>(XMLParserContext& context,
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
std::optional<ParseError> ParseAttribute<SVGUseElement>(XMLParserContext& context,
                                                        SVGUseElement element,
                                                        const XMLQualifiedNameRef& name,
                                                        std::string_view value) {
  if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(RcString(value));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <size_t I = 0, typename... Types>
std::optional<ParseError> ParseAttributesForElement(XMLParserContext& context, SVGElement& element,
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

std::optional<ParseError> AttributeParser::ParseAndSetAttribute(XMLParserContext& context,
                                                                SVGElement& element,
                                                                const XMLQualifiedNameRef& name,
                                                                std::string_view value) noexcept {
  return ParseAttributesForElement(context, element, name, value, AllSVGElements());
}

}  // namespace donner::svg
