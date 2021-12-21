#pragma once

#include <entt/entt.hpp>
#include <string_view>

namespace donner {

enum class Entity : std::uint32_t {};

using Registry = entt::basic_registry<Entity>;

enum class ElementType {
  SVG,
  Path,
  Rect,
  Style,
  Unknown,
};

std::string_view TypeToString(ElementType type);

}  // namespace donner
