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
  Path,
  Rect,
  Style,
  SVG,      //!< SVG root element.
  Unknown,  //!< For unknown elements.
  Use,
};

std::string_view TypeToString(ElementType type);

template <typename ComponentT>
struct HandleOfMixin {
protected:
  Entity entityOf(Registry& registry) const {
    return entt::to_entity(registry, static_cast<const ComponentT&>(*this));
  }

  EntityHandle handleOf(Registry& registry) const {
    return EntityHandle(registry, entityOf(registry));
  }
};

}  // namespace donner::svg
