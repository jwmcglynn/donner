#include "donner/svg/parser/AttributeParser.h"

#include <array>
#include <cctype>
#include <cmath>
#include <entt/entity/fwd.hpp>  // entt::type_list, entt::type_list_element
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/MathUtils.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/ParseResult.h"
#include "donner/base/RcString.h"
#include "donner/base/SmallVector.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/css/parser/ValueParser.h"
#include "donner/svg/AllSVGElements.h"  // IWYU pragma: keep
#include "donner/svg/SVGAnimateElement.h"
#include "donner/svg/SVGAnimateTransformElement.h"
#include "donner/svg/SVGClipPathElement.h"
#include "donner/svg/SVGFilterElement.h"
#include "donner/svg/SVGImageElement.h"
#include "donner/svg/SVGMarkerElement.h"
#include "donner/svg/SVGSetElement.h"
#include "donner/svg/components/ConditionalProcessingComponent.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/filter/FilterUnits.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/core/MaskUnits.h"
#include "donner/svg/parser/AngleParser.h"
#include "donner/svg/parser/AnimationAttributeParser.h"
#include "donner/svg/parser/ClockValueParser.h"
#include "donner/svg/parser/ListParser.h"
#include "donner/svg/parser/Number2dParser.h"
#include "donner/svg/parser/PathParser.h"
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

std::optional<int> CheckedInteger(double value, int minimum, int maximum) {
  if (!std::isfinite(value) || value < static_cast<double>(minimum) ||
      value > static_cast<double>(maximum)) {
    return std::nullopt;
  }
  return static_cast<int>(value);
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
    ParseDiagnostic err;
    err.reason = "Unexpected data at end of attribute";
    err.range.start = FileOffset::Offset(maybeLengthResult.result().consumedChars);
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    return std::nullopt;
  }

  return maybeLengthResult.result().length;
}

/**
 * Parses a `<symbol>` `refX`/`refY` attribute, which per SVG2 accepts a `<length>` or one of the
 * axis keywords. Keywords map to a percentage `Lengthd` resolved against the symbol's viewBox at
 * layout time: refX `left`/`center`/`right` → 0%/50%/100%, refY `top`/`center`/`bottom` →
 * 0%/50%/100%.
 *
 * See https://www.w3.org/TR/SVG2/struct.html#SymbolElementRefXAttribute.
 *
 * @param context The parser context, used to report warnings on invalid values.
 * @param value The raw attribute value.
 * @param isX True for `refX` (left/center/right keywords), false for `refY`
 *   (top/center/bottom keywords).
 * @return The parsed length, or \c std::nullopt if the value was invalid (a warning is reported).
 */
std::optional<Lengthd> ParseSymbolRefAttribute(SVGParserContext& context, std::string_view value,
                                               bool isX) {
  // Keyword → percentage mapping. `center` is valid on both axes.
  if (value == "center") {
    return Lengthd(50.0, Lengthd::Unit::Percent);
  } else if (isX && value == "left") {
    return Lengthd(0.0, Lengthd::Unit::Percent);
  } else if (isX && value == "right") {
    return Lengthd(100.0, Lengthd::Unit::Percent);
  } else if (!isX && value == "top") {
    return Lengthd(0.0, Lengthd::Unit::Percent);
  } else if (!isX && value == "bottom") {
    return Lengthd(100.0, Lengthd::Unit::Percent);
  }

  return ParseLengthAttribute(context, value);
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
    ParseDiagnostic err;
    err.reason = "Unexpected data at end of attribute";
    err.range.start = FileOffset::Offset(maybeLengthResult.result().consumedChars);
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
 * Standard keywords: SourceGraphic, SourceAlpha, FillPaint, StrokePaint, BackgroundImage and
 * BackgroundAlpha. Any other non-empty string is treated as a named result reference.
 *
 * BackgroundImage and BackgroundAlpha read the Filter Effects backdrop, what the nearest isolation
 * group painted beneath the element. The SVG 1.1 `enable-background` snapshot they once read was
 * removed in SVG 2 and is not implemented.
 *
 * @param value Attribute value.
 * @return The parsed FilterInput.
 */
components::FilterInput ParseFilterInput(std::string_view value) {
  return components::FilterInput::FromAttribute(value);
}

/// \ref ParseFilterInput, where an empty value means the attribute is absent. Filter Effects
/// treats a reference to a result that does not exist like an unspecified input.
std::optional<components::FilterInput> ParseOptionalFilterInput(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  return ParseFilterInput(value);
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
  auto& primitive = element.entityHandle().template get<components::FilterPrimitiveComponent>();
  if (name == XMLQualifiedNameRef("in")) {
    primitive.in = ParseOptionalFilterInput(value);
    return true;
  }
  if (name == XMLQualifiedNameRef("in2")) {
    primitive.in2 = ParseOptionalFilterInput(value);
    return true;
  }
  if (name == XMLQualifiedNameRef("result")) {
    primitive.result = value.empty() ? std::nullopt : std::optional<RcString>(RcString(value));
    return true;
  }
  return false;
}

/**
 * Parses `x`, `y`, `width` and `height` of a filter or filter primitive into \p region, a
 * component whose region fields are optional. A value that does not parse leaves the field unset,
 * as it is when the attribute is absent, so parsing an empty value restores the default.
 *
 * @return True if the attribute was one of the four.
 */
template <typename Region>
bool ParseFilterRegionAttribute(SVGParserContext& context, Region& region,
                                const XMLQualifiedNameRef& name, std::string_view value) {
  std::optional<Lengthd>* field = nullptr;
  if (name == XMLQualifiedNameRef("x")) {
    field = &region.x;
  } else if (name == XMLQualifiedNameRef("y")) {
    field = &region.y;
  } else if (name == XMLQualifiedNameRef("width")) {
    field = &region.width;
  } else if (name == XMLQualifiedNameRef("height")) {
    field = &region.height;
  } else {
    return false;
  }
  *field = value.empty() ? std::nullopt : ParseLengthAttribute(context, value);
  return true;
}

/// \ref ParseFilterRegionAttribute for a filter primitive's subregion.
template <typename T>
bool ParsePrimitiveSubregion(SVGParserContext& context, T element, const XMLQualifiedNameRef& name,
                             std::string_view value) {
  return ParseFilterRegionAttribute(
      context, element.entityHandle().template get<components::FilterPrimitiveComponent>(), name,
      value);
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
    ParseDiagnostic err;
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
    ParseDiagnostic err;
    err.reason = "Unexpected data after angle value in '" + std::string(value) + "'";
    context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
  }

  return parseResult.result();
}

void ParsePresentationAttribute(SVGParserContext& context, SVGElement& element,
                                const XMLQualifiedNameRef& name, std::string_view value) {
  // TODO: Move this logic into SVGElement::setAttribute.

  // Storing the attribute already performs the presentation-attribute parse and reports both its
  // diagnostic and whether the value was recognized, so this asks for the outcome instead of
  // parsing the value once here and a second time inside the store.
  bool consumedAsPresentationAttribute = false;
  std::optional<ParseDiagnostic> error =
      AttributeParser::ApplyParsedAttribute(element, name, value, &consumedAsPresentationAttribute);

  // TODO: Detect the SVG namespace here and only parse elements in that namespace.
  // For now, we only parse attributes that are not in a namespace.
  if (name.namespacePrefix.empty()) {
    if (error.has_value()) {
      context.addSubparserWarning(std::move(error.value()), context.parserOriginFrom(value));
    } else if (!consumedAsPresentationAttribute && context.options().disableUserAttributes) {
      if (context.warningsEnabled()) {
        ParseDiagnostic err;
        err.reason = "Unknown attribute '" + name.toString() + "' (disableUserAttributes: true)";
        if (auto maybeLocation = context.getAttributeLocation(element, name)) {
          err.range.start = maybeLocation->start;
        }
        context.addWarning(std::move(err));
      }
      AttributeParser::RemoveParsedAttribute(element, name);
    }
  }
}

/**
 * Store a conditional-processing attribute (`requiredFeatures`, `requiredExtensions`,
 * `systemLanguage`) in the element's \ref components::ConditionalProcessingComponent.
 *
 * These attributes are valid on any element, so they are handled in the common attribute path.
 *
 * @param element The element to store the attribute on.
 * @param name The attribute name.
 * @param value The attribute value.
 * @return True if the attribute was a conditional-processing attribute.
 */
bool ParseConditionalProcessingAttribute(SVGElement& element, const XMLQualifiedNameRef& name,
                                         std::string_view value) {
  if (!components::IsConditionalProcessingAttribute(name)) {
    return false;
  }

  auto& conditional =
      element.entityHandle().get_or_emplace<components::ConditionalProcessingComponent>();
  return components::SetConditionalProcessingAttribute(conditional, name, value);
}

void ParseUnconditionalCommonAttribute(SVGParserContext& context, SVGElement& element,
                                       const XMLQualifiedNameRef& name, std::string_view value) {
  // TODO: Support namespaces on presentation attributes.
  // For now, only parse attributes that are not in a namespace as presentation attributes.
  if (IsAlwaysGenericAttribute(name)) {
    (void)AttributeParser::ApplyParsedAttribute(element, name, value);
  } else if (name.namespacePrefix.empty() &&
             ParseConditionalProcessingAttribute(element, name, value)) {
    // Conditional-processing attributes are stored on a component; keep the raw attribute so it
    // round-trips through the DOM.
    (void)AttributeParser::ApplyParsedAttribute(element, name, value);
  } else {
    ParsePresentationAttribute(context, element, name, value);
  }
}

template <typename T>
std::optional<ParseDiagnostic> ParseCommonAttribute(SVGParserContext& context, T& element,
                                                    const XMLQualifiedNameRef& name,
                                                    std::string_view value) {
  if (context.typedAttributesOnly()) {
    context.noteCommonAttributeFallThrough();
    return std::nullopt;
  }
  if constexpr (HasPathLength<T>) {
    if (name == XMLQualifiedNameRef("pathLength")) {
      // Parse the attribute as a number, and if it resolves set the length.
      if (auto maybeNumber = ParseNumberNoSuffix(value)) {
        element.setPathLength(maybeNumber.value());
      } else {
        ParseDiagnostic err;
        err.reason = "Invalid pathLength value '" + std::string(value) + "'";
        context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
      }

      return std::nullopt;
    }
  }

  parser::ParseUnconditionalCommonAttribute(context, element, name, value);
  return std::nullopt;
}

std::optional<ParseDiagnostic> ParseGradientCommonAttribute(SVGParserContext& context,
                                                            SVGGradientElement& element,
                                                            const XMLQualifiedNameRef& name,
                                                            std::string_view value) {
  if (name == XMLQualifiedNameRef("gradientUnits")) {
    if (value == "userSpaceOnUse") {
      element.setGradientUnits(GradientUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setGradientUnits(GradientUnits::ObjectBoundingBox);
    } else {
      ParseDiagnostic err;
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
      ParseDiagnostic err;
      err.reason = "Invalid spreadMethod value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(RcStringOrRef(value));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <typename T>
std::optional<ParseDiagnostic> ParseAttribute(SVGParserContext& context, T element,
                                              const XMLQualifiedNameRef& name,
                                              std::string_view value) {
  return ParseCommonAttribute(context, element, name, value);
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGClipPathElement>(SVGParserContext& context,
                                                                  SVGClipPathElement element,
                                                                  const XMLQualifiedNameRef& name,
                                                                  std::string_view value) {
  if (name == XMLQualifiedNameRef("clipPathUnits")) {
    if (value == "userSpaceOnUse") {
      element.setClipPathUnits(ClipPathUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setClipPathUnits(ClipPathUnits::ObjectBoundingBox);
    } else {
      ParseDiagnostic err;
      err.reason = "Invalid clipPathUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGMaskElement>(SVGParserContext& context,
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
      ParseDiagnostic err;
      err.reason = "Invalid maskUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("maskContentUnits")) {
    if (value == "userSpaceOnUse") {
      element.setMaskContentUnits(MaskContentUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setMaskContentUnits(MaskContentUnits::ObjectBoundingBox);
    } else {
      ParseDiagnostic err;
      err.reason = "Invalid maskContentUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFilterElement>(SVGParserContext& context,
                                                                SVGFilterElement element,
                                                                const XMLQualifiedNameRef& name,
                                                                std::string_view value) {
  if (ParseFilterRegionAttribute(context, element.entityHandle().get<components::FilterComponent>(),
                                 name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("filterUnits")) {
    auto& filter = element.entityHandle().get<components::FilterComponent>();
    filter.filterUnits.reset();
    if (value == "userSpaceOnUse") {
      filter.filterUnits = FilterUnits::UserSpaceOnUse;
    } else if (value == "objectBoundingBox") {
      filter.filterUnits = FilterUnits::ObjectBoundingBox;
    } else if (!value.empty()) {
      ParseDiagnostic err;
      err.reason = "Invalid filterUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("primitiveUnits")) {
    auto& filter = element.entityHandle().get<components::FilterComponent>();
    filter.primitiveUnits.reset();
    if (value == "userSpaceOnUse") {
      filter.primitiveUnits = PrimitiveUnits::UserSpaceOnUse;
    } else if (value == "objectBoundingBox") {
      filter.primitiveUnits = PrimitiveUnits::ObjectBoundingBox;
    } else if (!value.empty()) {
      ParseDiagnostic err;
      err.reason = "Invalid primitiveUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("color-interpolation-filters")) {
    auto& filter = element.entityHandle().get<components::FilterComponent>();
    filter.colorInterpolationFilters.reset();
    if (value == "sRGB") {
      filter.colorInterpolationFilters = ColorInterpolationFilters::SRGB;
    } else if (value == "linearRGB") {
      filter.colorInterpolationFilters = ColorInterpolationFilters::LinearRGB;
    }
  } else if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    auto& filter = element.entityHandle().get<components::FilterComponent>();
    filter.href =
        value.empty() ? std::nullopt : std::optional<Reference>(Reference(RcString(value)));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEGaussianBlurElement>(
    SVGParserContext& context, SVGFEGaussianBlurElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    // Handled by filter primitive standard attributes.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("stdDeviation")) {
    const components::FEGaussianBlurComponent defaults;
    element.setStdDeviation(defaults.stdDeviationX, defaults.stdDeviationY);
    const auto maybeNumber2d = Number2dParser::Parse(value);
    if (maybeNumber2d.hasResult()) {
      const Number2dParser::Result number2d = maybeNumber2d.result();
      // TODO: Does this handle whitespace at the end of the string?
      if (number2d.consumedChars == value.size()) {
        element.setStdDeviation(number2d.numberX, number2d.numberY);
      } else {
        ParseDiagnostic err;
        err.reason = "Unexpected additional data in stdDeviation, '" + std::string(value) + "'";
        context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
      }
    } else {
      ParseDiagnostic err;
      err.reason = "Invalid stdDeviation value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("edgeMode")) {
    auto& comp = element.entityHandle().get<components::FEGaussianBlurComponent>();
    comp.edgeMode = components::FEGaussianBlurComponent().edgeMode;
    if (value == "none") {
      comp.edgeMode = components::FEGaussianBlurComponent::EdgeMode::None;
    } else if (value == "duplicate") {
      comp.edgeMode = components::FEGaussianBlurComponent::EdgeMode::Duplicate;
    } else if (value == "wrap") {
      comp.edgeMode = components::FEGaussianBlurComponent::EdgeMode::Wrap;
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEBlendElement>(SVGParserContext& context,
                                                                 SVGFEBlendElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("mode")) {
    auto& comp = element.entityHandle().get<components::FEBlendComponent>();
    comp.mode = components::FEBlendComponent().mode;
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
    } else if (value == "overlay") {
      comp.mode = components::FEBlendComponent::Mode::Overlay;
    } else if (value == "color-dodge") {
      comp.mode = components::FEBlendComponent::Mode::ColorDodge;
    } else if (value == "color-burn") {
      comp.mode = components::FEBlendComponent::Mode::ColorBurn;
    } else if (value == "hard-light") {
      comp.mode = components::FEBlendComponent::Mode::HardLight;
    } else if (value == "soft-light") {
      comp.mode = components::FEBlendComponent::Mode::SoftLight;
    } else if (value == "difference") {
      comp.mode = components::FEBlendComponent::Mode::Difference;
    } else if (value == "exclusion") {
      comp.mode = components::FEBlendComponent::Mode::Exclusion;
    } else if (value == "hue") {
      comp.mode = components::FEBlendComponent::Mode::Hue;
    } else if (value == "saturation") {
      comp.mode = components::FEBlendComponent::Mode::Saturation;
    } else if (value == "color") {
      comp.mode = components::FEBlendComponent::Mode::Color;
    } else if (value == "luminosity") {
      comp.mode = components::FEBlendComponent::Mode::Luminosity;
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEComponentTransferElement>(
    SVGParserContext& context, SVGFEComponentTransferElement element,
    const XMLQualifiedNameRef& name, std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
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
bool ParseFuncAttributes(components::FEFuncComponent& comp, const XMLQualifiedNameRef& name,
                         std::string_view value) {
  const components::FEFuncComponent defaults(comp.channel);
  if (name == XMLQualifiedNameRef("type")) {
    comp.type = defaults.type;
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
    std::vector<double> parsedValues;
    std::string_view remaining = value;
    bool valid = true;
    while (!remaining.empty()) {
      while (!remaining.empty() &&
             (remaining.front() == ' ' || remaining.front() == ',' || remaining.front() == '\t' ||
              remaining.front() == '\n' || remaining.front() == '\r')) {
        remaining.remove_prefix(1);
      }
      if (remaining.empty()) {
        break;
      }
      const auto maybeNumber = donner::parser::NumberParser::Parse(remaining);
      if (maybeNumber.hasResult()) {
        parsedValues.push_back(maybeNumber.result().number);
        remaining.remove_prefix(maybeNumber.result().consumedChars);
      } else {
        valid = false;
        break;
      }
    }
    while (!remaining.empty() &&
           (remaining.front() == ' ' || remaining.front() == ',' || remaining.front() == '\t' ||
            remaining.front() == '\n' || remaining.front() == '\r')) {
      remaining.remove_prefix(1);
    }
    if (!remaining.empty()) {
      valid = false;
    }

    if (valid) {
      comp.tableValues = std::move(parsedValues);
    } else {
      comp.tableValues.clear();
    }
  } else if (name == XMLQualifiedNameRef("slope")) {
    comp.slope = ParseNumberNoSuffix(value).value_or(defaults.slope);
  } else if (name == XMLQualifiedNameRef("intercept")) {
    comp.intercept = ParseNumberNoSuffix(value).value_or(defaults.intercept);
  } else if (name == XMLQualifiedNameRef("amplitude")) {
    comp.amplitude = ParseNumberNoSuffix(value).value_or(defaults.amplitude);
  } else if (name == XMLQualifiedNameRef("exponent")) {
    comp.exponent = ParseNumberNoSuffix(value).value_or(defaults.exponent);
  } else if (name == XMLQualifiedNameRef("offset")) {
    comp.offset = ParseNumberNoSuffix(value).value_or(defaults.offset);
  } else {
    return false;
  }
  return true;
}

}  // namespace

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEFuncRElement>(SVGParserContext& context,
                                                                 SVGFEFuncRElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  if (!ParseFuncAttributes(comp, name, value)) {
    return ParseCommonAttribute(context, element, name, value);
  }
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEFuncGElement>(SVGParserContext& context,
                                                                 SVGFEFuncGElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  if (!ParseFuncAttributes(comp, name, value)) {
    return ParseCommonAttribute(context, element, name, value);
  }
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEFuncBElement>(SVGParserContext& context,
                                                                 SVGFEFuncBElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  if (!ParseFuncAttributes(comp, name, value)) {
    return ParseCommonAttribute(context, element, name, value);
  }
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEFuncAElement>(SVGParserContext& context,
                                                                 SVGFEFuncAElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  auto& comp = element.entityHandle().get<components::FEFuncComponent>();
  if (!ParseFuncAttributes(comp, name, value)) {
    return ParseCommonAttribute(context, element, name, value);
  }
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEColorMatrixElement>(
    SVGParserContext& context, SVGFEColorMatrixElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("type")) {
    auto& comp = element.entityHandle().get<components::FEColorMatrixComponent>();
    comp.type = components::FEColorMatrixComponent().type;
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
      while (!remaining.empty() &&
             (remaining.front() == ' ' || remaining.front() == ',' || remaining.front() == '\t' ||
              remaining.front() == '\n' || remaining.front() == '\r')) {
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
std::optional<ParseDiagnostic> ParseAttribute<SVGFECompositeElement>(
    SVGParserContext& context, SVGFECompositeElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("operator")) {
    auto& comp = element.entityHandle().get<components::FECompositeComponent>();
    comp.op = components::FECompositeComponent().op;
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
    element.entityHandle().get<components::FECompositeComponent>().k1 =
        ParseNumberNoSuffix(value).value_or(components::FECompositeComponent().k1);
  } else if (name == XMLQualifiedNameRef("k2")) {
    element.entityHandle().get<components::FECompositeComponent>().k2 =
        ParseNumberNoSuffix(value).value_or(components::FECompositeComponent().k2);
  } else if (name == XMLQualifiedNameRef("k3")) {
    element.entityHandle().get<components::FECompositeComponent>().k3 =
        ParseNumberNoSuffix(value).value_or(components::FECompositeComponent().k3);
  } else if (name == XMLQualifiedNameRef("k4")) {
    element.entityHandle().get<components::FECompositeComponent>().k4 =
        ParseNumberNoSuffix(value).value_or(components::FECompositeComponent().k4);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEDropShadowElement>(
    SVGParserContext& context, SVGFEDropShadowElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  }

  auto& comp = element.entityHandle().get<components::FEDropShadowComponent>();
  const components::FEDropShadowComponent defaults;
  if (name == XMLQualifiedNameRef("dx")) {
    comp.dx = ParseNumberNoSuffix(value).value_or(defaults.dx);
  } else if (name == XMLQualifiedNameRef("dy")) {
    comp.dy = ParseNumberNoSuffix(value).value_or(defaults.dy);
  } else if (name == XMLQualifiedNameRef("stdDeviation")) {
    comp.stdDeviationX = defaults.stdDeviationX;
    comp.stdDeviationY = defaults.stdDeviationY;
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
std::optional<ParseDiagnostic> ParseAttribute<SVGFEFloodElement>(SVGParserContext& context,
                                                                 SVGFEFloodElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
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
std::optional<ParseDiagnostic> ParseAttribute<SVGFEMorphologyElement>(
    SVGParserContext& context, SVGFEMorphologyElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("operator")) {
    auto& comp = element.entityHandle().get<components::FEMorphologyComponent>();
    comp.op = components::FEMorphologyComponent().op;
    if (value == "erode") {
      comp.op = components::FEMorphologyComponent::Operator::Erode;
    } else if (value == "dilate") {
      comp.op = components::FEMorphologyComponent::Operator::Dilate;
    }
  } else if (name == XMLQualifiedNameRef("radius")) {
    auto& comp = element.entityHandle().get<components::FEMorphologyComponent>();
    comp.radiusX = components::FEMorphologyComponent().radiusX;
    comp.radiusY = components::FEMorphologyComponent().radiusY;
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
std::optional<ParseDiagnostic> ParseAttribute<SVGFEDisplacementMapElement>(
    SVGParserContext& context, SVGFEDisplacementMapElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("scale")) {
    auto& comp = element.entityHandle().get<components::FEDisplacementMapComponent>();
    comp.scale =
        ParseNumberNoSuffix(value).value_or(components::FEDisplacementMapComponent().scale);
  } else if (name == XMLQualifiedNameRef("xChannelSelector")) {
    auto& comp = element.entityHandle().get<components::FEDisplacementMapComponent>();
    comp.xChannelSelector = components::FEDisplacementMapComponent().xChannelSelector;
    if (value == "R") {
      comp.xChannelSelector = components::FEDisplacementMapComponent::Channel::R;
    } else if (value == "G") {
      comp.xChannelSelector = components::FEDisplacementMapComponent::Channel::G;
    } else if (value == "B") {
      comp.xChannelSelector = components::FEDisplacementMapComponent::Channel::B;
    } else if (value == "A") {
      comp.xChannelSelector = components::FEDisplacementMapComponent::Channel::A;
    }
  } else if (name == XMLQualifiedNameRef("yChannelSelector")) {
    auto& comp = element.entityHandle().get<components::FEDisplacementMapComponent>();
    comp.yChannelSelector = components::FEDisplacementMapComponent().yChannelSelector;
    if (value == "R") {
      comp.yChannelSelector = components::FEDisplacementMapComponent::Channel::R;
    } else if (value == "G") {
      comp.yChannelSelector = components::FEDisplacementMapComponent::Channel::G;
    } else if (value == "B") {
      comp.yChannelSelector = components::FEDisplacementMapComponent::Channel::B;
    } else if (value == "A") {
      comp.yChannelSelector = components::FEDisplacementMapComponent::Channel::A;
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEImageElement>(SVGParserContext& context,
                                                                 SVGFEImageElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    auto& comp = element.entityHandle().get<components::FEImageComponent>();
    comp.href = RcString(value);
    // Also set on ImageComponent so ResourceManagerContext loads the image.
    if (value.empty()) {
      element.entityHandle().remove<components::ImageComponent>();
    } else {
      element.entityHandle().get_or_emplace<components::ImageComponent>().href = RcString(value);
    }
  } else if (name == XMLQualifiedNameRef("preserveAspectRatio")) {
    auto& comp = element.entityHandle().get<components::FEImageComponent>();
    comp.preserveAspectRatio = components::FEImageComponent().preserveAspectRatio;
    auto maybeResult = parser::PreserveAspectRatioParser::Parse(value);
    if (maybeResult.hasResult()) {
      comp.preserveAspectRatio = maybeResult.result();
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEDiffuseLightingElement>(
    SVGParserContext& context, SVGFEDiffuseLightingElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("surfaceScale")) {
    auto& comp = element.entityHandle().get<components::FEDiffuseLightingComponent>();
    comp.surfaceScale =
        ParseNumberNoSuffix(value).value_or(components::FEDiffuseLightingComponent().surfaceScale);
  } else if (name == XMLQualifiedNameRef("diffuseConstant")) {
    auto& comp = element.entityHandle().get<components::FEDiffuseLightingComponent>();
    const std::optional<double> n = ParseNumberNoSuffix(value);
    comp.diffuseConstant =
        n ? std::max(0.0, *n) : components::FEDiffuseLightingComponent().diffuseConstant;
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFESpecularLightingElement>(
    SVGParserContext& context, SVGFESpecularLightingElement element,
    const XMLQualifiedNameRef& name, std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("surfaceScale")) {
    auto& comp = element.entityHandle().get<components::FESpecularLightingComponent>();
    comp.surfaceScale =
        ParseNumberNoSuffix(value).value_or(components::FESpecularLightingComponent().surfaceScale);
  } else if (name == XMLQualifiedNameRef("specularConstant")) {
    auto& comp = element.entityHandle().get<components::FESpecularLightingComponent>();
    const std::optional<double> n = ParseNumberNoSuffix(value);
    comp.specularConstant =
        n ? std::max(0.0, *n) : components::FESpecularLightingComponent().specularConstant;
  } else if (name == XMLQualifiedNameRef("specularExponent")) {
    auto& comp = element.entityHandle().get<components::FESpecularLightingComponent>();
    comp.specularExponent = ParseNumberNoSuffix(value).value_or(
        components::FESpecularLightingComponent().specularExponent);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEDistantLightElement>(
    SVGParserContext& context, SVGFEDistantLightElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  auto& comp = element.entityHandle().get<components::LightSourceComponent>();
  const components::LightSourceComponent defaults(comp.type);
  if (name == XMLQualifiedNameRef("azimuth")) {
    comp.azimuth = ParseNumberNoSuffix(value).value_or(defaults.azimuth);
  } else if (name == XMLQualifiedNameRef("elevation")) {
    comp.elevation = ParseNumberNoSuffix(value).value_or(defaults.elevation);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEPointLightElement>(
    SVGParserContext& context, SVGFEPointLightElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  auto& comp = element.entityHandle().get<components::LightSourceComponent>();
  const components::LightSourceComponent defaults(comp.type);
  if (name == XMLQualifiedNameRef("x")) {
    comp.x = ParseNumberNoSuffix(value).value_or(defaults.x);
  } else if (name == XMLQualifiedNameRef("y")) {
    comp.y = ParseNumberNoSuffix(value).value_or(defaults.y);
  } else if (name == XMLQualifiedNameRef("z")) {
    comp.z = ParseNumberNoSuffix(value).value_or(defaults.z);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFESpotLightElement>(
    SVGParserContext& context, SVGFESpotLightElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  auto& comp = element.entityHandle().get<components::LightSourceComponent>();
  const components::LightSourceComponent defaults(comp.type);
  if (name == XMLQualifiedNameRef("x")) {
    comp.x = ParseNumberNoSuffix(value).value_or(defaults.x);
  } else if (name == XMLQualifiedNameRef("y")) {
    comp.y = ParseNumberNoSuffix(value).value_or(defaults.y);
  } else if (name == XMLQualifiedNameRef("z")) {
    comp.z = ParseNumberNoSuffix(value).value_or(defaults.z);
  } else if (name == XMLQualifiedNameRef("pointsAtX")) {
    comp.pointsAtX = ParseNumberNoSuffix(value).value_or(defaults.pointsAtX);
  } else if (name == XMLQualifiedNameRef("pointsAtY")) {
    comp.pointsAtY = ParseNumberNoSuffix(value).value_or(defaults.pointsAtY);
  } else if (name == XMLQualifiedNameRef("pointsAtZ")) {
    comp.pointsAtZ = ParseNumberNoSuffix(value).value_or(defaults.pointsAtZ);
  } else if (name == XMLQualifiedNameRef("specularExponent")) {
    comp.spotExponent = ParseNumberNoSuffix(value).value_or(defaults.spotExponent);
  } else if (name == XMLQualifiedNameRef("limitingConeAngle")) {
    comp.limitingConeAngle = ParseNumberNoSuffix(value);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEConvolveMatrixElement>(
    SVGParserContext& context, SVGFEConvolveMatrixElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("order")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    const auto exceedsLimit = [](double number) {
      return std::isfinite(number) &&
             number > static_cast<double>(components::kMaximumFilterConvolveOrder);
    };
    comp.orderExceedsLimit = false;
    comp.orderX = components::FEConvolveMatrixComponent().orderX;
    comp.orderY = components::FEConvolveMatrixComponent().orderY;
    const auto firstNumber = donner::parser::NumberParser::Parse(value);
    if (firstNumber.hasResult()) {
      const std::optional<int> orderX =
          CheckedInteger(firstNumber.result().number, 1, components::kMaximumFilterConvolveOrder);
      if (!orderX.has_value()) {
        comp.orderExceedsLimit = exceedsLimit(firstNumber.result().number);
        return std::nullopt;
      }
      int orderY = *orderX;
      std::string_view remaining = value.substr(firstNumber.result().consumedChars);
      while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == ',')) {
        remaining.remove_prefix(1);
      }
      if (!remaining.empty()) {
        const auto secondNumber = donner::parser::NumberParser::Parse(remaining);
        if (secondNumber.hasResult()) {
          comp.orderExceedsLimit = exceedsLimit(secondNumber.result().number);
          orderY = CheckedInteger(secondNumber.result().number, 1,
                                  components::kMaximumFilterConvolveOrder)
                       .value_or(orderY);
        }
      }
      comp.orderX = *orderX;
      comp.orderY = orderY;
    }
  } else if (name == XMLQualifiedNameRef("kernelMatrix")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    comp.kernelMatrix.clear();
    std::string_view remaining = value;
    while (!remaining.empty()) {
      while (!remaining.empty() &&
             (remaining.front() == ' ' || remaining.front() == ',' || remaining.front() == '\n' ||
              remaining.front() == '\r' || remaining.front() == '\t')) {
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
    comp.divisor = components::FEConvolveMatrixComponent().divisor;
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.divisor = maybeNumber.result().number;
    }
  } else if (name == XMLQualifiedNameRef("bias")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    comp.bias = components::FEConvolveMatrixComponent().bias;
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.bias = maybeNumber.result().number;
    }
  } else if (name == XMLQualifiedNameRef("targetX")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    comp.targetX = components::FEConvolveMatrixComponent().targetX;
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.targetX = CheckedInteger(maybeNumber.result().number, std::numeric_limits<int>::min(),
                                    std::numeric_limits<int>::max());
    }
  } else if (name == XMLQualifiedNameRef("targetY")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    comp.targetY = components::FEConvolveMatrixComponent().targetY;
    const auto maybeNumber = donner::parser::NumberParser::Parse(value);
    if (maybeNumber.hasResult()) {
      comp.targetY = CheckedInteger(maybeNumber.result().number, std::numeric_limits<int>::min(),
                                    std::numeric_limits<int>::max());
    }
  } else if (name == XMLQualifiedNameRef("edgeMode")) {
    auto& comp = element.entityHandle().get<components::FEConvolveMatrixComponent>();
    comp.edgeMode = components::FEConvolveMatrixComponent().edgeMode;
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
std::optional<ParseDiagnostic> ParseAttribute<SVGFETurbulenceElement>(
    SVGParserContext& context, SVGFETurbulenceElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("baseFrequency")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    comp.baseFrequencyX = components::FETurbulenceComponent().baseFrequencyX;
    comp.baseFrequencyY = components::FETurbulenceComponent().baseFrequencyY;
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
    comp.numOctaves = components::FETurbulenceComponent().numOctaves;
    if (auto maybeNumber = ParseNumberNoSuffix(value)) {
      comp.numOctaves = CheckedInteger(*maybeNumber, std::numeric_limits<int>::min(),
                                       std::numeric_limits<int>::max())
                            .value_or(comp.numOctaves);
    }
  } else if (name == XMLQualifiedNameRef("seed")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    comp.seed = ParseNumberNoSuffix(value).value_or(components::FETurbulenceComponent().seed);
  } else if (name == XMLQualifiedNameRef("type")) {
    auto& comp = element.entityHandle().get<components::FETurbulenceComponent>();
    comp.type = components::FETurbulenceComponent().type;
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
std::optional<ParseDiagnostic> ParseAttribute<SVGFETileElement>(SVGParserContext& context,
                                                                SVGFETileElement element,
                                                                const XMLQualifiedNameRef& name,
                                                                std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEOffsetElement>(SVGParserContext& context,
                                                                  SVGFEOffsetElement element,
                                                                  const XMLQualifiedNameRef& name,
                                                                  std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("dx")) {
    element.setOffset(ParseNumberNoSuffix(value).value_or(components::FEOffsetComponent().dx),
                      element.dy());
  } else if (name == XMLQualifiedNameRef("dy")) {
    element.setOffset(element.dx(),
                      ParseNumberNoSuffix(value).value_or(components::FEOffsetComponent().dy));
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEMergeElement>(SVGParserContext& context,
                                                                 SVGFEMergeElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  if (ParsePrimitiveSubregion(context, element, name, value)) {
    return std::nullopt;
  } else if (ParseFilterPrimitiveAttributes(element, name, value)) {
    return std::nullopt;
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGFEMergeNodeElement>(
    SVGParserContext& context, SVGFEMergeNodeElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  if (name == XMLQualifiedNameRef("in")) {
    element.entityHandle().get<components::FEMergeNodeComponent>().in =
        ParseOptionalFilterInput(value);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGImageElement>(SVGParserContext& context,
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
std::optional<ParseDiagnostic> ParseAttribute<SVGLineElement>(SVGParserContext& context,
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
std::optional<ParseDiagnostic> ParseAttribute<SVGLinearGradientElement>(
    SVGParserContext& context, SVGLinearGradientElement element, const XMLQualifiedNameRef& name,
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
std::optional<ParseDiagnostic> ParseAttribute<SVGPatternElement>(SVGParserContext& context,
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
      ParseDiagnostic err;
      err.reason = "Invalid patternUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("patternContentUnits")) {
    if (value == "userSpaceOnUse") {
      element.setPatternContentUnits(PatternContentUnits::UserSpaceOnUse);
    } else if (value == "objectBoundingBox") {
      element.setPatternContentUnits(PatternContentUnits::ObjectBoundingBox);
    } else {
      ParseDiagnostic err;
      err.reason = "Invalid patternContentUnits value '" + std::string(value) + "'";
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
std::optional<ParseDiagnostic> ParseAttribute<SVGPolygonElement>(SVGParserContext& context,
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
std::optional<ParseDiagnostic> ParseAttribute<SVGPolylineElement>(SVGParserContext& context,
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
std::optional<ParseDiagnostic> ParseAttribute<SVGRadialGradientElement>(
    SVGParserContext& context, SVGRadialGradientElement element, const XMLQualifiedNameRef& name,
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
std::optional<ParseDiagnostic> ParseAttribute<SVGSVGElement>(SVGParserContext& context,
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
std::optional<ParseDiagnostic> ParseAttribute<SVGStopElement>(SVGParserContext& context,
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

namespace {
std::optional<ParseDiagnostic> ParseAnimationElementAttribute(SVGParserContext& context,
                                                              SVGElement element,
                                                              const XMLQualifiedNameRef& name,
                                                              std::string_view value) {
  auto parsed = ParseAnimationAttribute(element.entityHandle(), name, value);
  if (parsed.hasError()) {
    context.addSubparserWarning(std::move(parsed.error()), context.parserOriginFrom(value));
    return std::nullopt;
  }
  return parsed.result() ? std::nullopt : ParseCommonAttribute(context, element, name, value);
}
}  // namespace

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGAnimateElement>(SVGParserContext& context,
                                                                 SVGAnimateElement element,
                                                                 const XMLQualifiedNameRef& name,
                                                                 std::string_view value) {
  return ParseAnimationElementAttribute(context, element, name, value);
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGAnimateTransformElement>(
    SVGParserContext& context, SVGAnimateTransformElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  return ParseAnimationElementAttribute(context, element, name, value);
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGAnimateMotionElement>(
    SVGParserContext& context, SVGAnimateMotionElement element, const XMLQualifiedNameRef& name,
    std::string_view value) {
  return ParseAnimationElementAttribute(context, element, name, value);
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGMPathElement>(SVGParserContext& context,
                                                               SVGMPathElement element,
                                                               const XMLQualifiedNameRef& name,
                                                               std::string_view value) {
  return ParseAnimationElementAttribute(context, element, name, value);
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGSetElement>(SVGParserContext& context,
                                                             SVGSetElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  return ParseAnimationElementAttribute(context, element, name, value);
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGStyleElement>(SVGParserContext& context,
                                                               SVGStyleElement element,
                                                               const XMLQualifiedNameRef& name,
                                                               std::string_view value) {
  if (name == XMLQualifiedNameRef("type")) {
    if (value.empty() ||
        StringUtils::Equals<StringComparison::IgnoreCase>(value, std::string_view("text/css"))) {
      // The value is valid.
    } else {
      ParseDiagnostic err;
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
std::optional<ParseDiagnostic> ParseAttribute<SVGUseElement>(SVGParserContext& context,
                                                             SVGUseElement element,
                                                             const XMLQualifiedNameRef& name,
                                                             std::string_view value) {
  if (ParseXYWidthHeight(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    // The shared attribute write resolves `href` over `xlink:href` regardless of attribute order,
    // as DOM mutations and incremental reparses do.
    return AttributeParser::ApplyParsedAttribute(element, name, value);
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

/// SVG2 allows `<marker>`/`<symbol>` `refX`/`refY` to be a keyword that resolves
/// to a percentage of the reference viewport: `refX` accepts `left`/`center`/
/// `right` (→ 0%/50%/100% of the width) and `refY` accepts `top`/`center`/
/// `bottom` (→ 0%/50%/100% of the height). Percentages already resolve against
/// the viewport extent at render time, so the keyword just maps to a percentage
/// `Lengthd`. Returns `std::nullopt` when @p value is not one of these keywords
/// (the caller then falls back to the normal length/number grammar).
std::optional<Lengthd> ParseRefKeyword(std::string_view value, bool isX) {
  if (value == "center") {
    return Lengthd(50.0, Lengthd::Unit::Percent);
  }
  if (isX) {
    if (value == "left") {
      return Lengthd(0.0, Lengthd::Unit::Percent);
    }
    if (value == "right") {
      return Lengthd(100.0, Lengthd::Unit::Percent);
    }
  } else {
    if (value == "top") {
      return Lengthd(0.0, Lengthd::Unit::Percent);
    }
    if (value == "bottom") {
      return Lengthd(100.0, Lengthd::Unit::Percent);
    }
  }
  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGMarkerElement>(SVGParserContext& context,
                                                                SVGMarkerElement element,
                                                                const XMLQualifiedNameRef& name,
                                                                std::string_view value) {
  if (ParseViewBoxPreserveAspectRatio(context, element, name, value)) {
    // Warning already added if there was an error.
    return std::nullopt;
  } else if (name == XMLQualifiedNameRef("markerWidth")) {
    if (auto maybeLength = ParseLengthAttribute(context, value)) {
      element.setMarkerWidth(maybeLength.value());
    }
    // Warning already added by ParseLengthAttribute on error.
  } else if (name == XMLQualifiedNameRef("markerHeight")) {
    if (auto maybeLength = ParseLengthAttribute(context, value)) {
      element.setMarkerHeight(maybeLength.value());
    }
  } else if (name == XMLQualifiedNameRef("refX")) {
    if (auto keyword = ParseRefKeyword(value, /*isX=*/true)) {
      element.setRefX(*keyword);
    } else if (auto maybeLength = ParseLengthAttribute(context, value)) {
      element.setRefX(maybeLength.value());
    }
  } else if (name == XMLQualifiedNameRef("refY")) {
    if (auto keyword = ParseRefKeyword(value, /*isX=*/false)) {
      element.setRefY(*keyword);
    } else if (auto maybeLength = ParseLengthAttribute(context, value)) {
      element.setRefY(maybeLength.value());
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
      ParseDiagnostic err;
      err.reason = "Invalid markerUnits value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGSymbolElement>(SVGParserContext& context,
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
    if (auto maybeLength = ParseSymbolRefAttribute(context, value, /*isX=*/true)) {
      element.setRefX(maybeLength.value());
    }
    // Warning already added by ParseSymbolRefAttribute on error.
  } else if (name == XMLQualifiedNameRef("refY")) {
    if (auto maybeLength = ParseSymbolRefAttribute(context, value, /*isX=*/false)) {
      element.setRefY(maybeLength.value());
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
std::optional<ParseDiagnostic> ParseAttribute<SVGTextElement>(SVGParserContext& context,
                                                              SVGTextElement element,
                                                              const XMLQualifiedNameRef& name,
                                                              std::string_view value) {
  if (name == XMLQualifiedNameRef("textLength")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setTextLength(length);
    }
  } else if (name == XMLQualifiedNameRef("lengthAdjust")) {
    if (value == "spacing") {
      element.setLengthAdjust(LengthAdjust::Spacing);
    } else if (value == "spacingAndGlyphs") {
      element.setLengthAdjust(LengthAdjust::SpacingAndGlyphs);
    } else {
      ParseDiagnostic err;
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
 * Parses attributes for \ref xml_a elements.
 *
 * `<a>` is a transparent grouping/text-content element. It carries the hyperlink target
 * (`href` / `xlink:href`) and, as a text-content group, the per-glyph positioning attributes
 * (`x`, `y`, `dx`, `dy`, `rotate`) plus `textLength` / `lengthAdjust`, mirroring \ref xml_tspan.
 *
 * @param context The parser context.
 * @param element The element to parse.
 * @param name The name of the attribute to parse.
 * @param value The value of the attribute to parse.
 */
template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGAElement>(SVGParserContext& context,
                                                           SVGAElement element,
                                                           const XMLQualifiedNameRef& name,
                                                           std::string_view value) {
  if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(RcString(value));
  } else if (name == XMLQualifiedNameRef("x")) {
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
  } else if (name == XMLQualifiedNameRef("textLength")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setTextLength(length);
    }
  } else if (name == XMLQualifiedNameRef("lengthAdjust")) {
    if (value == "spacing") {
      element.setLengthAdjust(LengthAdjust::Spacing);
    } else if (value == "spacingAndGlyphs") {
      element.setLengthAdjust(LengthAdjust::SpacingAndGlyphs);
    }
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
std::optional<ParseDiagnostic> ParseAttribute<SVGTSpanElement>(SVGParserContext& context,
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
  } else if (name == XMLQualifiedNameRef("textLength")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setTextLength(length);
    }
  } else if (name == XMLQualifiedNameRef("lengthAdjust")) {
    if (value == "spacing") {
      element.setLengthAdjust(LengthAdjust::Spacing);
    } else if (value == "spacingAndGlyphs") {
      element.setLengthAdjust(LengthAdjust::SpacingAndGlyphs);
    }
  } else {
    return ParseCommonAttribute(context, element, name, value);
  }

  return std::nullopt;
}

/**
 * Parses attributes for \ref xml_textPath elements.
 */
template <>
std::optional<ParseDiagnostic> ParseAttribute<SVGTextPathElement>(SVGParserContext& context,
                                                                  SVGTextPathElement element,
                                                                  const XMLQualifiedNameRef& name,
                                                                  std::string_view value) {
  if (name == XMLQualifiedNameRef("href") || name == XMLQualifiedNameRef("xlink", "href")) {
    element.setHref(RcStringOrRef(value));
  } else if (name == XMLQualifiedNameRef("path")) {
    auto& comp = element.entityHandle().get_or_emplace<components::TextPathComponent>();
    comp.inlinePath.reset();
    // A `path` that yields no geometry is ignored so that `href` still applies, per SVG 2
    // textPath: the referenced path is the fallback when `path` is absent or in error.
    ParseResult<Path> maybePath = PathParser::Parse(value);
    if (maybePath.hasResult() && !maybePath.result().empty()) {
      comp.inlinePath = std::move(maybePath.result());
    }
    if (maybePath.hasError()) {
      context.addSubparserWarning(std::move(maybePath.error()), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("startOffset")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setStartOffset(*length);
    }
  } else if (name == XMLQualifiedNameRef("textLength")) {
    if (auto length = ParseLengthAttribute(context, value)) {
      element.setTextLength(length);
    }
  } else if (name == XMLQualifiedNameRef("lengthAdjust")) {
    if (value == "spacing") {
      element.setLengthAdjust(LengthAdjust::Spacing);
    } else if (value == "spacingAndGlyphs") {
      element.setLengthAdjust(LengthAdjust::SpacingAndGlyphs);
    } else {
      ParseDiagnostic err;
      err.reason = "Invalid lengthAdjust value '" + std::string(value) + "'";
      context.addSubparserWarning(std::move(err), context.parserOriginFrom(value));
    }
  } else if (name == XMLQualifiedNameRef("method")) {
    auto& comp = element.entityHandle().get_or_emplace<components::TextPathComponent>();
    if (value == "align") {
      comp.method = components::TextPathMethod::Align;
    } else if (value == "stretch") {
      comp.method = components::TextPathMethod::Stretch;
    }
  } else if (name == XMLQualifiedNameRef("side")) {
    auto& comp = element.entityHandle().get_or_emplace<components::TextPathComponent>();
    if (value == "left") {
      comp.side = components::TextPathSide::Left;
    } else if (value == "right") {
      comp.side = components::TextPathSide::Right;
    }
  } else if (name == XMLQualifiedNameRef("spacing")) {
    auto& comp = element.entityHandle().get_or_emplace<components::TextPathComponent>();
    if (value == "auto") {
      comp.spacing = components::TextPathSpacing::Auto;
    } else if (value == "exact") {
      comp.spacing = components::TextPathSpacing::Exact;
    }
  } else if (name == XMLQualifiedNameRef("x")) {
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

/// Signature of the per-element-type attribute handler stored in \ref kParseAttributeTable.
using ParseAttributeFn = std::optional<ParseDiagnostic> (*)(SVGParserContext&, SVGElement&,
                                                            const XMLQualifiedNameRef&,
                                                            std::string_view);

/// Downcasts to \p ElementT and runs that type's attribute handler.
template <typename ElementT>
std::optional<ParseDiagnostic> ParseAttributeAs(SVGParserContext& context, SVGElement& element,
                                                const XMLQualifiedNameRef& name,
                                                std::string_view value) {
  ElementT elementDerived = element.cast<ElementT>();
  return ParseAttribute(context, elementDerived, name, value);
}

/// Runs the handler shared by every element type, used when no element type claims the runtime
/// type (for example \ref ElementType::Unknown).
std::optional<ParseDiagnostic> ParseAttributeGeneric(SVGParserContext& context, SVGElement& element,
                                                     const XMLQualifiedNameRef& name,
                                                     std::string_view value) {
  return ParseAttribute(context, element, name, value);
}

/// Returns true when every element type in \p Types indexes inside a \ref kElementTypeCount-sized
/// table. Guards the assumption \ref kElementTypeCount encodes, that \ref ElementType::Use is the
/// last enumerator: an enumerator added after it that a real element type uses would otherwise
/// index past the end of the dispatch table.
template <typename... Types>
constexpr bool AllElementTypesIndexInsideTable(entt::type_list<Types...>) {
  return ((static_cast<size_t>(Types::Type) < kElementTypeCount) && ...);
}

static_assert(AllElementTypesIndexInsideTable(AllSVGElements()),
              "An SVG element type's ElementType is outside the range kElementTypeCount covers. "
              "Update kElementTypeCount in ElementType.h to match the last enumerator.");

/**
 * Build the \ref ElementType-indexed table of attribute handlers.
 *
 * Entries are filled in type-list order and an already-occupied slot is left alone, so a type
 * appearing earlier in \ref AllSVGElements wins - the same element a linear "first type whose
 * `Type` matches" scan would have selected. Types are left null when nothing claims them (\ref
 * ElementType::Unknown, and any enumerator no element type in the list uses), and the caller falls
 * back to the shared handler for those.
 */
template <typename... Types>
constexpr std::array<ParseAttributeFn, kElementTypeCount> BuildParseAttributeTable(
    entt::type_list<Types...>) {
  std::array<ParseAttributeFn, kElementTypeCount> table = {};
  (
      [&table]<typename ElementT>() {
        ParseAttributeFn& slot = table[static_cast<size_t>(ElementT::Type)];
        if (slot == nullptr) {
          slot = &ParseAttributeAs<ElementT>;
        }
      }.template operator()<Types>(),
      ...);
  return table;
}

/// Attribute handler for each element type, replacing a linear walk of the element type list that
/// re-read the element's runtime type at every step.
constexpr std::array<ParseAttributeFn, kElementTypeCount> kParseAttributeTable =
    BuildParseAttributeTable(AllSVGElements());

}  // namespace

std::optional<ParseDiagnostic> AttributeParser::ParseAndSetAttribute(
    SVGParserContext& context, SVGElement& element, const XMLQualifiedNameRef& name,
    std::string_view value) noexcept {
  const size_t typeIndex = static_cast<size_t>(element.type());
  const ParseAttributeFn parseFn =
      typeIndex < kParseAttributeTable.size() ? kParseAttributeTable[typeIndex] : nullptr;
  return parseFn != nullptr ? parseFn(context, element, name, value)
                            : ParseAttributeGeneric(context, element, name, value);
}

std::optional<ParseDiagnostic> AttributeParser::ApplyParsedAttribute(
    SVGElement& element, const XMLQualifiedNameRef& name, std::string_view value,
    bool* consumedAsPresentationAttribute) {
  return element.setAttributeWithRouter(name, value, consumedAsPresentationAttribute, nullptr);
}

void AttributeParser::RemoveParsedAttribute(SVGElement& element, const XMLQualifiedNameRef& name) {
  element.removeAttributeWithRouter(name, nullptr);
}

components::TypedFilterAttributeEdit AttributeParser::ReapplyTypedFilterAttributes(
    SVGParserContext& context, SVGElement& element, const XMLQualifiedNameRef& editedName) {
  if (!IsFilterElementFamily(element.type())) {
    return {};
  }
  const EntityHandle handle = element.entityHandle();
  const bool wasTypedOnly = context.typedAttributesOnly();
  context.setTypedAttributesOnly(true);
  (void)context.takeCommonAttributeFallThrough();

  const auto* image = handle.try_get<components::ImageComponent>();
  const RcString previousImageHref = image ? image->href : RcString();

  // Every type-specific filter attribute takes its default when its value is empty or does not
  // parse, so parsing only the edited attribute, with an empty value once it is removed, gives it
  // the state a fresh parse would and leaves every other value alone, including values set through
  // the typed setters, which do not write attributes.
  const std::optional<RcString> editedValue = element.getAttribute(editedName);
  const std::size_t warningsBefore = context.warnings().size();
  std::optional<ParseDiagnostic> diagnostic =
      ParseAndSetAttribute(context, element, editedName,
                           editedValue ? std::string_view(*editedValue) : std::string_view());
  // A name the type-specific parser does not recognize reaches the common attribute path, which
  // the typed-only mode records instead of applying; such an edit changes no typed state.
  const bool typed = !context.takeCommonAttributeFallThrough();
  context.setTypedAttributesOnly(wasTypedOnly);
  if (!typed) {
    return {};
  }

  components::TypedFilterAttributeEdit result{.typed = true};
  // A value the parser rejects with a warning, such as an unparsable length, takes the default
  // like a fresh parse does; the edit still applies, so report that warning as a warning. A
  // removal parses an empty value, whose warnings mean nothing to the caller.
  if (editedValue.has_value()) {
    if (diagnostic.has_value()) {
      result.diagnostic = std::move(diagnostic);
    } else if (context.warnings().size() > warningsBefore) {
      result.diagnostic = context.warnings()[warningsBefore];
      result.diagnostic->severity = DiagnosticSeverity::Warning;
    }
  }

  const auto* edited = handle.try_get<components::ImageComponent>();
  if ((edited ? edited->href : RcString()) != previousImageHref) {
    handle.remove<components::LoadedImageComponent, components::LoadedSVGImageComponent>();
  }
  return result;
}

}  // namespace donner::svg::parser
