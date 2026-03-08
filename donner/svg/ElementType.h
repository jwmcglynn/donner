#pragma once
/// @file

#include <cstdint>
#include <ostream>

namespace donner::svg {

/**
 * SVG element types, corresponds to each XML element name, such as \ref xml_circle, \ref xml_svg,
 * \ref xml_rect, etc.
 */
enum class ElementType : uint8_t {
  Circle,          //!< \ref xml_circle
  ClipPath,        //!< \ref xml_clipPath
  Defs,            //!< \ref xml_defs
  Ellipse,         //!< \ref xml_ellipse
  FeBlend,         //!< \ref xml_feBlend
  FeColorMatrix,   //!< \ref xml_feColorMatrix
  FeComponentTransfer,  //!< \ref xml_feComponentTransfer
  FeComposite,          //!< \ref xml_feComposite
  FeConvolveMatrix,     //!< \ref xml_feConvolveMatrix
  FeDropShadow,         //!< \ref xml_feDropShadow
  FeFlood,              //!< \ref xml_feFlood
  FeFuncA,              //!< \ref xml_feFuncA
  FeFuncB,              //!< \ref xml_feFuncB
  FeFuncG,              //!< \ref xml_feFuncG
  FeFuncR,              //!< \ref xml_feFuncR
  FeGaussianBlur,  //!< \ref xml_feGaussianBlur
  FeMerge,         //!< \ref xml_feMerge
  FeMergeNode,     //!< \ref xml_feMergeNode
  FeMorphology,    //!< \ref xml_feMorphology
  FeOffset,        //!< \ref xml_feOffset
  FeTile,          //!< \ref xml_feTile
  FeTurbulence,    //!< \ref xml_feTurbulence
  Filter,          //!< \ref xml_filter
  G,               //!< \ref xml_g
  Image,           //!< \ref xml_image
  Line,            //!< \ref xml_line
  LinearGradient,  //!< \ref xml_linearGradient
  Marker,          //!< \ref xml_marker
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
  Symbol,          //!< \ref xml_symbol
  Text,            //!< \ref xml_text
  TSpan,           //!< \ref xml_tspan
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
    case ElementType::FeBlend:
      return fn(std::integral_constant<ElementType, ElementType::FeBlend>());
    case ElementType::FeColorMatrix:
      return fn(std::integral_constant<ElementType, ElementType::FeColorMatrix>());
    case ElementType::FeComponentTransfer:
      return fn(std::integral_constant<ElementType, ElementType::FeComponentTransfer>());
    case ElementType::FeComposite:
      return fn(std::integral_constant<ElementType, ElementType::FeComposite>());
    case ElementType::FeConvolveMatrix:
      return fn(std::integral_constant<ElementType, ElementType::FeConvolveMatrix>());
    case ElementType::FeDropShadow:
      return fn(std::integral_constant<ElementType, ElementType::FeDropShadow>());
    case ElementType::FeFlood:
      return fn(std::integral_constant<ElementType, ElementType::FeFlood>());
    case ElementType::FeFuncA:
      return fn(std::integral_constant<ElementType, ElementType::FeFuncA>());
    case ElementType::FeFuncB:
      return fn(std::integral_constant<ElementType, ElementType::FeFuncB>());
    case ElementType::FeFuncG:
      return fn(std::integral_constant<ElementType, ElementType::FeFuncG>());
    case ElementType::FeFuncR:
      return fn(std::integral_constant<ElementType, ElementType::FeFuncR>());
    case ElementType::FeGaussianBlur:
      return fn(std::integral_constant<ElementType, ElementType::FeGaussianBlur>());
    case ElementType::FeMerge:
      return fn(std::integral_constant<ElementType, ElementType::FeMerge>());
    case ElementType::FeMergeNode:
      return fn(std::integral_constant<ElementType, ElementType::FeMergeNode>());
    case ElementType::FeMorphology:
      return fn(std::integral_constant<ElementType, ElementType::FeMorphology>());
    case ElementType::FeOffset:
      return fn(std::integral_constant<ElementType, ElementType::FeOffset>());
    case ElementType::FeTile:
      return fn(std::integral_constant<ElementType, ElementType::FeTile>());
    case ElementType::FeTurbulence:
      return fn(std::integral_constant<ElementType, ElementType::FeTurbulence>());
    case donner::svg::ElementType::Filter:
      return fn(std::integral_constant<ElementType, ElementType::Filter>());
    case ElementType::G: return fn(std::integral_constant<ElementType, ElementType::G>());
    case ElementType::Image: return fn(std::integral_constant<ElementType, ElementType::Image>());
    case ElementType::Line: return fn(std::integral_constant<ElementType, ElementType::Line>());
    case ElementType::LinearGradient:
      return fn(std::integral_constant<ElementType, ElementType::LinearGradient>());
    case ElementType::Marker: return fn(std::integral_constant<ElementType, ElementType::Marker>());
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
    case ElementType::Symbol: return fn(std::integral_constant<ElementType, ElementType::Symbol>());
    case ElementType::Text: return fn(std::integral_constant<ElementType, ElementType::Text>());
    case ElementType::TSpan: return fn(std::integral_constant<ElementType, ElementType::TSpan>());
    case ElementType::Unknown:
      return fn(std::integral_constant<ElementType, ElementType::Unknown>());
    case ElementType::Use: return fn(std::integral_constant<ElementType, ElementType::Use>());
  }
}

}  // namespace donner::svg
