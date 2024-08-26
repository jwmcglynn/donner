#pragma once
/// @file

#include "donner/base/parser/ParseResult.h"
#include "donner/css/Declaration.h"
#include "donner/css/Specificity.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::parser {

struct UnparsedProperty {
  css::Declaration declaration;
  css::Specificity specificity;
};

enum class PropertyParseBehavior {
  Default,
  AllowUserUnits,
};

struct PropertyParseFnParams {
  std::variant<std::string_view, std::span<const css::ComponentValue>> valueOrComponents;
  PropertyState explicitState = PropertyState::NotSet;
  css::Specificity specificity;
  /// For presentation attributes, values may be unitless, in which case they the spec says they are
  /// specified in "user units". See https://www.w3.org/TR/SVG2/types.html#syntax.
  PropertyParseBehavior parseBehavior = PropertyParseBehavior::Default;

  std::span<const css::ComponentValue> components() const;
  bool allowUserUnits() const { return parseBehavior == PropertyParseBehavior::AllowUserUnits; }

private:
  mutable std::optional<std::vector<css::ComponentValue>> parsedComponents_;
};

template <typename T, PropertyCascade kCascade, typename ParseCallbackFn>
std::optional<ParseError> Parse(const PropertyParseFnParams& params, ParseCallbackFn callbackFn,
                                Property<T, kCascade>* destination) {
  if (params.specificity < destination->specificity) {
    // Existing specificity is higher than the new one, so we don't need to parse.
    return std::nullopt;
  }

  // If the property is set to a built-in keyword, such as "inherit", the property has already been
  // parsed so we can just set based on the value of explicitState.
  if (params.explicitState != PropertyState::NotSet) {
    destination->set(params.explicitState, params.specificity);
    return std::nullopt;
  }

  auto result = callbackFn(params);
  if (result.hasError()) {
    // If there is a parse error, the CSS specification requires user agents to ignore the
    // declaration, and not modify the existing value.
    // See https://www.w3.org/TR/CSS2/syndata.html#ignore.
    return std::move(result.error());
  }

  destination->set(std::move(result.result()), params.specificity);
  return std::nullopt;
}

PropertyParseFnParams CreateParseFnParams(
    const css::Declaration& declaration, css::Specificity specificity,
    PropertyParseBehavior PropertyParseBehavior = PropertyParseBehavior::Default);

ParseResult<bool> ParseSpecialAttributes(PropertyParseFnParams& params, std::string_view name,
                                         std::optional<ElementType> type = std::nullopt,
                                         EntityHandle handle = EntityHandle());

/**
 * If the components contain only a single ident, returns an RcString for that ident's contents.
 *
 * @param components Component values, which should already be trimmed.
 * @return A string if the components contain a single ident, otherwise std::nullopt.
 */
std::optional<RcString> TryGetSingleIdent(std::span<const css::ComponentValue> components);

/**
 * Parse a `<length-percentage>`, which may optionally be set to "auto", in which case this returns
 * `std::nullopt`.
 *
 * @param components Component values, which should already be trimmed.
 * @param allowUserUnits Whether to allow unitless values, if this is a parse in the context of XML
 *   attributes.
 * @return Return an optional, which is set to `std::nullopt` if the value is "auto", or a Length,
 * or a parse error.
 */
ParseResult<std::optional<Lengthd>> ParseLengthPercentageOrAuto(
    std::span<const css::ComponentValue> components, bool allowUserUnits);

/**
 * Parse an `<alpha-value>`, defined by CSS Color:
 * https://www.w3.org/TR/css-color/#typedef-alpha-value
 *
 * ```
 * <alpha-value> = <number> | <percentage>
 * ```
 *
 * Where if a number is specified, it's represented with `1.0` being `100%`.
 *
 * @param components
 * @return ParseResult<double> Parsed alpha value as a double, in the range of [0, 1].
 */
ParseResult<double> ParseAlphaValue(std::span<const css::ComponentValue> components);

}  // namespace donner::svg::parser
