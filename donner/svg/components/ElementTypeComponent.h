#pragma once
/// @file

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

  /// Get the parsed element type as an enum.
  ElementType type() const { return type_; }

private:
  ElementType type_;  //!< Type of the element as a parsed enum, if known (e.g. SVG, Circle, etc.)
};

}  // namespace donner::svg::components
