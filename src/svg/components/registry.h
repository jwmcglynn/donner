#pragma once

#include <entt/entt.hpp>
#include <ostream>
#include <string_view>

namespace donner::svg {

enum class Entity : std::uint32_t {};

inline std::ostream& operator<<(std::ostream& os, const Entity& entity) {
  return os << "#" << static_cast<std::uint32_t>(entity);
}

using Registry = entt::basic_registry<Entity>;
using EntityHandle = entt::basic_handle<Entity>;

enum class ElementType {
  Circle,
  Defs,
  Ellipse,
  G,
  Line,
  Path,
  Polygon,
  Polyline,
  Rect,
  Style,
  SVG,      //!< SVG root element.
  Unknown,  //!< For unknown elements.
  Use,
};

std::string_view TypeToString(ElementType type);

template <typename ReturnType, typename FnT>
ReturnType ToConstexpr(ElementType type, FnT fn) {
  switch (type) {
    case ElementType::Circle: return fn(std::integral_constant<ElementType, ElementType::Circle>());
    case ElementType::Defs: return fn(std::integral_constant<ElementType, ElementType::Defs>());
    case ElementType::Ellipse:
      return fn(std::integral_constant<ElementType, ElementType::Ellipse>());
    case ElementType::G: return fn(std::integral_constant<ElementType, ElementType::G>());
    case ElementType::Line: return fn(std::integral_constant<ElementType, ElementType::Line>());
    case ElementType::Path: return fn(std::integral_constant<ElementType, ElementType::Path>());
    case ElementType::Polygon:
      return fn(std::integral_constant<ElementType, ElementType::Polygon>());
    case ElementType::Polyline:
      return fn(std::integral_constant<ElementType, ElementType::Polyline>());
    case ElementType::Rect: return fn(std::integral_constant<ElementType, ElementType::Rect>());
    case ElementType::Style: return fn(std::integral_constant<ElementType, ElementType::Style>());
    case ElementType::SVG: return fn(std::integral_constant<ElementType, ElementType::SVG>());
    case ElementType::Unknown:
      return fn(std::integral_constant<ElementType, ElementType::Unknown>());
    case ElementType::Use: return fn(std::integral_constant<ElementType, ElementType::Use>());
  };
}

}  // namespace donner::svg
