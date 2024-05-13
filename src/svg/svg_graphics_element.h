#pragma once
/// @file

#include "src/base/transform.h"
#include "src/svg/svg_element.h"

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
protected:
  /// Inheriting constructor to be called by derived classes. \ref SVGGraphicsElement cannot be
  /// instantiated directly.
  explicit SVGGraphicsElement(EntityHandle handle);

public:
  /// Get the 2d transformation for this element.
  Transformd transform() const;

  /// Set the 2d transformation for this element, which can be identity to make the transform a
  /// no-op.
  void setTransform(Transformd transform);

protected:
  void invalidateTransform();
  void computeTransform() const;
};

}  // namespace donner::svg
