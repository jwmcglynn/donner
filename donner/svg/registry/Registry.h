#pragma once
/// @file

#include <compare>
#include <entt/entt.hpp>
#include <ostream>
#include <string_view>

namespace donner::svg {

/**
 * Entity type for the SVG \ref Registry, a `std::uint32_t` alias.
 *
 * This is a core type for the \ref ECS, and is used to identify entities in the \ref Registry.
 *
 * \see \ref Registry
 */
using Entity = entt::entity;

/// Compare two \ref Entity values.
inline auto operator<=>(Entity lhs, Entity rhs) {
  return static_cast<std::uint32_t>(lhs) <=> static_cast<std::uint32_t>(rhs);
}

/// Ostream output operator, outputs `#<id>` where `<id>` is the numeric value of the \ref Entity.
inline std::ostream& operator<<(std::ostream& os, const Entity& entity) {
  return os << "#" << static_cast<std::uint32_t>(entity);
}

/// Registry type for the SVG \ref ECS.
using Registry = entt::basic_registry<Entity, std::allocator<Entity>>;

/**
 * Convenience handle for a \ref Entity with an attached \ref Registry.
 *
 * Allows calling functions typically on \ref Registry without having to pass around two values.
 */
using EntityHandle = entt::basic_handle<Registry>;

/**
 * SVG element types, corresponds to each XML element name, such as \ref xml_circle, \ref xml_svg,
 * \ref xml_rect, etc.
 */
enum class ElementType {
  Circle,          //!< \ref xml_circle
  Defs,            //!< \ref xml_defs
  Ellipse,         //!< \ref xml_ellipse
  FeGaussianBlur,  //!< \ref xml_feGaussianBlur
  Filter,          //!< \ref xml_filter
  G,               //!< \ref xml_g
  Line,            //!< \ref xml_line
  LinearGradient,  //!< \ref xml_linearGradient
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

std::string_view TypeToString(ElementType type);

template <typename ReturnType, typename FnT>
ReturnType ToConstexpr(ElementType type, FnT fn) {
  switch (type) {
    case ElementType::Circle: return fn(std::integral_constant<ElementType, ElementType::Circle>());
    case ElementType::Defs: return fn(std::integral_constant<ElementType, ElementType::Defs>());
    case ElementType::Ellipse:
      return fn(std::integral_constant<ElementType, ElementType::Ellipse>());
    case ElementType::FeGaussianBlur:
      return fn(std::integral_constant<ElementType, ElementType::FeGaussianBlur>());
    case donner::svg::ElementType::Filter:
      return fn(std::integral_constant<ElementType, ElementType::Filter>());
    case ElementType::G: return fn(std::integral_constant<ElementType, ElementType::G>());
    case ElementType::Line: return fn(std::integral_constant<ElementType, ElementType::Line>());
    case ElementType::LinearGradient:
      return fn(std::integral_constant<ElementType, ElementType::LinearGradient>());
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