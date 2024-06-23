#pragma once
/// @file

#include "donner/base/Box.h"
#include "donner/base/parser/ParseResult.h"
#include "donner/css/Color.h"
#include "donner/css/Declaration.h"
#include "donner/css/Stylesheet.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/properties/PropertyParsing.h"
#include "donner/svg/registry/Registry.h"  // For EntityHandle

namespace donner::svg {

namespace detail {

template <typename T, std::size_t... I>
auto tuple_remove_const(T tuple, std::index_sequence<I...>) {
  using TupleType = std::remove_reference_t<T>;

  // Using const_cast to transform each tuple element from 'const T&' to 'T&'
  return std::forward_as_tuple(
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      (const_cast<
          std::remove_const_t<std::remove_reference_t<std::tuple_element_t<I, TupleType>>>&>(
          std::get<I>(tuple)))...);
}

template <typename... Args>
auto as_mutable(const std::tuple<Args...>& tuple) {
  auto result = tuple_remove_const(tuple, std::index_sequence_for<Args...>{});
  static_assert(!std::is_const_v<std::remove_reference_t<decltype(std::get<0>(result))>>,
                "Expected non-const reference");
  return result;
}

}  // namespace detail

class PropertyRegistry;
using PropertyParseFn = std::optional<parser::ParseError> (*)(
    PropertyRegistry& registry, const parser::PropertyParseFnParams& params);

class PropertyRegistry {
public:
  Property<css::Color, PropertyCascade::Inherit> color{
      "color", []() -> std::optional<css::Color> { return css::Color(css::RGBA(0, 0, 0, 0xFF)); }};
  Property<Display> display{"display", []() -> std::optional<Display> { return Display::Inline; }};
  Property<double> opacity{"opacity", []() -> std::optional<double> { return 1.0; }};
  Property<Visibility, PropertyCascade::Inherit> visibility{
      "visibility", []() -> std::optional<Visibility> { return Visibility::Visible; }};

  // Fill
  Property<PaintServer, PropertyCascade::PaintInherit> fill{
      "fill", []() -> std::optional<PaintServer> {
        return PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0)));
      }};
  Property<FillRule, PropertyCascade::Inherit> fillRule{
      "fill-rule", []() -> std::optional<FillRule> { return FillRule::NonZero; }};
  Property<double, PropertyCascade::Inherit> fillOpacity{
      "fill-opacity", []() -> std::optional<double> { return 1.0; }};

  // Stroke
  Property<PaintServer, PropertyCascade::PaintInherit> stroke{
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

  // Clip paths
  Property<Reference, PropertyCascade::None> clipPath{
      "clip-path", []() -> std::optional<Reference> { return std::nullopt; }};

  // Filter
  Property<FilterEffect> filter{
      "filter", []() -> std::optional<FilterEffect> { return FilterEffect::None(); }};

  /// Properties which don't have specific listings above, which are stored as raw css declarations.
  std::map<RcString, parser::UnparsedProperty> unparsedProperties;

  /// Constructor.
  PropertyRegistry();

  /// Destructor.
  ~PropertyRegistry();

  /// Copy and move constructors.
  PropertyRegistry(const PropertyRegistry&);
  PropertyRegistry(PropertyRegistry&&) noexcept;
  PropertyRegistry& operator=(const PropertyRegistry&);
  PropertyRegistry& operator=(PropertyRegistry&&) noexcept;

  /**
   * Return a tuple of all properties within the PropertyRegistry.
   *
   * To get the size of the tuple, use \ref numProperties().
   */
  auto allProperties() const {
    return std::forward_as_tuple(color, display, opacity, visibility, fill, fillRule, fillOpacity,
                                 stroke, strokeOpacity, strokeWidth, strokeLinecap, strokeLinejoin,
                                 strokeMiterlimit, strokeDasharray, strokeDashoffset, clipPath,
                                 filter);
  }

  /**
   * Return a mutable tuple of all properties within the PropertyRegistry.
   *
   * @see allProperties()
   */
  auto allPropertiesMutable() { return detail::as_mutable(allProperties()); }

  /**
   * Return the size of the tuple returned by \ref allProperties().
   */
  static constexpr size_t numProperties() {
    // If this is at class scope, it fails with a compiler error: "function with deduced return type
    // cannot be used before it is defined".
    using PropertiesTuple =
        std::invoke_result_t<decltype(&PropertyRegistry::allProperties), PropertyRegistry>;
    return std::tuple_size_v<PropertiesTuple>;
  }

  /**
   * Calls a compile time functor for each property in the registry.
   *
   * Example:
   * ```
   * auto properties = allProperties();
   * forEachProperty<0, numProperties()>([&properties](auto i) {
   *   auto& property = std::get<i>(properties);
   * }
   * ```
   *
   * @tparam Start Index of the current property to call.
   * @tparam End Total number of properties in the registry.
   * @tparam F Type of the functor object, with signature `void(std::integral_constant<size_t>)`.
   * @param f Functor instance.
   */
  template <size_t Start, size_t End, class F>
  static constexpr void forEachProperty(const F& f) {
    if constexpr (Start < End) {
      f(std::integral_constant<size_t, Start>{});
      forEachProperty<Start + 1, End>(f);
    }
  }

  /**
   * Inherit the value of each element in the stylesheet.
   *
   * @param parent PropertyRegistry from this element's direct parent, where properties will be
   *   inherited from.
   * @param options Options for how to inherit properties, which can be used to skip inheritance for
   *   a category of properties.
   */
  [[nodiscard]] PropertyRegistry inheritFrom(
      const PropertyRegistry& parent,
      PropertyInheritOptions options = PropertyInheritOptions::All) const;

  /**
   * Calls \ref Property::resolveUnits for each property in the registry, which converts the units
   * of this property to pixel-relative values, if it contains a value which is relative such as a
   * font- or viewport-relative length.
   *
   * @param viewbox The viewbox to use for resolving relative lengths.
   * @param fontMetrics The font metrics to use for resolving relative lengths.
   */
  void resolveUnits(const Boxd& viewbox, const FontMetrics& fontMetrics);

  /**
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @param specificity Specificity of the declaration.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<parser::ParseError> parseProperty(const css::Declaration& declaration,
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
   * @return true if the element supports this attribute and it was parsed successfully, or a \ref
   *   ParseError if parsing failed.
   */
  parser::ParseResult<bool> parsePresentationAttribute(
      std::string_view name, std::string_view value, std::optional<ElementType> type = std::nullopt,
      EntityHandle handle = EntityHandle());

  /**
   * Ostream output operator, for debugging which outputs a human-readable representation of all of
   * the properties.
   *
   * Example output:
   * ```
   * PropertyRegistry {
   *   color: Color(rgba(0, 255, 0, 255)) (set) @ Specificity(0, 0, 0)
   * }
   * ```
   *
   * @param os Output stream.
   * @param registry PropertyRegistry to output.
   */
  friend std::ostream& operator<<(std::ostream& os, const PropertyRegistry& registry);
};

}  // namespace donner::svg
