#pragma once
/// @file

#include <map>

#include "donner/base/EcsRegistry.h"  // For EntityHandle
#include "donner/base/ParseResult.h"
#include "donner/base/SmallVector.h"
#include "donner/css/Color.h"
#include "donner/css/Declaration.h"
#include "donner/svg/core/ClipRule.h"
#include "donner/svg/core/ColorInterpolationFilters.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/core/CursorType.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/core/DominantBaseline.h"
#include "donner/base/FillRule.h"
#include "donner/svg/core/FontStretch.h"
#include "donner/svg/core/FontStyle.h"
#include "donner/svg/core/FontVariant.h"
#include "donner/svg/core/Isolation.h"
#include "donner/svg/core/MixBlendMode.h"
#include "donner/svg/core/Overflow.h"
#include "donner/svg/core/PointerEvents.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/core/TextAnchor.h"
#include "donner/svg/core/TextDecoration.h"
#include "donner/svg/core/TransformOrigin.h"
#include "donner/svg/core/Visibility.h"
#include "donner/svg/core/WritingMode.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/properties/PropertyParsing.h"  // IWYU pragma: keep, used for parser::UnparsedProperty

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
 * | `overflow`   | \ref overflow | `visible` |
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
 * | `mask` | \ref mask | `none` |
 * | `pointer-events` | \ref pointerEvents | `auto` |
 * | `cursor` | \ref cursor | `auto` |
 * | `marker-start` | \ref markerStart | `none` |
 * | `marker-mid` | \ref markerMid | `none` |
 * | `marker-end` | \ref markerEnd | `none` |
 * | `font-family` | \ref fontFamily | `serif` |
 * | `font-size` | \ref fontSize | `16px` |
 * | `text-anchor` | \ref textAnchor | `start` |
 * | `text-decoration` | \ref textDecoration | `none` |
 * | `dominant-baseline` | \ref dominantBaseline | `auto` |
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

  /// `overflow` property, which determines how content that overflows the element's box is handled.
  /// Defaults to `visible`.
  Property<Overflow> overflow{"overflow",
                              []() -> std::optional<Overflow> { return Overflow::Visible; }};

  /// `transform-origin` property, which sets the origin for transformations.
  Property<TransformOrigin> transformOrigin{
      "transform-origin", []() -> std::optional<TransformOrigin> {
        return TransformOrigin{Lengthd(50, Lengthd::Unit::Percent),
                               Lengthd(50, Lengthd::Unit::Percent)};
      }};

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
  // Clipping and masking
  //

  /// `clip-path` property, which determines the shape of the element's clipping region. Defaults to
  /// none.
  Property<Reference, PropertyCascade::None> clipPath{
      "clip-path", []() -> std::optional<Reference> { return std::nullopt; }};

  /// `clip-rule` property, which determines how the interior of the element is filled in the case
  /// of overlapping shapes. Defaults to \ref ClipRule::NonZero.
  Property<ClipRule, PropertyCascade::Inherit> clipRule{
      "clip-rule", []() -> std::optional<ClipRule> { return ClipRule::NonZero; }};

  /// `mask` property, which determines the shape of the element's clipping region. Defaults to
  /// none.
  Property<Reference, PropertyCascade::None> mask{
      "mask", []() -> std::optional<Reference> { return std::nullopt; }};

  //
  // Filter
  //

  /// `filter` property, which determines the filter effect(s) to apply to the element. Defaults to
  /// an empty list (no filter). Supports CSS filter function lists like `blur(5px) grayscale()`.
  Property<std::vector<FilterEffect>> filter{
      "filter",
      []() -> std::optional<std::vector<FilterEffect>> { return std::vector<FilterEffect>(); }};

  /// `color-interpolation-filters` property, which determines the color space for filter
  /// operations. Defaults to \ref ColorInterpolationFilters::LinearRGB. Inherited.
  Property<ColorInterpolationFilters, PropertyCascade::Inherit> colorInterpolationFilters{
      "color-interpolation-filters",
      []() -> std::optional<ColorInterpolationFilters> {
        return ColorInterpolationFilters::LinearRGB;
      }};

  //
  // Interaction
  //

  /// `pointer-events` property, which determines how the element responds to pointer events (such
  /// as clicks or hover). Defaults to \ref PointerEvents::VisiblePainted.
  Property<PointerEvents, PropertyCascade::Inherit> pointerEvents{
      "pointer-events",
      []() -> std::optional<PointerEvents> { return PointerEvents::VisiblePainted; }};

  /// `cursor` property, which defines the mouse cursor to display when hovering over the element.
  /// Defaults to \ref CursorType::Auto. Inherited.
  Property<CursorType, PropertyCascade::Inherit> cursor{
      "cursor", []() -> std::optional<CursorType> { return CursorType::Auto; }};

  //
  // Markers
  //

  /// `marker-start` property, which determines the marker to be drawn at the start of the path.
  Property<Reference, PropertyCascade::Inherit> markerStart{
      "marker-start", []() -> std::optional<Reference> { return std::nullopt; }};

  /// `marker-mid` property, which determines the marker to be drawn at the middle of the path.
  Property<Reference, PropertyCascade::Inherit> markerMid{
      "marker-mid", []() -> std::optional<Reference> { return std::nullopt; }};

  /// `marker-end` property, which determines the marker to be drawn at the end of the path.
  Property<Reference, PropertyCascade::Inherit> markerEnd{
      "marker-end", []() -> std::optional<Reference> { return std::nullopt; }};

  /// `font-family` property, which determines the font family for text content. Inherited.
  Property<SmallVector<RcString, 1>, PropertyCascade::Inherit> fontFamily{
      "font-family", []() -> std::optional<SmallVector<RcString, 1>> {
        return SmallVector<RcString, 1>{RcString("serif")};
      }};

  /// `font-size` property, which determines the font size for text content. Inherited.
  /// Initial value is `medium`, which maps to the UA default font size (12px).
  Property<Lengthd, PropertyCascade::Inherit> fontSize{
      "font-size", []() -> std::optional<Lengthd> { return Lengthd(12, Lengthd::Unit::Px); }};

  /// Sentinel value for the `bolder` relative keyword, resolved during cascade.
  static constexpr int kFontWeightBolder = -1;
  /// Sentinel value for the `lighter` relative keyword, resolved during cascade.
  static constexpr int kFontWeightLighter = -2;

  /// `font-weight` property, which determines the font weight (boldness) for text content.
  /// Inherited. Value is a numeric weight (100-900), where 400=normal and 700=bold.
  /// The relative keywords `bolder`/`lighter` are stored as sentinel values and resolved
  /// by \ref resolveFontWeight() during style cascade.
  Property<int, PropertyCascade::Inherit> fontWeight{"font-weight",
                                                     []() -> std::optional<int> { return 400; }};

  /// `font-style` property, which determines the style (normal/italic/oblique) for text content.
  /// Inherited. Defaults to \ref FontStyle::Normal.
  Property<FontStyle, PropertyCascade::Inherit> fontStyle{
      "font-style", []() -> std::optional<FontStyle> { return FontStyle::Normal; }};

  /// Sentinel value for the `narrower` relative keyword, resolved during cascade.
  static constexpr int kFontStretchNarrower = -1;
  /// Sentinel value for the `wider` relative keyword, resolved during cascade.
  static constexpr int kFontStretchWider = -2;

  /// `font-stretch` property, which determines the width (condensed/expanded) for text content.
  /// Inherited. Stored as the underlying integer of \ref FontStretch for sentinel support.
  /// Defaults to \ref FontStretch::Normal (5).
  Property<int, PropertyCascade::Inherit> fontStretch{
      "font-stretch", []() -> std::optional<int> { return static_cast<int>(FontStretch::Normal); }};

  /// `font-variant` shorthand property (SVG 1.1 subset: normal | small-caps).
  /// Inherited. Defaults to \ref FontVariant::Normal.
  Property<FontVariant, PropertyCascade::Inherit> fontVariant{
      "font-variant", []() -> std::optional<FontVariant> { return FontVariant::Normal; }};

  /// `text-anchor` property, which determines the alignment of text relative to its anchor point.
  /// Inherited. Defaults to \ref TextAnchor::Start.
  Property<TextAnchor, PropertyCascade::Inherit> textAnchor{
      "text-anchor", []() -> std::optional<TextAnchor> { return TextAnchor::Start; }};

  /// `text-decoration` property, which determines decoration lines drawn on text.
  /// Not inherited. Defaults to \ref TextDecoration::None.
  Property<TextDecoration> textDecoration{
      "text-decoration", []() -> std::optional<TextDecoration> { return TextDecoration::None; }};

  /// `dominant-baseline` property, which determines the baseline alignment for text.
  /// Not inherited. Defaults to \ref DominantBaseline::Auto.
  Property<DominantBaseline> dominantBaseline{
      "dominant-baseline",
      []() -> std::optional<DominantBaseline> { return DominantBaseline::Auto; }};

  /// `letter-spacing` property, extra spacing between characters. Inherited.
  /// "normal" maps to 0. Defaults to 0 (normal).
  Property<Lengthd, PropertyCascade::Inherit> letterSpacing{
      "letter-spacing", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  /// `word-spacing` property, extra spacing between words (U+0020 space characters). Inherited.
  /// "normal" maps to 0. Defaults to 0 (normal).
  Property<Lengthd, PropertyCascade::Inherit> wordSpacing{
      "word-spacing", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  /// `baseline-shift` property. Shifts the dominant baseline of the element.
  /// Not inherited. "baseline" = 0, "sub"/"super" map to em-relative offsets.
  /// Positive = shift up (per CSS). Stored as Lengthd.
  Property<Lengthd> baselineShift{
      "baseline-shift", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  /// `alignment-baseline` property. Specifies how an inline element aligns with its parent's
  /// baseline. Uses the same enum as dominant-baseline. Not inherited.
  Property<DominantBaseline> alignmentBaseline{
      "alignment-baseline",
      []() -> std::optional<DominantBaseline> { return DominantBaseline::Auto; }};

  /// `writing-mode` property, controlling text flow direction. Inherited.
  Property<WritingMode, PropertyCascade::Inherit> writingMode{
      "writing-mode", []() -> std::optional<WritingMode> { return WritingMode::HorizontalTb; }};

  /// `mix-blend-mode` property. Controls how an element composites with its backdrop.
  /// Not inherited. Defaults to Normal (SourceOver).
  Property<MixBlendMode> mixBlendMode{
      "mix-blend-mode", []() -> std::optional<MixBlendMode> { return MixBlendMode::Normal; }};

  /// `isolation` property. Forces creation of a new stacking context.
  /// Not inherited. Defaults to Auto.
  Property<Isolation> isolation{"isolation",
                                []() -> std::optional<Isolation> { return Isolation::Auto; }};

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
    return std::forward_as_tuple(
        color, display, opacity, visibility, overflow, transformOrigin, fill, fillRule, fillOpacity,
        stroke, strokeOpacity, strokeWidth, strokeLinecap, strokeLinejoin, strokeMiterlimit,
        strokeDasharray, strokeDashoffset, clipPath, clipRule, mask, filter,
        colorInterpolationFilters, pointerEvents, cursor, markerStart, markerMid, markerEnd,
        fontFamily, fontSize, fontWeight, fontStyle, fontStretch, fontVariant, textAnchor,
        textDecoration, dominantBaseline, writingMode, letterSpacing, wordSpacing, baselineShift,
        alignmentBaseline, mixBlendMode, isolation);
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
   * Return the number of properties set within the PropertyRegistry.
   */
  size_t numPropertiesSet() const;

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
   * Resolve font-size from relative units (em, %, ex) to absolute pixels. Must be called after CSS
   * cascade with the parent's computed font-size, since font-size percentages and em units resolve
   * against the parent (not the viewBox).
   *
   * @param parentFontSizePx The parent element's computed font-size in pixels. Use 16.0 (CSS
   *   initial value) for the root element.
   */
  void resolveFontSize(double parentFontSizePx);

  /**
   * Resolve relative font-weight keywords (`bolder`/`lighter`) against the parent's
   * computed font-weight.
   *
   * @param parentFontWeight The parent element's computed font-weight (100-900). Use 400 for root.
   */
  void resolveFontWeight(int parentFontWeight);

  /**
   * Resolve relative font-stretch keywords (`narrower`/`wider`) against the parent's
   * computed font-stretch value.
   *
   * @param parentFontStretch The parent element's computed font-stretch (as int). Use
   *   static_cast<int>(FontStretch::Normal) for root.
   */
  void resolveFontStretch(int parentFontStretch);

  /**
   * Parse a single declaration, adding it to the property registry.
   *
   * @param declaration Declaration to parse.
   * @param specificity Specificity of the declaration.
   * @return Error if the declaration had errors parsing or the property is not supported.
   */
  std::optional<ParseDiagnostic> parseProperty(const css::Declaration& declaration,
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
   * Parse a common presentation attribute (CSS properties like fill, stroke, etc.) or `transform`.
   *
   * This handles properties stored in PropertyRegistry and the `transform` attribute. For
   * element-specific presentation attributes (like `cx` on circles), use
   * \ref parser::ParsePresentationAttribute separately.
   *
   * @see https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes
   *
   * @param name Name of the attribute.
   * @param value Value of the attribute, parsed as a CSS value.
   * @param handle Entity handle, needed for `transform` attribute parsing.
   * @return true if the attribute was parsed successfully, false if not recognized, or a \ref
   * ParseDiagnostic if parsing failed.
   */
  ParseResult<bool> parsePresentationAttribute(std::string_view name, std::string_view value,
                                               EntityHandle handle = EntityHandle());

  /**
   * Returns true if a presentation attribute participates in inheritance.
   *
   * This uses the underlying property cascade metadata instead of a hand-maintained list in DOM
   * mutation code.
   *
   * @param name Presentation attribute name.
   * @return True if the property inherits to descendants.
   */
  static bool isPresentationAttributeInherited(std::string_view name);

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
