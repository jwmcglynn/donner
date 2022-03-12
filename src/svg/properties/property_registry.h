#pragma once

#include <span>

#include "src/base/box.h"
#include "src/base/parser/parse_result.h"
#include "src/css/color.h"
#include "src/css/declaration.h"
#include "src/css/stylesheet.h"
#include "src/svg/properties/paint_server.h"
#include "src/svg/properties/property.h"
#include "src/svg/properties/property_parsing.h"
#include "src/svg/registry/registry.h"  // For EntityHandle

namespace donner::svg {

class PropertyRegistry;
using PropertyParseFn = std::optional<ParseError> (*)(PropertyRegistry& registry,
                                                      const PropertyParseFnParams& params);

class PropertyRegistry {
public:
  Property<css::Color, PropertyCascade::Inherit> color{
      "color", []() -> std::optional<css::Color> { return css::Color(css::RGBA(0, 0, 0, 0xFF)); }};
  Property<Display> display{"display", []() -> std::optional<Display> { return Display::Inline; }};
  Property<double> opacity{"opacity", []() -> std::optional<double> { return 1.0; }};
  Property<Visibility, PropertyCascade::Inherit> visibility{
      "visibility", []() -> std::optional<Visibility> { return Visibility::Visible; }};

  // Fill
  Property<PaintServer, PropertyCascade::Inherit> fill{
      "fill", []() -> std::optional<PaintServer> {
        return PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0)));
      }};
  Property<FillRule, PropertyCascade::Inherit> fillRule{
      "fill-rule", []() -> std::optional<FillRule> { return FillRule::NonZero; }};
  Property<double, PropertyCascade::Inherit> fillOpacity{
      "fill-opacity", []() -> std::optional<double> { return 1.0; }};

  // Stroke
  Property<PaintServer, PropertyCascade::Inherit> stroke{
      "stroke", []() -> std::optional<PaintServer> { return PaintServer::None(); }};
  Property<double, PropertyCascade::Inherit> strokeOpacity{
      "stroke-opacity", []() -> std::optional<double> { return 1.0; }};
  Property<Lengthd, PropertyCascade::Inherit> strokeWidth{
      "stroke-width", []() -> std::optional<Lengthd> { return Lengthd(1, Lengthd::Unit::None); }};
  Property<StrokeLinecap, PropertyCascade::Inherit> strokeLinecap{
      "stroke-linecap", []() -> std::optional<StrokeLinecap> { return StrokeLinecap::Butt; }};
  Property<StrokeLinejoin, PropertyCascade::Inherit> strokeLinejoin{
      "stroke-linejoin", []() -> std::optional<StrokeLinejoin> { return StrokeLinejoin::Miter; }};
  Property<double, PropertyCascade::Inherit> strokeMiterlimit{
      "stroke-miterlimit", []() -> std::optional<double> { return 4.0; }};
  Property<StrokeDasharray, PropertyCascade::Inherit> strokeDasharray{
      "stroke-dasharray", []() -> std::optional<StrokeDasharray> { return std::nullopt; }};
  Property<Lengthd, PropertyCascade::Inherit> strokeDashoffset{
      "stroke-dashoffset",
      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  std::map<RcString, UnparsedProperty> unparsedProperties;

  /**
   * Return a tuple of all properties within the PropertyRegistry.
   */
  auto allProperties() {
    return std::forward_as_tuple(color, display, opacity, visibility, fill, fillRule, fillOpacity,
                                 stroke, strokeOpacity, strokeWidth, strokeLinecap, strokeLinejoin,
                                 strokeMiterlimit, strokeDasharray, strokeDashoffset);
  }

  static constexpr size_t numProperties() {
    // If this is at class scope, it fails with a compiler error: "function with deduced return type
    // cannot be used before it is defined".
    using PropertiesTuple =
        std::invoke_result_t<decltype(&PropertyRegistry::allProperties), PropertyRegistry>;
    return std::tuple_size_v<PropertiesTuple>;
  }

  template <size_t Start, size_t End, class F>
  static constexpr void forEachProperty(F&& f) {
    if constexpr (Start < End) {
      f(std::integral_constant<size_t, Start>{});
      forEachProperty<Start + 1, End>(f);
    }
  }

  /**
   * Inherit the value of each element in the stylesheet.
   */
  [[nodiscard]] PropertyRegistry inheritFrom(const PropertyRegistry& parent) const {
    PropertyRegistry result;
    result.unparsedProperties = unparsedProperties;  // Unparsed properties are not inherited.

    auto resultProperties = result.allProperties();
    const auto parentProperties = const_cast<PropertyRegistry&>(parent).allProperties();
    const auto selfProperties = const_cast<PropertyRegistry*>(this)->allProperties();

    forEachProperty<0, numProperties()>(
        [&resultProperties, parentProperties, selfProperties](auto i) {
          std::get<i.value>(resultProperties) =
              std::get<i.value>(selfProperties).inheritFrom(std::get<i.value>(parentProperties));
        });

    return result;
  }

  void resolveUnits(const Boxd& viewbox, const FontMetrics& fontMetrics) {
    std::apply([&viewbox, &fontMetrics](
                   auto&&... property) { (property.resolveUnits(viewbox, fontMetrics), ...); },
               allProperties());
  }

  /**
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @param specificity Specificity of the declaration.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<ParseError> parseProperty(const css::Declaration& declaration,
                                          css::Specificity specificity);

  /**
   * Parse a HTML/SVG style attribute, corresponding to a CSS <declaration-list>, ignoring any parse
   * errors or unsupported properties.
   *
   * @param str Input string.
   */
  void parseStyle(std::string_view str);

  /**
   * Parse a presentation attribute, which can contain a CSS value.
   *
   * @see https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
   *
   * @param name Name of the attribute.
   * @param value Value of the attribute, parsed as a CSS value.
   * @param type If set, parses additional presentation attributes for the given element type.
   * @param handle Entity handle to use for parsing additional attributes.
   * @return true if the attribute name was supported.
   */
  ParseResult<bool> parsePresentationAttribute(std::string_view name, std::string_view value,
                                               std::optional<ElementType> type = std::nullopt,
                                               EntityHandle handle = EntityHandle());

  friend std::ostream& operator<<(std::ostream& os, const PropertyRegistry& registry);
};

}  // namespace donner::svg
