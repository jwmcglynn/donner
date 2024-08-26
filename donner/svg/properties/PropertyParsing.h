#pragma once
/// @file

#include "donner/base/parser/ParseResult.h"
#include "donner/css/Declaration.h"
#include "donner/css/Specificity.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::parser {

/**
 * Represents an unparsed property, which is a CSS property that is element-specific and needs to be
 * matched with the actual element before it can be parsed and applied. For example, the `transform`
 * property.
 */
struct UnparsedProperty {
  /// CSS declaration, e.g. "transform: translate(10px, 20px);". Contains the name and list of \ref
  /// css::ComponentValue for the value.
  css::Declaration declaration;

  /// Specificity of the declaration.
  css::Specificity specificity;
};

/**
 * Set the parse behavior for numbers. For properties set on the XML element, units can be omitted
 * and will be considered as "user units", which are equivalent to pixels. For properties set on the
 * CSS style attribute, units must be specified.
 *
 * When set to \ref AllowUserUnits, the parser will accept numbers without units, such as `15`.
 */
enum class PropertyParseBehavior {
  /// Require units for numbers, such as `15px`, with the exception of `0` which may be unitless.
  Default,
  /// Allow numbers without units, e.g. `15`.
  AllowUserUnits,
};

/**
 * Parameters for a property parse function.
 */
struct PropertyParseFnParams {
  /**
   * Create a new \ref PropertyParseFnParams from a declaration and specificity.
   *
   * @param declaration CSS declaration, e.g. "transform: translate(10px, 20px);".
   * @param specificity Specificity of the declaration.
   * @param parseBehavior Behavior for parsing numbers. See \ref PropertyParseBehavior.
   */
  static PropertyParseFnParams Create(
      const css::Declaration& declaration, css::Specificity specificity,
      PropertyParseBehavior parseBehavior = PropertyParseBehavior::Default);

  /// Property value, which may either be a string or list of \ref css::ComponentValue.
  std::variant<std::string_view, std::span<const css::ComponentValue>> valueOrComponents;

  /// Explicit state of the property, such as "inherit", "initial" or "unset". If this is \ref
  /// PropertyState::NotSet, ignore this field and parse \ref valueOrComponents.
  PropertyState explicitState = PropertyState::NotSet;

  /// Specificity of the property, used for inheritance.
  css::Specificity specificity;

  /// For presentation attributes, values may be unitless, in which case they the spec says they are
  /// specified in "user units". See https://www.w3.org/TR/SVG2/types.html#syntax.
  PropertyParseBehavior parseBehavior = PropertyParseBehavior::Default;

  /// Get the list of \ref css::ComponentValue for the property value.
  std::span<const css::ComponentValue> components() const;

  /// Returns true if user units are allowed for the property.
  bool allowUserUnits() const { return parseBehavior == PropertyParseBehavior::AllowUserUnits; }

private:
  /// Parsed list of \ref css::ComponentValue for the property value.
  mutable std::optional<std::vector<css::ComponentValue>> parsedComponents_;
};

/**
 * Parse a property value into a \ref Property.
 *
 * @tparam T The type of the property value.
 * @param params Parameters for the property parse function.
 * @param callbackFn Function to parse the property value.
 * @param destination The property to set the value on.
 */
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

/**
 * Parse special property attributes, currently used for `transform`.
 *
 * @param params Parameters for the property parse function.
 * @param name Name of the attribute to parse.
 * @param type Type of the element, if known.
 * @param handle Entity handle of the element, if known.
 */
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
