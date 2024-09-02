#pragma once
/// @file

#include "donner/base/parser/ParseResult.h"
#include "donner/css/Color.h"
#include "donner/css/Declaration.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/core/ClipRule.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/core/FillRule.h"
#include "donner/svg/core/PointerEvents.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/Visibility.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/properties/PropertyParsing.h"  // IWYU pragma: keep, used for parser::UnparsedProperty
#include "donner/svg/registry/Registry.h"           // For EntityHandle

namespace donner::svg {

namespace details {

/**
 * Helper function used by \ref as_mutable to convert a tuple of const-refs to mutable-refs, given a
 * tuple and index sequence for each element.
 *
 * @tparam T Tuple type.
 * @tparam I Index sequence type.
 * @param tuple Tuple to convert.
 */
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

/**
 * Converts a tuple containing const-refs to mutable-refs, e.g. `std::tuple<const T&...>` to
 * `std::tuple<T&...>`.
 *
 * @tparam Args Types of the tuple elements.
 * @param tuple Tuple to convert.
 */
template <typename... Args>
auto as_mutable(const std::tuple<Args...>& tuple) {
  auto result = tuple_remove_const(tuple, std::index_sequence_for<Args...>{});
  static_assert(!std::is_const_v<std::remove_reference_t<decltype(std::get<0>(result))>>,
                "Expected non-const reference");
  return result;
}

}  // namespace details

/**
 * Holds CSS properties for a single element. This class stores common properties which may be
 * applied to any element, plus \ref unparsedProperties which contains element-specific properties
 * which are applied if the element matches.
 *
 * For \ref unparsedProperties, presentation attributes specified in CSS such as `transform` will be
 * stored here until they can be applied to the element.
 *
 * # Supported properties
 *
 * | Property | Member | Default |
 * |----------|--------|---------|
 * | `color` | \ref color | `black` |
 * | `display` | \ref display | `inline` |
 * | `opacity` | \ref opacity | `1.0` |
 * | `visibility` | \ref visibility | `visible` |
 * | `fill` | \ref fill | `black` |
 * | `fill-rule` | \ref fillRule | `nonzero` |
 * | `fill-opacity` | \ref fillOpacity | `1.0` |
 * | `stroke` | \ref stroke | `none` |
 * | `stroke-opacity` | \ref strokeOpacity | `1.0` |
 * | `stroke-width` | \ref strokeWidth | `1.0` |
 * | `stroke-linecap` | \ref strokeLinecap | `butt` |
 * | `stroke-linejoin` | \ref strokeLinejoin | `miter` |
 * | `stroke-miterlimit` | \ref strokeMiterlimit | `4.0` |
 * | `stroke-dasharray` | \ref strokeDasharray | `none` |
 * | `stroke-dashoffset` | \ref strokeDashoffset | `0` |
 * | `clip-path` | \ref clipPath | `none` |
 * | `clip-rule` | \ref clipRule | `nonzero` |
 * | `filter` | \ref filter | `none` |
 * | `pointer-events` | \ref pointerEvents | `auto` |
 */
class PropertyRegistry {
public:
  /// `color` property, which stores the context color of the element. For painting shapes, use \ref
  /// stroke or \ref fill instead.
  Property<css::Color, PropertyCascade::Inherit> color{
      "color", []() -> std::optional<css::Color> { return css::Color(css::RGBA(0, 0, 0, 0xFF)); }};

  /// `display` property, which determines how the element is rendered. Set to \ref Display::None to
  /// hide the element.
  Property<Display> display{"display", []() -> std::optional<Display> { return Display::Inline; }};

  /// `opacity` property, which determines the opacity of the element. A value of 0.0 will make the
  /// element invisible.
  Property<double> opacity{"opacity", []() -> std::optional<double> { return 1.0; }};

  /// `visibility` property, which determines whether the element is visible. Set to \ref
  /// Visibility::Hidden to hide the element. Compared to \ref display with \ref Display::None,
  /// hiding the element will not remove it from the document, so it will still contribute to
  /// bounding boxes.
  Property<Visibility, PropertyCascade::Inherit> visibility{
      "visibility", []() -> std::optional<Visibility> { return Visibility::Visible; }};

  //
  // Fill
  //

  /// `fill` property, which determines the color of the element's interior. Defaults to black.
  Property<PaintServer, PropertyCascade::PaintInherit> fill{
      "fill", []() -> std::optional<PaintServer> {
        return PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0)));
      }};

  /// `fill-rule` property, which determines how the interior of the element is filled in the case
  /// of overlapping shapes. Defaults to \ref FillRule::NonZero.
  Property<FillRule, PropertyCascade::Inherit> fillRule{
      "fill-rule", []() -> std::optional<FillRule> { return FillRule::NonZero; }};

  /// `fill-opacity` property, which determines the opacity of the element's interior. Defaults
  /// to 1.0. A value of 0.0 will make the interior invisible.
  Property<double, PropertyCascade::Inherit> fillOpacity{
      "fill-opacity", []() -> std::optional<double> { return 1.0; }};

  //
  // Stroke
  //

  /// `stroke` property, which determines the color of the element's outline stroke. Defaults to
  /// none.
  Property<PaintServer, PropertyCascade::PaintInherit> stroke{
      "stroke", []() -> std::optional<PaintServer> { return PaintServer::None(); }};

  /// `stroke-opacity` property, which determines the opacity of the element's outline stroke.
  /// Defaults to 1.0. A value of 0.0 will make the outline invisible.
  Property<double, PropertyCascade::Inherit> strokeOpacity{
      "stroke-opacity", []() -> std::optional<double> { return 1.0; }};

  /// `stroke-width` property, which determines the width of the element's outline stroke. Defaults
  /// to 1.0.
  Property<Lengthd, PropertyCascade::Inherit> strokeWidth{
      "stroke-width", []() -> std::optional<Lengthd> { return Lengthd(1, Lengthd::Unit::None); }};

  /// `stroke-linecap` property, which determines the shape of the element's outline stroke at the
  /// ends of the path. Defaults to \ref StrokeLinecap::Butt.
  Property<StrokeLinecap, PropertyCascade::Inherit> strokeLinecap{
      "stroke-linecap", []() -> std::optional<StrokeLinecap> { return StrokeLinecap::Butt; }};

  /// `stroke-linejoin` property, which determines the shape of the element's outline stroke in
  /// between line segments. Defaults to \ref StrokeLinejoin::Miter.
  Property<StrokeLinejoin, PropertyCascade::Inherit> strokeLinejoin{
      "stroke-linejoin", []() -> std::optional<StrokeLinejoin> { return StrokeLinejoin::Miter; }};

  /// `stroke-miterlimit` property, which determines the limit of the ratio of the miter length to
  /// the stroke width. Defaults to 4.0.
  Property<double, PropertyCascade::Inherit> strokeMiterlimit{
      "stroke-miterlimit", []() -> std::optional<double> { return 4.0; }};

  /// `stroke-dasharray` property, which determines the pattern of dashes and gaps used to stroke
  /// paths.
  Property<StrokeDasharray, PropertyCascade::Inherit> strokeDasharray{
      "stroke-dasharray", []() -> std::optional<StrokeDasharray> { return std::nullopt; }};

  /// `stroke-dashoffset` property, which determines the distance into the dash pattern to start the
  /// stroke.
  Property<Lengthd, PropertyCascade::Inherit> strokeDashoffset{
      "stroke-dashoffset",
      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  //
  // Clipping
  //

  /// `clip-path` property, which determines the shape of the element's clipping region. Defaults to
  /// none.
  Property<Reference, PropertyCascade::None> clipPath{
      "clip-path", []() -> std::optional<Reference> { return std::nullopt; }};

  /// `clip-rule` property, which determines how the interior of the element is filled in the case
  /// of overlapping shapes. Defaults to \ref ClipRule::NonZero.
  Property<ClipRule, PropertyCascade::Inherit> clipRule{
      "clip-rule", []() -> std::optional<ClipRule> { return ClipRule::NonZero; }};

  //
  // Filter
  //

  /// `filter` property, which determines the filter effect to apply to the element. Defaults to
  /// none.
  Property<FilterEffect> filter{
      "filter", []() -> std::optional<FilterEffect> { return FilterEffect::None(); }};

  //
  // Interaction
  //

  /// `pointer-events` property, which determines how the element responds to pointer events (such
  /// as clicks or hover). Defaults to \ref PointerEvents::VisiblePainted.
  Property<PointerEvents, PropertyCascade::Inherit> pointerEvents{
      "pointer-events",
      []() -> std::optional<PointerEvents> { return PointerEvents::VisiblePainted; }};

  /// Properties which don't have specific listings above, which are stored as raw css
  /// declarations.
  std::map<RcString, parser::UnparsedProperty> unparsedProperties;

  /// Constructor.
  PropertyRegistry();

  /// Destructor.
  ~PropertyRegistry();

  // Copy and move constructors.
  /// Copy constructor.
  PropertyRegistry(const PropertyRegistry&);
  /// Move constructor.
  PropertyRegistry(PropertyRegistry&&) noexcept;
  /// Copy assignment operator.
  PropertyRegistry& operator=(const PropertyRegistry&);
  /// Move assignment operator.
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
                                 clipRule, filter, pointerEvents);
  }

  /**
   * Return a mutable tuple of all properties within the PropertyRegistry.
   *
   * @see allProperties()
   */
  auto allPropertiesMutable() { return details::as_mutable(allProperties()); }

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
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @param specificity Specificity of the declaration.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<parser::ParseError> parseProperty(const css::Declaration& declaration,
                                                  css::Specificity specificity);

  /**
   * Parse a SVG style attribute, and set the parsed values on this PropertyRegistry. Does not
   * clear existing properties, new ones are applied additively.
   *
   * Parses the string as a CSS "<declaration-list>", ignoring any parse errors or unsupported
   * properties
   *
   * @param str Input string from a style attribute, e.g. "fill: red; stroke: blue".
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
   *   donner::base::parser::ParseError if parsing failed.
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
