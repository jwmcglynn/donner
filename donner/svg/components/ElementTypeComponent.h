#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/svg/ElementType.h"

namespace donner::svg::components {

/**
 * Stores the parsed element type of the current entity.
 */
class ElementTypeComponent {
public:
  /**
   * Tag this entity to have ElementType \p type.
   *
   * @param type The type of the element.
   */
  explicit ElementTypeComponent(ElementType type) : type_(type) {}

  /// Get the effective type, reporting foreign-namespace elements as unknown.
  ElementType type() const { return inSvgNamespace_ ? type_ : ElementType::Unknown; }

  /// Retained semantic type, independent of the current namespace binding.
  ElementType parsedType() const { return type_; }

  /// Whether this element currently belongs to the SVG namespace.
  bool inSvgNamespace() const { return inSvgNamespace_; }

  /// Update namespace membership without losing specialized components or rendering behavior.
  void setInSvgNamespace(bool value) { inSvgNamespace_ = value; }

private:
  bool inSvgNamespace_ = true;
  ElementType type_;  //!< Type of the element as a parsed enum, if known (e.g. SVG, Circle, etc.)
};

/// Synthetic entities without an SVG element type remain eligible for internal styles/resources.
inline bool HasSvgNamespace(const Registry& registry, Entity entity) {
  const auto* type = registry.try_get<ElementTypeComponent>(entity);
  return !type || type->inSvgNamespace();
}

}  // namespace donner::svg::components
