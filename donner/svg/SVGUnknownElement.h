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
  friend class parser::SVGParserImpl;

protected:
  /// Create an SVGUnknownElement wrapper from an entity.
  explicit SVGUnknownElement(EntityHandle handle) : SVGGraphicsElement(handle) {}

  /**
   * Internal constructor to create the element on an existing \ref Entity.
   *
   * @param handle Entity handle.
   */
  static SVGUnknownElement CreateOn(EntityHandle handle, const xml::XMLQualifiedNameRef& tagName);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::Unknown;

  static_assert(SVGGraphicsElement::IsBaseOf(Type));

  /**
   * Create a new unknown SVG element.
   *
   * @param document Containing document.
   * @param tagName XML type name.
   */
  static SVGUnknownElement Create(SVGDocument& document, const xml::XMLQualifiedNameRef& tagName) {
    return CreateOn(CreateEmptyEntity(document), tagName);
  }
};

}  // namespace donner::svg
