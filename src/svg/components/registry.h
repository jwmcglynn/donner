#pragma once

#include <entt/entt.hpp>

namespace donner {

ENTT_OPAQUE_TYPE(Entity, std::uint32_t);

using Registry = entt::basic_registry<Entity>;

enum class ElementType {
  SVG,
  Path,
  Rect,
  Unknown,
};

}  // namespace donner
