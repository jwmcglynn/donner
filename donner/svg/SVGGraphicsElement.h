#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * Base class for elements which render or impact the rendering tree, such as \ref xml_path and \ref
 * xml_g.
 *
 * This cannot be instantiated directly.
 *
 * Holds an transformation which defaults to identity.
 */
class SVGGraphicsElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  /// Inheriting constructor to be called by derived classes. \ref SVGGraphicsElement cannot be
  /// instantiated directly.
  explicit SVGGraphicsElement(EntityHandle handle);

public:
  /// Returns true if the given element type can be cast to \ref SVGTextContentElement.
  static constexpr bool IsBaseOf(ElementType type) {
    return type == ElementType::Circle || type == ElementType::Defs ||
           type == ElementType::Ellipse || type == ElementType::G || type == ElementType::Image ||
           type == ElementType::Line || type == ElementType::Path || type == ElementType::Polygon ||
           type == ElementType::Polyline || type == ElementType::Rect || type == ElementType::SVG ||
           type == ElementType::Symbol || type == ElementType::Text ||
           type == ElementType::TextPath || type == ElementType::TSpan ||
           type == ElementType::Unknown;
  }

  /// Get the 2d transformation for this element, element-from-parent.
  Transform2d transform() const;

  /// Set the 2d transformation for this element, element-from-parent. This is a no-op if the
  /// transform is identity.
  void setTransform(const Transform2d& transform);

  /// Get the absolute element-from-world transform for this element.
  Transform2d elementFromWorld() const;
};

}  // namespace donner::svg
