#pragma once
/// @file

#include "donner/svg/SVGGraphicsElement.h"

namespace donner::svg {

/**
 * DOM object for an unknown SVG element.
 *
 * This is a placeholder for any SVG element that is not explicitly supported by Donner.
 */
class SVGUnknownElement : public SVGGraphicsElement {
protected:
  /// Create an SVGUnknownElement wrapper from an entity.
  explicit SVGUnknownElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Unknown;

  /**
   * Create a new unknown SVG element.
   *
   * @param document Containing document.
   * @param xmlTypeName XML type name.
   */
  static SVGUnknownElement Create(SVGDocument& document, const XMLQualifiedNameRef& xmlTypeName);
};

}  // namespace donner::svg
