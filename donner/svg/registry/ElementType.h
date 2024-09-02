#pragma once
/// @file

#include <ostream>

namespace donner::svg {

/**
 * SVG element types, corresponds to each XML element name, such as \ref xml_circle, \ref xml_svg,
 * \ref xml_rect, etc.
 */
enum class ElementType {
  Circle,          //!< \ref xml_circle
  ClipPath,        //!< \ref xml_clipPath
  Defs,            //!< \ref xml_defs
  Ellipse,         //!< \ref xml_ellipse
  FeGaussianBlur,  //!< \ref xml_feGaussianBlur
  Filter,          //!< \ref xml_filter
  G,               //!< \ref xml_g
  Image,           //!< \ref xml_image
  Line,            //!< \ref xml_line
  LinearGradient,  //!< \ref xml_linearGradient
  Mask,            //!< \ref xml_mask
  Path,            //!< \ref xml_path
  Pattern,         //!< \ref xml_pattern
  Polygon,         //!< \ref xml_polygon
  Polyline,        //!< \ref xml_polyline
  RadialGradient,  //!< \ref xml_radialGradient
  Rect,            //!< \ref xml_rect
  Stop,            //!< \ref xml_stop, for gradient stops.
  Style,           //!< \ref xml_style
  SVG,             //!< \ref xml_svg, SVG root element.
  Unknown,         //!< For unknown elements.
  Use,             //!< \ref xml_use
};

/// Ostream output operator for \ref ElementType, outputs the element name.
std::ostream& operator<<(std::ostream& os, ElementType type);

/**
 * Converts a \ref ElementType runtime value to a compile-time value, allowing conditional behavior
 * for different element types with `constexpr`. Takes the runtime value and invokes a function with
 * \c std::integral_constant<ElementType> as a parameter.
 *
 * Example 1:
 * ```
 * SVGElement element = ...;
 * ToConstexpr<void>(element.type(), [&](auto elementType) {
 *   if constexpr (std::is_same_v<elementType(), ElementType::Circle>) {
 *     // Handle circle element.
 *     return ParseCircleAttributes(element);
 *   }
 * }
 * ```
 *
 * Example 2:
 * ```
 * template <ElementType Type>
 * ParseResult<bool> myFunction();
 *
 * ElementType type = element.type();
 * return ToConstexpr<ParseResult<bool>>(type, [&](auto elementType) {
 *    return ParsePresentationAttribute<elementType()>(element, name, value);
 *});
 * ```
 *
 * @tparam ReturnType Return type of the function.
 * @tparam FnT Function type, with signature `ReturnType(std::integral_constant<ElementType>)`.
 * @param type Runtime element type.
 * @param fn Function to call with the compile-time element type, with signature
 *`ReturnType(std::integral_constant<ElementType>)`.
 */
template <typename ReturnType, typename FnT>
ReturnType ToConstexpr(ElementType type, FnT fn) {
  switch (type) {
    case ElementType::Circle: return fn(std::integral_constant<ElementType, ElementType::Circle>());
    case ElementType::ClipPath:
      return fn(std::integral_constant<ElementType, ElementType::ClipPath>());
    case ElementType::Defs: return fn(std::integral_constant<ElementType, ElementType::Defs>());
    case ElementType::Ellipse:
      return fn(std::integral_constant<ElementType, ElementType::Ellipse>());
    case ElementType::FeGaussianBlur:
      return fn(std::integral_constant<ElementType, ElementType::FeGaussianBlur>());
    case donner::svg::ElementType::Filter:
      return fn(std::integral_constant<ElementType, ElementType::Filter>());
    case ElementType::G: return fn(std::integral_constant<ElementType, ElementType::G>());
    case ElementType::Image: return fn(std::integral_constant<ElementType, ElementType::Image>());
    case ElementType::Line: return fn(std::integral_constant<ElementType, ElementType::Line>());
    case ElementType::LinearGradient:
      return fn(std::integral_constant<ElementType, ElementType::LinearGradient>());
    case ElementType::Mask: return fn(std::integral_constant<ElementType, ElementType::Mask>());
    case ElementType::Path: return fn(std::integral_constant<ElementType, ElementType::Path>());
    case ElementType::Pattern:
      return fn(std::integral_constant<ElementType, ElementType::Pattern>());
    case ElementType::Polygon:
      return fn(std::integral_constant<ElementType, ElementType::Polygon>());
    case ElementType::Polyline:
      return fn(std::integral_constant<ElementType, ElementType::Polyline>());
    case ElementType::RadialGradient:
      return fn(std::integral_constant<ElementType, ElementType::RadialGradient>());
    case ElementType::Rect: return fn(std::integral_constant<ElementType, ElementType::Rect>());
    case ElementType::Stop: return fn(std::integral_constant<ElementType, ElementType::Stop>());
    case ElementType::Style: return fn(std::integral_constant<ElementType, ElementType::Style>());
    case ElementType::SVG: return fn(std::integral_constant<ElementType, ElementType::SVG>());
    case ElementType::Unknown:
      return fn(std::integral_constant<ElementType, ElementType::Unknown>());
    case ElementType::Use: return fn(std::integral_constant<ElementType, ElementType::Use>());
  };
}

}  // namespace donner::svg
